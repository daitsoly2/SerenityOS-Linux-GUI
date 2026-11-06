/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Windows program manager - shared registry + launcher helpers.
//
// Modeled on omikuji's management scheme:
//   - one registry of installed programs (persisted as an ini file), each entry
//     recording the exe, a per-program isolated WINEPREFIX (Ofuda-style, never a
//     shared one), and the launch mode (auto / app / game);
//   - a prefix manager that creates a `prefixes/{slug}` directory per program;
//   - a runner abstraction: every installed program gets a tiny wrapper script
//     `$data/winapps/wrappers/{slug}` that execs `serenity-winapp --prefix ...
//     --mode ... <exe>`. The wrapper is the launch primitive shared by the
//     desktop shortcut, the start-menu entry and the manager panel, so a program
//     always runs in its own prefix no matter where it is launched from.
//
// This header is header-only on purpose: both the desktop shell
// (serenity-shell) and the manager panel (WinAppsSettings) need it, and the
// Common directory is already header-only in this port.

#pragma once

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Window.h>
#include <Ports/LinuxGUI/Common/hostify.h>

namespace WinApps {

// One installed Windows program.
struct Program {
    int id { 0 };
    ByteString slug;   // unique on-disk token (used for prefix / wrapper / shortcuts)
    ByteString name;   // display name
    ByteString exe;    // absolute path to the .exe
    ByteString prefix; // absolute path to this program's isolated WINEPREFIX
    ByteString mode;   // "auto" | "app" | "game"
};

// --- Paths ----------------------------------------------------------------

inline ByteString config_dir()
{
    auto d = Core::StandardPaths::config_directory();
    if (d.is_empty())
        d = ByteString::formatted("{}/.config", Core::StandardPaths::home_directory());
    return d;
}

inline ByteString registry_path()
{
    return ByteString::formatted("{}/serenity/winapps.ini", config_dir());
}

inline ByteString data_root()
{
    auto d = Core::StandardPaths::data_directory();
    if (d.is_empty())
        d = ByteString::formatted("{}/.local/share", Core::StandardPaths::home_directory());
    return ByteString::formatted("{}/serenity/winapps", d);
}

inline ByteString prefixes_dir() { return ByteString::formatted("{}/prefixes", data_root()); }
inline ByteString wrappers_dir() { return ByteString::formatted("{}/wrappers", data_root()); }
inline ByteString user_apps_dir() { return ByteString::formatted("{}/apps", data_root()); }

inline ByteString desktop_dir()
{
    auto d = Core::StandardPaths::desktop_directory();
    if (d.is_empty())
        d = ByteString::formatted("{}/Desktop", Core::StandardPaths::home_directory());
    return d;
}

inline ErrorOr<void> ensure_dirs()
{
    for (auto const& d : Vector<ByteString> { data_root(), prefixes_dir(), wrappers_dir(), user_apps_dir(), desktop_dir() })
        TRY(Core::Directory::create(d, Core::Directory::CreateDirectories::Yes));
    return {};
}

// "Notepad++.exe" -> "notepad", "Foo Bar 2" -> "foo-bar-2".
inline ByteString slugify(StringView name)
{
    StringBuilder sb;
    for (auto ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            sb.append(ch);
        else if (ch >= 'A' && ch <= 'Z')
            sb.append(static_cast<char>(ch - 'A' + 'a'));
        else
            sb.append('-');
    }
    auto s = sb.to_byte_string();
    if (s.is_empty())
        s = "app"sv;
    return s;
}

// --- Registry (ini) -------------------------------------------------------

inline Vector<Program> list_programs()
{
    Vector<Program> out;
    auto cfg = Core::ConfigFile::open(registry_path());
    if (cfg.is_error())
        return out;
    for (auto const& group : cfg.value()->groups()) {
        if (!group.starts_with("App-"sv))
            continue;
        Program p;
        p.id = cfg.value()->read_entry(group, "Id").to_number<int>().value_or(0);
        p.slug = cfg.value()->read_entry(group, "Slug");
        p.name = cfg.value()->read_entry(group, "Name");
        p.exe = cfg.value()->read_entry(group, "Exe");
        p.prefix = cfg.value()->read_entry(group, "Prefix");
        p.mode = cfg.value()->read_entry(group, "Mode", "auto");
        if (!p.name.is_empty())
            out.append(move(p));
    }
    // ConfigFile hands groups back in file order, which changes when an entry
    // is removed and re-added. Sort by id so the manager list, the start menu
    // and the desktop never reshuffle between runs.
    quick_sort(out, [](auto const& a, auto const& b) { return a.id < b.id; });
    return out;
}

inline Optional<Program> find_by_id(int id)
{
    for (auto& p : list_programs())
        if (p.id == id)
            return p;
    return {};
}

inline Optional<Program> find_by_exe(StringView exe)
{
    for (auto& p : list_programs())
        if (p.exe == exe)
            return p;
    return {};
}

inline ErrorOr<Program> add_program(StringView name, StringView exe, StringView mode)
{
    TRY(ensure_dirs());
    // Same .exe twice would mean two isolated prefixes, two desktop icons and
    // two menu entries fighting over one program. Reject it here as well as in
    // the UI (the UI can name the existing entry, this is the last line of
    // defence for other callers).
    for (auto const& existing : list_programs()) {
        if (existing.exe == exe)
            return Error::from_string_literal("this program is already managed");
    }
    auto cfg = TRY(Core::ConfigFile::open(registry_path(), Core::ConfigFile::AllowWriting::Yes));

    int max_id = 0;
    for (auto const& group : cfg->groups()) {
        if (!group.starts_with("App-"sv))
            continue;
        max_id = max(max_id, cfg->read_entry(group, "Id").to_number<int>().value_or(0));
    }
    int id = max_id + 1;
    Program p;
    p.id = id;
    p.slug = ByteString::formatted("{}-{}", slugify(name), id);
    p.name = name;
    p.exe = exe;
    p.mode = mode;
    p.prefix = ByteString::formatted("{}/{}", prefixes_dir(), p.slug);

    auto group = ByteString::formatted("App-{}", id);
    cfg->write_entry(group, "Id", ByteString::number(id));
    cfg->write_entry(group, "Slug", p.slug);
    cfg->write_entry(group, "Name", p.name);
    cfg->write_entry(group, "Exe", p.exe);
    cfg->write_entry(group, "Prefix", p.prefix);
    cfg->write_entry(group, "Mode", p.mode);
    TRY(cfg->sync());
    return p;
}

inline ErrorOr<void> remove_program(int id)
{
    auto cfg = TRY(Core::ConfigFile::open(registry_path(), Core::ConfigFile::AllowWriting::Yes));
    cfg->remove_group(ByteString::formatted("App-{}", id));
    TRY(cfg->sync());
    return {};
}

inline bool is_valid_mode(StringView mode)
{
    return mode == "auto"sv || mode == "app"sv || mode == "game"sv;
}

inline ErrorOr<void> set_mode(int id, StringView mode)
{
    // A bogus token would be baked into the wrapper's `--mode` argument and the
    // launcher would then reject it at exec time (exit 2), i.e. the program
    // would stop launching entirely.
    if (!is_valid_mode(mode))
        return Error::from_string_literal("invalid launch mode");
    auto cfg = TRY(Core::ConfigFile::open(registry_path(), Core::ConfigFile::AllowWriting::Yes));
    cfg->write_entry(ByteString::formatted("App-{}", id), "Mode", mode);
    TRY(cfg->sync());
    return {};
}

// --- Derived artifact paths ----------------------------------------------

inline ByteString wrapper_path(Program const& p) { return ByteString::formatted("{}/{}", wrappers_dir(), p.slug); }
inline ByteString desktop_shortcut_path(Program const& p) { return ByteString::formatted("{}/{}.desktop", desktop_dir(), p.slug); }
inline ByteString menu_entry_path(Program const& p) { return ByteString::formatted("{}/{}.af", user_apps_dir(), p.slug); }

// The wrapper execs the compatibility launcher with this program's prefix and
// mode baked in. Absolute paths throughout so it works no matter who launches
// it (desktop shortcut, start menu, panel) and regardless of $PATH.
inline ErrorOr<void> write_wrapper(Program const& p)
{
    TRY(ensure_dirs());
    auto winapp = ByteString::formatted("{}/bin/serenity-winapp", PortHostify::install_prefix());
    auto content = ByteString::formatted(
        "#!/bin/sh\n"
        "# Auto-generated by Windows 程序管理. Do not edit by hand.\n"
        "exec '{}' --prefix '{}' --mode {} '{}'\n",
        winapp, p.prefix, p.mode, p.exe);
    auto path = wrapper_path(p);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(content.bytes()));
    file->close();
    TRY(Core::System::chmod(path, 0755));
    return {};
}

