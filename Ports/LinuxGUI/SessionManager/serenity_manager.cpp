/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenitid - session supervisor for the SerenityOS Linux GUI port.
//
// Instead of teaching systemd about six different binaries, Debian only starts
// this one daemon. It owns the whole session:
//
//   boot order   ConfigServer -> LaunchServer -> WindowServer -> login -> Taskbar
//   supervision  restart crashed children with backoff, log everything
//   lifecycle    SIGTERM shuts the children down in reverse order
//   progress     writes /run/serenity-progress for the boot splash
//
// Config: /etc/serenity/session.conf
//   [Session]
//   Autologin=serenity        ; passed to the login server
//   Language=zh-CN            ; UI language
//   Restart=yes               ; restart crashed children (default yes)

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DateTime.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibMain/Main.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace AK;

namespace {

// These default to the classic root-session locations but can be overridden via
// environment so a non-root session (User=serenity in the unit) can write its
// own log/progress without needing root. The splash (runs as root) reads the
// progress file, so that one must land somewhere both can reach (RuntimeDirectory).
ByteString g_log_path { "/var/log/serenity-session.log" };
ByteString g_progress_path { "/run/serenity-progress" };
ByteString g_config_path { "/etc/serenity/session.conf" };

enum class Stage {
    Boot,      // bringing up the graphics stack
    Login,     // waiting for the user
    Desktop,   // Taskbar is up
    ShuttingDown,
};

struct Child {
    ByteString name;
    ByteString path;
    Vector<ByteString> arguments;
    pid_t pid { -1 };
    bool essential { true };
    bool restart_on_exit { false };
    // Shell components exit cleanly when the compositor goes away, so a zero
    // exit status must not be read as "the user closed it".
    bool always_restart { false };
    int restart_count { 0 };
    int last_status { 0 };
};

FILE* g_log { nullptr };
Vector<NonnullOwnPtr<Child>> g_children;
Stage g_stage { Stage::Boot };
ByteString g_bin_dir;
bool g_restart_enabled { true };
bool g_shutting_down { false };
// "Log out" (SIGUSR1) does not try to un-start half the graph in place: the
// supervisor tears the children down and re-execs itself, so the next session
// walks the same boot sequence, login stage included.
volatile sig_atomic_t g_logout_pending { 0 };
// Core::Timer hands back a RefPtr: a local one dies at the end of the statement
// and the restart would silently never happen.
Vector<NonnullRefPtr<Core::Timer>> g_pending_restarts;
volatile sig_atomic_t g_cad_pending { 0 };

void log_line(StringView message)
{
    auto timestamp = Core::DateTime::now().to_byte_string("%Y-%m-%d %H:%M:%S"sv);
    auto line = ByteString::formatted("[{}] {}", timestamp, message);
    if (g_log) {
        fprintf(g_log, "%s\n", line.characters());
        fflush(g_log);
    }
    if (getenv("SERENITY_DEBUG"))
        warnln("{}", line);
}

// 写入 "百分比 阶段名"：启动画面把阶段名画在进度条上方。
// 阶段名可选（空就只写数字）。
void write_progress(int percent, StringView stage = {})
{
    auto* file = fopen(g_progress_path.characters(), "w");
    if (!file)
        return;
    if (stage.is_empty())
        fprintf(file, "%d\n", percent);
    else
        fprintf(file, "%d %.*s\n", percent, static_cast<int>(stage.length()), stage.characters_without_null_termination());
    fclose(file);
}

Child* find_child(StringView name)
{
    for (auto& child : g_children) {
        if (child->name == name)
            return child.ptr();
    }
    return nullptr;
}

ErrorOr<void> spawn(Child& child)
{
    pid_t pid = fork();
    if (pid < 0)
        return Error::from_errno(errno);
    if (pid == 0) {
        setsid();
        // Children inherit the session log; the console stays clean.
        if (g_log) {
            dup2(fileno(g_log), STDOUT_FILENO);
            dup2(fileno(g_log), STDERR_FILENO);
        }
        Vector<char const*> argv;
        argv.append(child.path.characters());
        for (auto& argument : child.arguments)
            argv.append(argument.characters());
        argv.append(nullptr);

        setenv("SERENITY_RESOURCE_ROOT", "/res", 0);
        auto lib_dir = ByteString::formatted("{}/../lib", g_bin_dir);
        setenv("LD_LIBRARY_PATH", lib_dir.characters(), 1);
        execv(child.path.characters(), const_cast<char**>(argv.data()));
        _exit(127);
    }
    child.pid = pid;
    log_line(ByteString::formatted("started {} (pid {})", child.name, pid));
    return {};
}

void stop_child(Child& child)
{
    if (child.pid <= 0)
        return;
    log_line(ByteString::formatted("stopping {} (pid {})", child.name, child.pid));
    kill(child.pid, SIGTERM);
    // Give it a moment, then be firm.
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        if (waitpid(child.pid, &status, WNOHANG) == child.pid) {
            child.pid = -1;
            return;
        }
        usleep(50 * 1000);
    }
    kill(child.pid, SIGKILL);
    waitpid(child.pid, nullptr, 0);
    child.pid = -1;
}

