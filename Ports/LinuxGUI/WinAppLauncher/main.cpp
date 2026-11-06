/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-winapp - native Windows-application launcher.
//
// This replaces the old shell script of the same name. The launch *policy*
// (app vs game, which runtime, environment, prefix prep) now lives here in
// native code, the same way omikuji's `assemble_launch` + `WineVariant` do it,
// instead of being delegated to /bin/sh. Only optional user pre/post-launch
// hooks would belong in a script; this tool has none.
//
//   serenity-winapp [--game|--app|--mode <auto|app|game>] [--prefix <dir>]
//                   [--dry-run] <exe> [args...]
//
// Runtimes:
//   game -> <prefix>/bin/umu-run + <prefix>/opt/dwproton
//           (Steam Runtime + Proton + protonfixes + DXVK/VKD3D). This is the
//           only path that gets the full Proton stack; games rely on it.
//   app  -> upstream (vanilla) wine. Proton is a gaming fork: diverged from
//           upstream, ships DLL overrides, expects the Steam Runtime. For
//           office/utility/installer apps that is more surface to break. We
//           prefer a real upstream wine and only fall back to DWProton's wine
//           when none is installed.
//
// "Is it a game?" is a heuristic because an .exe does not say so: we look for
// D3D/DXGI/XInput/Vulkan DLL names in the PE image and for engine markers next
// to the exe (UnityPlayer.dll, steam_api*, *.pak ...). --game/--app/--mode
// override it.

#include <cerrno>
#include <cctype>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string g_install_prefix;

bool file_exists(std::string const& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_executable(std::string const& path)
{
    return ::access(path.c_str(), X_OK) == 0;
}

std::string dir_name(std::string const& path)
{
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string self_exe_path()
{
    char buffer[PATH_MAX];
    auto n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0)
        return {};
    buffer[n] = '\0';
    return buffer;
}

std::string find_in_path(std::string const& name)
{
    auto* path = ::getenv("PATH");
    if (!path)
        return {};
    std::string dirs(path);
    size_t start = 0;
    while (start <= dirs.size()) {
        auto end = dirs.find(':', start);
        auto dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            auto candidate = dir + "/" + name;
            if (is_executable(candidate))
                return candidate;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {};
}

void setenv_if_unset(char const* name, std::string const& value)
{
    if (!::getenv(name))
        ::setenv(name, value.c_str(), 1);
}

// DWProton keeps its vkd3d runtime next to (not inside) the wine DLL dir, and a
// bare wine does not search it, so dxgi/wined3d fail with STATUS_DLL_NOT_FOUND.
// Only the DWProton/Proton runtime needs this; an upstream wine must not see
// Proton's vkd3d DLLs (ABI mismatch).
void apply_proton_dll_path()
{
    auto base = g_install_prefix + "/opt/dwproton/files/lib/vkd3d";
    auto value = base + "/x86_64-windows:" + base + "/i386-windows";
    if (auto* existing = ::getenv("WINEDLLPATH"); existing && *existing) {
        value += ":";
        value += existing;
    }
    ::setenv("WINEDLLPATH", value.c_str(), 1);
}

// Run `exe args...` in a child, discarding its output, and wait. Used for the
// one-time CJK font repair; failures are non-fatal.
void run_quiet(std::string const& exe, std::vector<std::string> const& args)
{
    std::vector<char*> av;
    av.push_back(const_cast<char*>(exe.c_str()));
    for (auto const& a : args)
        av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        ::execvp(av[0], av.data());
        ::_exit(127);
    }
    if (pid < 0)
        return;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
}

[[noreturn]] void exec_replacing(std::string const& exe, std::vector<std::string> const& args)
{
    std::vector<char*> av;
    av.push_back(const_cast<char*>(exe.c_str()));
    for (auto const& a : args)
        av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);

    ::execvp(av[0], av.data());
    std::fprintf(stderr, "serenity-winapp: failed to exec '%s': %s\n", exe.c_str(), std::strerror(errno));
    std::exit(127);
}