inline ErrorOr<void> write_desktop_shortcut(Program const& p)
{
    TRY(ensure_dirs());
    // No Path= here on purpose. This desktop's .desktop parser treats Path= as
    // "the file this shortcut points at" (right-click -> Compress), not as the
    // child's working directory. The exe directory is set as cwd by
    // serenity-winapp itself, which covers the desktop, start menu and panel
    // entry points alike.
    auto content = ByteString::formatted(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name={}\n"
        "Exec={}\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Comment=Managed by Windows 程序管理\n",
        p.name, wrapper_path(p));
    auto path = desktop_shortcut_path(p);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(content.bytes()));
    file->close();
    return {};
}

// Start-menu category for managed Windows programs. They get their own
// "Windows 应用" submenu instead of being mixed into Games. The category string
// is an exact key: keep the '&' mnemonic marker (the menu strips it), and mirror
// it in SystemMenu.ini so the submenu gets an icon.
inline ByteString menu_category() { return "&Windows 应用"; }

// A start-menu entry under the Windows category. AppCatalog scans the user apps
// dir (see AppCatalog.cpp) and groups it into that submenu automatically.
inline ErrorOr<void> write_menu_entry(Program const& p)
{
    TRY(ensure_dirs());
    auto content = ByteString::formatted(
        "[App]\n"
        "Name={}\n"
        "Executable={}\n"
        "Category={}\n",
        p.name, wrapper_path(p), menu_category());
    auto path = menu_entry_path(p);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(content.bytes()));
    file->close();
    return {};
}