bool socket_ready(ByteString const& path)
{
    struct stat st { };
    return stat(path.characters(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// stat() alone cannot distinguish a live listener from a stale socket pathname
// left behind by a crashed/wedged compositor. A non-blocking-ish connect()
// (no payload, closed immediately) proves somebody is still accepting.
// Only used by the CAD health check, never by the boot-time wait loops.
bool socket_accepting(ByteString const& path)
{
    if (!socket_ready(path))
        return false;
    if (path.length() >= sizeof(sockaddr_un::sun_path))
        return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.characters(), path.length());
    int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close(fd);
    return result == 0;
}

void start_desktop()
{
    write_progress(100, "STARTING DESKTOP"sv);

    // The shell owns both the desktop window (wallpaper + icons) and the
    // taskbar window, so there is a single process to start here.
    auto* shell = find_child("shell"sv);
    if (!shell)
        return;

    if (shell->pid <= 0 && spawn(*shell).is_error())
        log_line("failed to start shell"sv);

    // Applets register with the WindowServer and get placed in the shell's
    // applet area, so they start after it.
    for (auto name : { "volume"sv, "resource-graph"sv, "network"sv, "clipboard-history"sv }) {
        if (auto* applet = find_child(name); applet && applet->pid <= 0)
            (void)spawn(*applet);
    }
    g_stage = Stage::Desktop;
}

void on_login_finished(int status)
{
    if (g_stage != Stage::Login)
        return;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        log_line("login succeeded, starting desktop"sv);
        start_desktop();
    } else {
        log_line(ByteString::formatted("login exited with status {}", status));
        // Keep the session alive; restart the login prompt.
        auto* login = find_child("login"sv);
        if (login && !g_shutting_down) {
            login->pid = -1;
            if (spawn(*login).is_error())
                log_line("failed to restart login server"sv);
        }
    }
}

void reap_children()
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            return;

        Child* child = nullptr;
        for (auto& candidate : g_children) {
            if (candidate->pid == pid) {
                child = candidate.ptr();
                break;
            }
        }
        if (!child)
            continue;

        child->last_status = status;
        child->pid = -1;

        if (child->name == "login"sv) {
            on_login_finished(status);
            continue;
        }

        bool normal_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        log_line(ByteString::formatted("{} exited (status {}, signal {})", child->name, WEXITSTATUS(status), WTERMSIG(status)));

        if (g_shutting_down || !g_restart_enabled || !child->restart_on_exit)
            continue;
        if (normal_exit && !child->always_restart)
            continue;

        child->restart_count++;
        if (child->restart_count > 20) {
            log_line(ByteString::formatted("{} crashed too often, giving up", child->name));
            continue;
        }
        auto delay = min(5000, 200 * child->restart_count);
        log_line(ByteString::formatted("restarting {} in {} ms", child->name, delay));
        auto timer = Core::Timer::create_single_shot(delay, [child] {
            if (!g_shutting_down && child->pid <= 0)
                (void)spawn(*child);
        });
        g_pending_restarts.append(timer);
        timer->start();
    }
}

// Scheduler state letter from /proc/<pid>/stat, or 0 if it cannot be read.
// 'T' means stopped and 'D' means stuck in the kernel: both look alive to
// kill(pid, 0) but will never service another request.
char process_state(pid_t pid)
{
    auto path = ByteString::formatted("/proc/{}/stat", pid);
    auto* file = fopen(path.characters(), "r");
    if (!file)
        return 0;
    char buffer[512];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    auto* paren = strrchr(buffer, ')');
    if (!paren || paren[1] != ' ' || paren[2] == '\0')
        return 0;
    return paren[2];
}

bool process_is_healthy(pid_t pid)
{
    if (pid <= 0 || kill(pid, 0) != 0)
        return false;
    auto state = process_state(pid);
    return state != 'T' && state != 't' && state != 'Z' && state != 'D';
}