// Scan the PE image (bounded) for DLL names only games pull in. Games can be
// huge, so cap the read; the import table is normally within the first few MB
// and big-data files (paks) sit beside the exe, not inside it.
bool exe_imports_game_dlls(std::string const& exe)
{
    // Needles are already lowercase. We lowercase the window once then use
    // find(), which keeps a 64 MiB scan fast instead of an O(n*m) per-byte
    // comparison.
    static char const* needles[] = {
        "d3d10.dll", "d3d11.dll", "d3d12.dll", "d3d9.dll", "d3d8.dll",
        "dxgi.dll", "xinput", "dinput8.dll", "vulkan-1.dll", "openal32.dll"
    };
    FILE* f = std::fopen(exe.c_str(), "rb");
    if (!f)
        return false;

    constexpr size_t chunk = 1 << 20; // 1 MiB
    constexpr size_t cap = 32ull << 20; // 32 MiB
    constexpr size_t overlap = 32;
    std::vector<char> buffer(chunk + overlap);
    size_t total = 0;
    size_t carried = 0;
    bool found = false;
    while (total < cap && !found) {
        size_t got = std::fread(buffer.data() + carried, 1, chunk, f);
        if (got == 0)
            break;
        size_t len = carried + got;
        for (size_t k = 0; k < len; ++k) {
            auto c = static_cast<unsigned char>(buffer[k]);
            if (c >= 'A' && c <= 'Z')
                buffer[k] = static_cast<char>(c - 'A' + 'a');
        }
        std::string window(buffer.data(), len);
        for (auto const* needle : needles) {
            if (window.find(needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        carried = len < overlap ? len : overlap;
        std::memcpy(buffer.data(), buffer.data() + (len - carried), carried);
        total += got;
    }
    std::fclose(f);
    return found;
}

bool dir_has_game_marker(std::string const& dir)
{
    static char const* markers[] = {
        "UnityPlayer.dll", "steam_api.dll", "steam_api64.dll",
        "d3d11.dll", "dxgi.dll", "d3d9.dll"
    };
    for (auto const* marker : markers) {
        if (file_exists(dir + "/" + marker))
            return true;
    }

    static char const* extensions[] = { ".pak", ".uasset", ".pck", ".rpa", ".assets" };
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return false;
    bool found = false;
    while (auto* entry = ::readdir(d)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        for (auto const* ext : extensions) {
            size_t n = std::strlen(ext);
            if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) {
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    ::closedir(d);
    return found;
}

bool looks_like_game(std::string const& exe)
{
    if (exe_imports_game_dlls(exe))
        return true;
    return dir_has_game_marker(dir_name(exe));
}

void cjk_font_repair(std::string const& wineprefix)
{
    if (!file_exists(wineprefix + "/system.reg"))
        return;
    if (file_exists(wineprefix + "/.serenity-cjk-font"))
        return;
    auto tool = g_install_prefix + "/bin/serenity-wine-fonts";
    if (!is_executable(tool))
        return;
    run_quiet(tool, { "--prefix", wineprefix });
}

void prefer_utf8_locale()
{
    // Chinese apps set WM_NAME with the UTF-8 atom; under LANG=C.UTF-8 Xlib
    // reports an ASCII codeset and titles/text come out as garbage.
    if (::setlocale(LC_ALL, "zh_CN.UTF-8")) {
        ::setenv("LANG", "zh_CN.UTF-8", 1);
        ::setenv("LC_ALL", "zh_CN.UTF-8", 1);
    }
}

struct ManagedProgram {
    std::string mode;
    std::string prefix;
};

std::string trim_whitespace(std::string value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

// Registry location, mirroring WinApps::registry_path() (LibCore's
// StandardPaths honours XDG_CONFIG_HOME). Reading $HOME/.config directly made
// the launcher blind to the registry whenever the session sets XDG_CONFIG_HOME,
// so a managed program would silently fall back to mode=auto.
std::string registry_path()
{
    if (auto* xdg = ::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/serenity/winapps.ini";
    auto* home = ::getenv("HOME");
    if (!home || !*home)
        return {};
    return std::string(home) + "/.config/serenity/winapps.ini";
}

// Look `exe` up in the registry written by Windows 程序管理. Returning the
// stored mode/prefix lets every entry point - file manager, desktop, start
// menu - launch a managed program the same way.
ManagedProgram lookup_managed_program(std::string const& exe)
{
    auto path = registry_path();
    if (path.empty())
        return {};

    std::ifstream in(path);
    if (!in)
        return {};

    ManagedProgram found;
    std::string exe_value, mode_value, prefix_value;

    auto finish_group = [&] {
        if (found.mode.empty() && exe_value == exe) {
            found.mode = mode_value.empty() ? "auto" : mode_value;
            found.prefix = prefix_value;
        }
        exe_value.clear();
        mode_value.clear();
        prefix_value.clear();
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.front() == '[') {
            finish_group();
            continue;
        }
        auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        auto key = trim_whitespace(line.substr(0, equals));
        auto value = trim_whitespace(line.substr(equals + 1));
        if (key == "Exe")
            exe_value = value;
        else if (key == "Mode")
            mode_value = value;
        else if (key == "Prefix")
            prefix_value = value;
    }
    finish_group();
    return found;
}

void usage()
{
    std::fprintf(stderr,
        "usage: serenity-winapp [--game|--app|--mode <auto|app|game>] "
        "[--prefix <dir>] [--dry-run] <exe> [args...]\n");
}

} // namespace

int main(int argc, char** argv)
{
    // Resolve the install prefix: env override, else two levels up from this
    // binary (…/bin/serenity-winapp -> …).
    if (auto* prefix = ::getenv("SERENITY_PREFIX"); prefix && *prefix) {
        g_install_prefix = prefix;
    } else {
        auto self = self_exe_path();
        g_install_prefix = self.empty() ? "/serenity" : dir_name(dir_name(self));
    }

    std::string mode = "auto";
    bool mode_explicit = false;
    if (auto* env_mode = ::getenv("SERENITY_WINAPP_MODE"); env_mode && *env_mode) {
        mode = env_mode;
        mode_explicit = true;
    }

    std::string prefix_arg;
    bool dry_run = false;

    std::vector<std::string> rest;
    int i = 1;
    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--game") {
            mode = "game";
            mode_explicit = true;
        } else if (arg == "--app") {
            mode = "app";
            mode_explicit = true;
        } else if (arg == "--mode") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            mode = argv[++i];
            mode_explicit = true;
        } else if (arg == "--prefix") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            prefix_arg = argv[++i];
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--") {
            ++i;
            break;
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::fprintf(stderr, "serenity-winapp: unknown option '%s'\n", arg.c_str());
            usage();
            return 2;
        } else {
            break;
        }
    }
    for (; i < argc; ++i)
        rest.emplace_back(argv[i]);

    if (mode != "auto" && mode != "app" && mode != "game") {
        std::fprintf(stderr, "serenity-winapp: invalid --mode '%s' (expected auto|app|game)\n", mode.c_str());
        return 2;
    }
    if (rest.empty()) {
        usage();
        return 2;
    }

    std::string target = rest.front();
    std::vector<std::string> target_args(rest.begin() + 1, rest.end());

    // If the caller did not force a mode/prefix (e.g. opening the .exe from the
    // file manager), honour the mode stored by Windows 程序管理 instead of
    // guessing. Without this a Mode=game title opened from the file manager
    // would be detected as an "app" and run with plain wine.
    if (!mode_explicit && prefix_arg.empty()) {
        if (auto managed = lookup_managed_program(target); !managed.mode.empty()) {
            mode = managed.mode;
            if (!managed.prefix.empty())
                prefix_arg = managed.prefix;
        }
    }
    if (mode != "auto" && mode != "app" && mode != "game")
        mode = "auto";

    // Session-wide environment, matching the old launcher.
    setenv_if_unset("DISPLAY", ":100");
    setenv_if_unset("WINEDEBUG", "-all");
    setenv_if_unset("WINEESYNC", "1");
    setenv_if_unset("WINEFSYNC", "1");
    {
        // Make the upstream wine reachable for the CJK repair and the app path.
        auto* path = ::getenv("PATH");
        std::string new_path = "/opt/wine-stable/bin";
        if (path && *path) {
            new_path += ":";
            new_path += path;
        }
        ::setenv("PATH", new_path.c_str(), 1);
    }

    // Per-program prefix (Ofuda-style isolation). Unmanaged programs use one
    // shared prefix under $HOME; /root is unreadable when the session runs as a
    // normal user (e.g. serenity), so it must never be the default there.
    std::string wineprefix;
    if (!prefix_arg.empty()) {
        wineprefix = prefix_arg;
    } else if (auto* inherited = ::getenv("WINEPREFIX"); inherited && *inherited) {
        wineprefix = inherited;
    } else {
        auto* home = ::getenv("HOME");
        wineprefix = (home && *home) ? std::string(home) + "/.wine-dw" : "/root/.wine-dw";
    }
    ::setenv("WINEPREFIX", wineprefix.c_str(), 1);

    // Windows programs resolve assets and write saves relative to the .exe
    // directory. Launches from the panel / desktop used to get that as their cwd
    // (Core::Process::spawn) while wrapper / start-menu launches inherited the
    // shell's cwd, so the same program behaved differently depending on where it
    // was started. Normalise it here, for every entry point.
    if (target.find('/') != std::string::npos)
        (void)::chdir(dir_name(target).c_str());

    prefer_utf8_locale();

    if (mode == "auto")
        mode = looks_like_game(target) ? "game" : "app";

    if (dry_run) {
        std::printf("%s -> %s\n", target.c_str(), mode.c_str());
        return 0;
    }

    // One-time CJK font repair for this prefix. Kept after the dry-run exit so
    // that inspecting a program never mutates its prefix.
    cjk_font_repair(wineprefix);

    std::vector<std::string> command;

    if (mode == "game") {
        auto umu_run = g_install_prefix + "/bin/umu-run";
        if (is_executable(umu_run)) {
            // GAMEID lets umu/protonfixes find the title's fix; unknown -> 0.
            // PROTONPATH points at the bundled DWProton.
            setenv_if_unset("GAMEID", "0");
            ::setenv("UMU_ALLOW_ROOT", "1", 1);
            auto proton = g_install_prefix + "/opt/dwproton";
            ::setenv("PROTONPATH", proton.c_str(), 1);
            apply_proton_dll_path();
            command.push_back(umu_run);
            command.push_back(target);
            for (auto const& a : target_args)
                command.push_back(a);
            exec_replacing(command.front(), std::vector<std::string>(command.begin() + 1, command.end()));
        }
        // No umu-run: fall through to DWProton's wine below.
    }

    if (mode == "app") {
        std::vector<std::string> candidates;
        if (auto found = find_in_path("wine"); !found.empty())
            candidates.push_back(found);
        candidates.push_back("/opt/wine-stable/bin/wine");
        candidates.push_back("/usr/local/bin/wine");
        candidates.push_back("/usr/bin/wine");
        for (auto const& candidate : candidates) {
            if (!candidate.empty() && is_executable(candidate)) {
                // Vanilla wine: never point it at Proton's vkd3d DLLs.
                command.push_back(candidate);
                command.push_back(target);
                for (auto const& a : target_args)
                    command.push_back(a);
                exec_replacing(command.front(), std::vector<std::string>(command.begin() + 1, command.end()));
            }
        }
        // No vanilla wine: fall through to DWProton's wine below.
    }

    // Fallback: DWProton's wine (game without umu-run, or app with no vanilla wine).
    auto proton_wine = g_install_prefix + "/opt/dwproton/files/bin/wine";
    if (is_executable(proton_wine)) {
        apply_proton_dll_path();
        command.push_back(proton_wine);
        command.push_back(target);
        for (auto const& a : target_args)
            command.push_back(a);
        exec_replacing(command.front(), std::vector<std::string>(command.begin() + 1, command.end()));
    }

    std::fprintf(stderr, "serenity-winapp: neither a vanilla wine nor DWProton is installed\n");
    return 127;
}