// Older installs filed their start-menu entries under Games. Rewrite them to the
// Windows category so existing programs move without reinstalling.
inline void migrate_menu_category()
{
    for (auto const& p : list_programs()) {
        auto path = menu_entry_path(p);
        if (!FileSystem::exists(path))
            continue;
        auto cfg = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
        if (cfg.is_error())
            continue;
        auto& file = *cfg.value();
        if (file.read_entry("App", "Category") == menu_category())
            continue;
        file.write_entry("App", "Category", menu_category());
        (void)file.sync();
    }
}

inline ErrorOr<void> write_artifacts(Program const& p)
{
    TRY(write_wrapper(p));
    TRY(write_desktop_shortcut(p));
    TRY(write_menu_entry(p));
    return {};
}

inline ErrorOr<void> remove_artifacts(Program const& p)
{
    // Report failures instead of dropping them: a leftover .desktop would keep
    // launching a program that no longer exists in the registry.
    Optional<Error> first_error;
    for (auto const& path : Vector<ByteString> { wrapper_path(p), desktop_shortcut_path(p), menu_entry_path(p) }) {
        if (!FileSystem::exists(path))
            continue;
        if (auto result = FileSystem::remove(path, FileSystem::RecursionMode::Disallowed); result.is_error() && !first_error.has_value())
            first_error = result.release_error();
    }
    if (first_error.has_value())
        return first_error.release_value();
    return {};
}

inline ErrorOr<void> remove_prefix(Program const& p)
{
    if (FileSystem::exists(p.prefix))
        TRY(FileSystem::remove(p.prefix, FileSystem::RecursionMode::Allowed));
    return {};
}

// --- Launch ---------------------------------------------------------------

inline ErrorOr<pid_t> spawn_program(Program const& p)
{
    auto winapp = ByteString::formatted("{}/bin/serenity-winapp", PortHostify::install_prefix());
    Vector<ByteString> arguments;
    arguments.append("--prefix"sv);
    arguments.append(p.prefix);
    arguments.append("--mode"sv);
    arguments.append(p.mode);
    arguments.append(p.exe);
    return Core::Process::spawn(winapp, ReadonlySpan<ByteString> { arguments.data(), arguments.size() }, LexicalPath::dirname(p.exe));
}

inline void launch_program(Program const& p, GUI::Window* parent)
{
    if (auto result = spawn_program(p); result.is_error())
        GUI::MessageBox::show_error(parent, ByteString::formatted("启动失败: {}", result.error()));
}

}