// Ctrl+Alt+Del arrives as SIGINT because we point /proc/sys/kernel/cad_pid at
// ourselves. This is the one recovery path that keeps working when the graphics
// stack is wedged, so it lives here and not in a GUI app.
void on_cad_signal(int)
{
    g_cad_pending = 1;
}

// Log out: serenity-logout sends this, no privileges needed because the
// supervisor is the one that owns every child.
void on_logout_signal(int)
{
    g_logout_pending = 1;
}

// Stop every child in reverse start order, then replace this process with a
// fresh copy. exec also resets the signal handlers, so the new session starts
// with the defaults instead of inheriting the logout request.
void restart_session()
{
    log_line("logout requested, restarting the session"sv);
    g_shutting_down = true;
    g_stage = Stage::ShuttingDown;

    for (auto& child : g_children.in_reverse()) {
        if (child->pid > 0)
            stop_child(*child);
    }
    // Drop the log fd: the replacement opens it again, and two writers on the
    // same append-mode file interleave lines mid-record.
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }

    char exe_path[PATH_MAX];
    auto length = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (length <= 0) {
        log_line("logout: cannot resolve /proc/self/exe"sv);
        Core::EventLoop::current().quit(0);
        return;
    }
    exe_path[length] = '\0';
    char* argv[] = { exe_path, nullptr };
    execv(exe_path, argv);
    log_line(ByteString::formatted("logout: re-exec of {} failed: {}", exe_path, strerror(errno)));
    Core::EventLoop::current().quit(1);
}

void spawn_detached(ByteString const& path)
{
    pid_t pid = fork();
    if (pid != 0)
        return;
    setsid();
    if (g_log) {
        dup2(fileno(g_log), STDOUT_FILENO);
        dup2(fileno(g_log), STDERR_FILENO);
    }
    setenv("SERENITY_RESOURCE_ROOT", "/res", 0);
    auto lib_dir = ByteString::formatted("{}/../lib", g_bin_dir);
    setenv("LD_LIBRARY_PATH", lib_dir.characters(), 1);
    char* argv[] = { const_cast<char*>(path.characters()), nullptr };
    execv(path.characters(), argv);
    _exit(127);
}

// Give WindowServer a clean slate: a hung compositor never reacts to signals it
// is not reading, so SIGKILL is the only thing that reliably unsticks it.
bool restart_window_server()
{
    auto* window_server = find_child("window-server"sv);
    if (!window_server)
        return false;

    if (window_server->pid > 0) {
        log_line("CAD: killing window-server"sv);
        kill(window_server->pid, SIGKILL);

        // Wait for it to actually go away: while the corpse still holds the
        // DRM device the replacement cannot become DRM master and ends up
        // rendering into a surface nobody presents.
        for (int i = 0; i < 100 && window_server->pid > 0; ++i) {
            reap_children();
            usleep(20 * 1000);
        }
        window_server->pid = -1;
        window_server->restart_count = 0;
        usleep(100 * 1000);
    }

    if (spawn(*window_server).is_error()) {
        log_line("CAD: failed to respawn window-server"sv);
        return false;
    }

    for (int i = 0; i < 100 && !socket_ready(ByteString { "/tmp/portal/window" }); ++i) {
        reap_children();
        usleep(50 * 1000);
    }
    return socket_ready(ByteString { "/tmp/portal/window" });
}

// Ctrl+Alt+Del is the recovery affordance that must survive a wedged
// compositor. Retry the restart a few times instead of giving up after one
// failed spawn, then put the shell/applets back through the normal path.
bool recover_graphics_stack()
{
    for (int attempt = 1; attempt <= 3; ++attempt) {
        log_line(ByteString::formatted("CAD: graphics restart attempt {}/3", attempt));
        if (restart_window_server()) {
            start_desktop();
            return true;
        }
    }
    return false;
}

void on_signal(int signal_number)
{
    log_line(ByteString::formatted("received signal {}, shutting down", signal_number));
    g_shutting_down = true;
    g_stage = Stage::ShuttingDown;
    for (auto& child : g_children.in_reverse()) {
        if (child->pid > 0)
            stop_child(*child);
    }
    Core::EventLoop::current().quit(signal_number == SIGTERM ? 0 : 1);
}

} // namespace

ErrorOr<int> serenity_main(Main::Arguments)
{
    g_bin_dir = ByteString { "/home/serenity/serenity/Build/lagom/bin" };
    if (auto* dir = getenv("SERENITY_BIN"); dir && *dir)
        g_bin_dir = dir;

    // The install prefix is the parent of bin/ (usually /serenity). Children
    // inherit it: hostify.h looks for <prefix>/bin, the shell for
    // <prefix>/share/desktop.
    if (auto* prefix = getenv("SERENITY_PREFIX"); !prefix || !*prefix) {
        auto derived = ByteString { LexicalPath { g_bin_dir }.parent().string() };
        setenv("SERENITY_PREFIX", derived.characters(), 1);
    }

    // Allow a non-root session to redirect its own log/progress/config paths.
    // SERENITY_LOG / SERENITY_PROGRESS / SERENITY_CONFIG are set by the service
    // unit when it runs as an unprivileged user.
    if (auto* path = getenv("SERENITY_LOG"); path && *path)
        g_log_path = path;
    if (auto* path = getenv("SERENITY_PROGRESS"); path && *path)
        g_progress_path = path;
    if (auto* path = getenv("SERENITY_CONFIG"); path && *path)
        g_config_path = path;

    g_log = fopen(g_log_path.characters(), "a");
    if (!g_log)
        g_log = stderr;

    if (auto config = Core::ConfigFile::open(g_config_path); !config.is_error()) {
        auto restart = config.value()->read_entry("Session", "Restart");
        if (restart == "no"sv || restart == "0"sv || restart == "false"sv)
            g_restart_enabled = false;
    }

    log_line(ByteString::formatted("serenitid starting (bin dir {})", g_bin_dir));
    write_progress(5, "STARTING SERVICES"sv);

    // "name" is the logical role (lookups/logs), "binary" the file in $BIN.
    auto make_child = [](ByteString name, ByteString binary, Vector<ByteString> arguments, bool restart) -> NonnullOwnPtr<Child> {
        auto child = make<Child>();
        child->name = move(name);
        child->path = binary.starts_with("/"sv) ? binary : ByteString::formatted("{}/{}", g_bin_dir, binary);
        child->arguments = move(arguments);
        child->restart_on_exit = restart;
        return child;
    };

    g_children.append(make_child(ByteString { "config-server" }, ByteString { "serenity-config-server" }, {}, true));
    g_children.append(make_child(ByteString { "launch-server" }, ByteString { "serenity-launch-server" }, {}, true));
    // Copy/paste for every app: LibGUI only talks to a clipboard server, and
    // without this one apps run with an empty, permanently read-only clipboard.
    g_children.append(make_child(ByteString { "clipboard" }, ByteString { "serenity-clipboard-server" }, {}, true));
    // FileSystemAccessServer: backs File / Open / Save As in Notepad and the
    // other ported GUI apps. Spawned explicitly because systemd socket
    // activation (Lazy=true in SystemServerUser.ini) is not implemented here,
    // so the portal socket would otherwise never exist.
    g_children.append(make_child(ByteString { "file-system-access-server" }, ByteString { "serenity-file-system-access-server" }, {}, true));
    // D-Bus session bus. Nothing else starts one for this service (it has no
    // PAMName=login session, so there is no dbus.socket), and without it fcitx5
    // exits immediately on startup, GTK apps cannot reach xdg-desktop-portal and
    // anything that calls g_bus_get_sync just fails. The socket lives next to
    // the runtime dir that systemd already hands us.
    {
        auto runtime_dir = getenv("XDG_RUNTIME_DIR") ? ByteString { getenv("XDG_RUNTIME_DIR") } : ByteString { "/tmp" };
        auto bus_address = ByteString::formatted("unix:path={}/bus", runtime_dir);
        setenv("DBUS_SESSION_BUS_ADDRESS", bus_address.characters(), 0);
        g_children.append(make_child(ByteString { "dbus" }, ByteString { "/usr/bin/dbus-daemon" },
            { ByteString { "--session" }, ByteString { "--nofork" }, ByteString { "--nopidfile" },
                ByteString::formatted("--address={}", bus_address) },
            true));

        // Point D-Bus activation of the portal name at OUR portal. The distro's
        // xdg-desktop-portal is *D-Bus* activated (it is not a systemd unit), so
        // systemctl masking does nothing; a user-level .service file shadows
        // /usr/share/dbus-1/services because the session bus searches
        // XDG_DATA_HOME first. Clients (fcitx5, GTK at startup) then launch
        // serenity-portal instead of a portal that cannot serve this desktop.
        // Written before dbus starts so the very first activation already uses it.
        {
            ByteString data_home;
            if (auto* dh = getenv("XDG_DATA_HOME"); dh && *dh)
                data_home = ByteString { dh };
            else if (auto* home = getenv("HOME"); home && *home)
                data_home = ByteString::formatted("{}/.local/share", home);
            else
                data_home = ByteString { "/tmp" };
            ByteString service_dir = ByteString::formatted("{}/dbus-1/services", data_home);
            ByteString mkdir_cmd = ByteString::formatted("mkdir -p \"{}\"", service_dir);
            system(mkdir_cmd.characters());
            ByteString service_path = ByteString::formatted("{}/org.freedesktop.portal.Desktop.service", service_dir);
            ByteString portal_bin = ByteString::formatted("{}/serenity-portal", g_bin_dir);
            if (FILE* f = fopen(service_path.characters(), "w")) {
                fprintf(f,
                    "[D-BUS Service]\n"
                    "Name=org.freedesktop.portal.Desktop\n"
                    "Exec=%s\n",
                    portal_bin.characters());
                fclose(f);
            }
        }
    }
    // Audio: AudioServer mixes into /dev/audio/0 (serenity_compat), the bridge
    // forwards that PCM to the host's sound card.
    g_children.append(make_child(ByteString { "audio-server" }, ByteString { "serenity-audio-server" }, {}, true));
    g_children.append(make_child(ByteString { "audio-bridge" }, ByteString { "serenity-audio-bridge" }, {}, true));
    auto window_server_child = make_child(ByteString { "window-server" }, ByteString { "serenity-window-server" }, { ByteString { "--config" }, ByteString { "/etc/WindowServer.ini" } }, true);
    window_server_child->always_restart = true;
    g_children.append(move(window_server_child));
    g_children.append(make_child(ByteString { "login" }, ByteString { "serenity-login-server" }, {}, false));

    // Foreign clients all go through serenity-wlroots-host (the wlroots-backed
    // host): it speaks wl_compositor/xdg-shell via wlroots to Wayland natives
    // and runs its own rootless Xwayland plus an XWM for X11 apps, so every
    // foreign window - Wayland or X11 - becomes a first-class Serenity window
    // with the same input path.
    //
    // 旧桥 serenity-wayland-host（自研协议实现）已放弃：它缺 wl_subcompositor
    // 等 global，Firefox 的 Wayland 后端会在启动 0.2s 内断言退出
    // （MOZ_RELEASE_ASSERT(GetSubcompositor() — "We're missing subcompositor
    // interface!")）。旧桥只保留二进制供 A/B 对照，不再进生产链路。
    //
    // The Xvfb + serenity-x11-bridge pair this replaced copied a full frame per
    // window per tick and could never show native Wayland clients at all; it was
    // deleted once the host handled both worlds (see LINUX_GUI_PORT.md).
    {
        // The shell re-exports both names to everything it launches: Wayland
        // clients go to the host's socket, X11-only binaries to the Xwayland
        // display it creates. Keep these in sync with WaylandHost's defaults.
        setenv("WAYLAND_DISPLAY", "wayland-serenity-0", 1);
        setenv("SERENITY_APP_DISPLAY", ":100", 1);
        // Every GTK app wears the desktop's own theme instead of Adwaita. The
        // generated theme (scripts/port/serenity-theme-gen.py) is installed as
        // /usr/share/themes/Serenity; GTK_THEME also covers libadwaita, which
        // ignores gtk-theme-name.
        setenv("GTK_THEME", "Serenity", 0);
        // GTK3 (Firefox, GIMP, ...) still draws its own CSD - a rounded window
        // with a ~26 px transparent shadow border - even though we answer
        // SERVER_SIDE for xdg-decoration. Without this the client's shadow
        // shows as a black band between our Serenity title bar and the content,
        // so the frame looks like it does not wrap. GTK_CSD=0 makes GTK3 drop
        // CSD entirely and rely on our server-side frame. (GTK4 ignores it and
        // always uses CSD, which is why GTK4 apps keep their own header bar.)
        setenv("GTK_CSD", "0", 0);
        // Launch foreign clients as native Wayland, not X11 (Xwayland). Without
        // this Firefox/GTK pick X11 by default, so apps started from the desktop
        // land in Xwayland while the same binary started by hand (with the env
        // set) runs on Wayland. Prefer Wayland and fall back to X11 only when a
        // client cannot speak it.
        setenv("MOZ_ENABLE_WAYLAND", "1", 0);
        setenv("GDK_BACKEND", "wayland,x11", 0);
        setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
        // Qt has no process-global stylesheet env var, so use Fusion rather
        // than whatever platform default the no-DESKTOP_SESSION session picks.
        // The generated palette lives in /usr/share/serenity for qt5ct/qt6ct
        // setups to pick up.
        setenv("QT_STYLE_OVERRIDE", "Fusion", 0);
        auto wayland_child = make_child(ByteString { "wayland-host" }, ByteString { "serenity-wlroots-host" }, {}, true);
        wayland_child->always_restart = true;
        g_children.append(move(wayland_child));

        // The distro's xdg-desktop-portal is D-Bus activated and answers
        // org.freedesktop.portal.Desktop with interfaces this desktop does not
        // implement ("Unknown interface" -> GTK loses its settings/file picker).
        // We already shadowed its activation with our .service file (see the
        // D-Bus setup above); also clear any instance a previous session left
        // running, which would still hold the name and win over activation.
        system("pkill -f '/usr/libexec/xdg-desktop-portal' >/dev/null 2>&1");
        // Tell GTK this is a real (if unusual) desktop so it uses our portal
        // rather than probing a hardcoded GNOME/other backend.
        setenv("XDG_CURRENT_DESKTOP", "Serenity", 1);

        // Do NOT spawn serenity-portal directly: it is D-Bus activated through
        // the .service file we wrote above. That way it starts when the first
        // client (fcitx5, GTK) asks for org.freedesktop.portal.Desktop, i.e.
        // always after the session bus exists - and there is exactly one
        // instance instead of a direct spawn racing the activated one.
        (void)0;
    }

    // Input method: fcitx5 runs as a session child. LibGUI text widgets reach
    // it through the serenityime addon's unix socket (LibGUI/InputMethod.cpp);
    // GTK/Qt/X11 clients use fcitx's own IM modules. D-Bus is started before
    // this child, so restart it on exit: a one-shot fcitx5 that exits (even
    // cleanly, for example after `fcitx5 -r`) must not leave the session with
    // no input method. The X_OK filter below drops it entirely when fcitx5 is
    // not installed.
    {
        setenv("GTK_IM_MODULE", "fcitx", 0);
        setenv("QT_IM_MODULE", "fcitx", 0);
        setenv("XMODIFIERS", "@im=fcitx", 0);
        // XIM needs the X server Wine and other X11 clients actually use.
        setenv("DISPLAY", ":100", 0);
        g_children.append(make_child(ByteString { "ime" }, ByteString { "/usr/bin/fcitx5" }, {}, true));
    }

    // One shell process: desktop (wallpaper + icons) and taskbar (start menu,
    // window list, clock, tray) share an app catalog and an icon cache.
    auto shell_child = make_child(ByteString { "shell" }, ByteString { "serenity-shell" }, {}, true);
    shell_child->always_restart = true;
    g_children.append(move(shell_child));
    // Tray applets (right-hand side of the Taskbar).
    g_children.append(make_child(ByteString { "resource-graph" }, ByteString { "serenity-resource-graph" }, { ByteString { "--cpu" }, ByteString { "CPU,#4ec94e" }, ByteString { "--memory" }, ByteString { "MEM,#c9b44e" }, ByteString { "--network" }, ByteString { "NET,#4e9ac9" } }, true));
    g_children.append(make_child(ByteString { "network" }, ByteString { "serenity-network" }, {}, true));
    // 剪贴板历史录制器：没有窗口，只把每次复制落盘，Win+V 打开列表读同一份文件。
    g_children.append(make_child(ByteString { "clipboard-history" }, ByteString { "serenity-clipboard-history" }, { ByteString { "--record" } }, true));
    // eBPF 采集每进程网络流量（kprobe tcp_sendmsg/tcp_recvmsg）。加载失败就自己退出，
    // 不要重启循环：任务管理器读不到文件时会退回网卡级统计。
    g_children.append(make_child(ByteString { "netprobe" }, ByteString { "serenity-netprobe" }, {}, false));

    // Components that are built separately (or not at all) must not turn into a
    // restart loop: drop the ones whose binary is missing.
    g_children.remove_all_matching([](auto const& child) {
        if (access(child->path.characters(), X_OK) == 0)
            return false;
        log_line(ByteString::formatted("{} is not installed, skipping", child->name));
        return true;
    });

    Core::EventLoop loop;

    auto previous_term = signal(SIGTERM, on_signal);
    auto previous_int = signal(SIGINT, on_cad_signal);
    auto previous_usr1 = signal(SIGUSR1, on_logout_signal);
    (void)previous_term;
    (void)previous_int;
    (void)previous_usr1;

    // Ask the kernel to deliver Ctrl+Alt+Del to us instead of to init, so the
    // shortcut opens the task manager rather than rebooting.
    // /proc/sys/kernel/cad_pid is root:root 0600 and the session now runs as an
    // unprivileged user, so normally systemd's ExecStartPost=+ (root) writes
    // our PID there. Keep the direct write for sessions started as root; do not
    // claim failure here -- logging "could not claim" used to be a red herring
    // even when the systemd claim had succeeded.
    if (auto* file = fopen("/proc/sys/kernel/cad_pid", "w")) {
        fprintf(file, "%d", getpid());
        fclose(file);
        log_line("claimed Ctrl+Alt+Del directly (root session)"sv);
    }

    auto cad_timer = Core::Timer::create_repeating(200, [] {
        if (!g_cad_pending)
            return;
        g_cad_pending = 0;
        log_line("CAD pressed"sv);

        auto* window_server = find_child("window-server"sv);
        auto window_socket = ByteString { "/tmp/portal/window" };
        bool healthy = window_server && process_is_healthy(window_server->pid)
            && socket_ready(window_socket) && socket_accepting(window_socket);

        if (!healthy) {
            log_line("CAD: graphics stack is not responding, restarting it"sv);
            if (!recover_graphics_stack())
                log_line("CAD: could not bring the window server back"sv);
        }

        // The task manager is a normal client, so it works again once the
        // compositor is back. Not installed -> only the restart above happens.
        auto task_manager = ByteString::formatted("{}/serenity-system-monitor", g_bin_dir);
        if (access(task_manager.characters(), X_OK) == 0) {
            log_line("CAD: launching task manager"sv);
            spawn_detached(task_manager);
        } else {
            log_line("CAD: task manager not installed, shell restarted only"sv);
        }
    });
    cad_timer->start();

    // Stage 1: graphics stack.
    if (auto result = spawn(*find_child("config-server"sv)); result.is_error())
        log_line(ByteString::formatted("failed to start ConfigServer: {}", result.error()));
    write_progress(20, "CONFIG AND LAUNCHER"sv);

    if (auto result = spawn(*find_child("launch-server"sv)); result.is_error())
        log_line(ByteString::formatted("failed to start LaunchServer: {}", result.error()));

    if (auto* clipboard = find_child("clipboard"sv))
        (void)spawn(*clipboard);

    // NOTE: FileSystemAccessServer is spawned *after* WindowServer (see below):
    // it is a real LibGUI application (GUI::Application::create connects to the
    // window server immediately) and would abort at startup otherwise.

    // Audio first: apps may play as soon as the shell is up.
    if (auto* audio_server = find_child("audio-server"sv))
        (void)spawn(*audio_server);
    if (auto* audio_bridge = find_child("audio-bridge"sv))
        (void)spawn(*audio_bridge);
    write_progress(35, "GRAPHICS STACK"sv);

    // Retire the framebuffer splash before WindowServer grabs the DRM device.
    write_progress(100, "STARTING DESKTOP"sv);
    system("pkill -x serenity-fb-spl 2>/dev/null");
    usleep(150 * 1000);

    auto* window_server = find_child("window-server"sv);
    if (auto result = spawn(*window_server); result.is_error())
        log_line(ByteString::formatted("failed to start WindowServer: {}", result.error()));

    // Service the event loop until the WindowServer socket shows up.
    for (int i = 0; i < 200 && !socket_ready(ByteString { "/tmp/portal/window" }); ++i) {
        reap_children();
        usleep(50 * 1000);
    }
    if (!socket_ready(ByteString { "/tmp/portal/window" }))
        log_line("WindowServer socket never appeared"sv);
    write_progress(80, "WINDOW SERVER"sv);

    // FileSystemAccessServer backs File / Open / Save As in every GUI app that
    // uses GUI::FilePicker (Notepad, PixelPaint, ImageViewer, ...). It must come
    // up after WindowServer: the process is a LibGUI client, and on its first
    // request it calls ConnectionToWindowServer::the(), which pushes the system
    // palette into GUI::Application::the() -- a null pointer if the server ran
    // without an Application (that used to be a hard SIGSEGV on every Open).
    if (auto* filesystem_access_server = find_child("file-system-access-server"sv))
        (void)spawn(*filesystem_access_server);

    // Foreign-client stack. The Wayland host has to be listening before the
    // shell starts: the shell hands WAYLAND_DISPLAY to every app it launches,
    // and a GTK app that finds no socket falls back to X11 without saying so.
    bool const wlroots_presenter = getenv("SERENITY_PRESENTER") && !strcmp(getenv("SERENITY_PRESENTER"), "wlroots");
    if (wlroots_presenter) {
        // WindowServer 自己就是合成器（SERENITY_PRESENTER=wlroots）：客户端的
        // Wayland socket 由它在进程内提供，绝不能再起独立的 wayland-host ——
        // 两者会争 DRM master / libinput，还会抢同一个 socket 名。
        auto runtime_dir = getenv("XDG_RUNTIME_DIR") ? ByteString { getenv("XDG_RUNTIME_DIR") } : ByteString { "/tmp" };
        auto socket = ByteString::formatted("{}/wayland-serenity-0", runtime_dir);
        for (int i = 0; i < 200 && !socket_ready(socket); ++i) {
            reap_children();
            usleep(25 * 1000);
        }
        if (socket_ready(socket))
            log_line("wlroots compositor socket ready (Wayland host not started)"sv);
        else
            log_line("wlroots compositor socket never appeared - Wayland apps will not start"sv);
    } else if (auto* wayland = find_child("wayland-host"sv)) {
        if (auto result = spawn(*wayland); result.is_error())
            log_line(ByteString::formatted("failed to start Wayland host: {}", result.error()));
        auto runtime_dir = getenv("XDG_RUNTIME_DIR") ? ByteString { getenv("XDG_RUNTIME_DIR") } : ByteString { "/tmp" };
        auto socket = ByteString::formatted("{}/wayland-serenity-0", runtime_dir);
        for (int i = 0; i < 200 && !socket_ready(socket); ++i) {
            reap_children();
            usleep(25 * 1000);
        }
        if (socket_ready(socket))
            log_line("Wayland host socket ready"sv);
        else
            log_line("Wayland host socket never appeared - Wayland apps will not start"sv);
    }

    // Portal first: the shell's very first launched app must not stall on it.
    if (auto* portal = find_child("portal"sv))
        (void)spawn(*portal);

    // D-Bus session bus: it is a prerequisite for fcitx5 and any GTK/Qt client
    // that asks for a session bus. Start it before the input method and wait
    // briefly for the socket so fcitx5 does not come up without D-Bus.
    if (auto* dbus = find_child("dbus"sv)) {
        if (auto result = spawn(*dbus); result.is_error()) {
            log_line(ByteString::formatted("failed to start D-Bus session bus: {}", result.error()));
        } else {
            auto runtime_dir = getenv("XDG_RUNTIME_DIR") ? ByteString { getenv("XDG_RUNTIME_DIR") } : ByteString { "/tmp" };
            auto bus_path = ByteString::formatted("{}/bus", runtime_dir);
            for (int i = 0; i < 100 && !socket_ready(bus_path); ++i) {
                reap_children();
                usleep(20 * 1000);
            }
            if (socket_ready(bus_path))
                log_line("D-Bus session bus ready"sv);
            else
                log_line("D-Bus session bus socket never appeared - fcitx5 and GTK/Qt apps will not find a session bus"sv);
        }
    }

    // Input method before the shell starts apps: the first focused text box
    // should find an engine socket instead of quietly typing English only.
    if (auto* ime = find_child("ime"sv))
        (void)spawn(*ime);

    // Windows compatibility: the Wayland host owns Xwayland, so nothing else to
    // start here - a .exe launch just needs DISPLAY, which the shell exports.

    // Stage 2: graphical login (exits 0 once authenticated).
    g_stage = Stage::Login;
    auto* login = find_child("login"sv);
    auto autologin = ByteString { };
    if (auto config = Core::ConfigFile::open(g_config_path); !config.is_error())
        autologin = config.value()->read_entry("Session", "Autologin");
    // --auto-login USER as two words: the login server parses them as a flag
    // followed by a value, so "--auto-login=USER" would be silently ignored.
    if (!autologin.is_empty()) {
        login->arguments.append(ByteString { "--auto-login" });
        login->arguments.append(autologin);
    }

    if (auto result = spawn(*login); result.is_error())
        log_line(ByteString::formatted("failed to start login server: {}", result.error()));

    // Stage 3: supervision.
    auto supervisor = Core::Timer::create_repeating(250, [] {
        if (g_logout_pending) {
            g_logout_pending = 0;
            restart_session();
            return;
        }
        reap_children();
        // Drop finished restart timers so the list does not grow forever.
        g_pending_restarts.remove_all_matching([](auto& timer) { return !timer->is_active(); });
    });
    supervisor->start();

    log_line("entering supervision loop"sv);
    return loop.exec();
}
