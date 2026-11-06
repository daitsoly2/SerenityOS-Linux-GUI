/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-shell: the desktop shell, in one process, the way explorer.exe is on
// Windows.
//
//   - the desktop window: wallpaper + icons (WindowType::Desktop)
//   - the taskbar window: start menu, window list, clock, tray
//
// Why one process: both halves read the same /res/apps list and icon cache
// (AppCatalog) and resolve launches through the same helper, so splitting them
// meant scanning twice, decoding icons twice and keeping two copies of the
// "/bin/Name" -> "serenity-<kebab-case>" rule. It also saves an IPC connection
// to the WindowServer and lets the desktop and taskbar share menu state.

#include "DesktopIconsWidget.h"
#include "SystemMenu.h"
#include "TaskbarWindow.h"
#include "AppCatalog.h"
#include <AK/Debug.h>
#include <LibConfig/Client.h>
#include <LibCore/EventLoop.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/Application.h>
#include <LibGUI/ConnectionToWindowManagerServer.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Menu.h>
#include <LibGUI/UITranslation.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <WindowServer/Window.h>
#include <signal.h>
#include <sys/wait.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    // Apps launched from the start menu (.desktop entries) are X11 clients:
    // give them the bridge display in their environment (overridable for
    // tests). Inherited via posix_spawn by every child spawned from here.
    setenv("DISPLAY", getenv("SERENITY_APP_DISPLAY") ? getenv("SERENITY_APP_DISPLAY") : ":100", 0);

    // Wayland clients connect to the WaylandHost's socket, so GTK/Qt apps are
    // hosted as first-class windows in the same layer as native ones (instead
    // of falling through to XWayland). Set after DISPLAY so apps that probe
    // both pick Wayland first; X11-only apps ignore it and use DISPLAY.
    setenv("WAYLAND_DISPLAY", "wayland-serenity-0", 0);

    // Two windows share this event loop; closing one of them must not end the
    // session (the session manager restarts the shell only if it really exits).
    app->set_quit_when_last_window_deleted(false);

    // "WindowManager" is needed by GUI::Desktop::wallpaper_path(): the desktop
    // reads the wallpaper the user picked in DisplaySettings from ConfigServer,
    // just like FileManager and ImageViewer already do upstream.
    Config::pledge_domains({ "Taskbar", "Calendar", "WindowManager" });
    Config::monitor_domain("Taskbar");
    Config::monitor_domain("Calendar");

    app->event_loop().register_signal(SIGCHLD, [](int) {
        // Reap everything the shell spawned: start menu entries, desktop icons,
        // wine, archive tools.
        while (waitpid(-1, nullptr, WNOHANG) > 0)
            ;
    });

    // Both connections must exist before either window is created: the taskbar
    // listens for WM events, the desktop spawns through LaunchServer.
    GUI::ConnectionToWindowManagerServer::the();
    Desktop::Launcher::ensure_connection();

    // Desktop first, so it sits behind the taskbar in the window stack.
    auto desktop_window = GUI::Window::construct();
    desktop_window->set_title("Desktop"sv);
    desktop_window->set_window_type(GUI::WindowType::Desktop);
    desktop_window->set_frameless(true);
    desktop_window->set_resizable(false);
    desktop_window->set_rect(GUI::Desktop::the().rect());
    (void)desktop_window->set_main_widget<DesktopIconsWidget>();
    desktop_window->show();

    auto taskbar_window = TRY(TaskbarWindow::create());

    // Right-click removal / drag-to-reclassify mutate the data behind the Start
    // menu, so the shell needs a way to swap in a freshly built menu tree.
    set_start_menu_rebuilder([taskbar_window] {
        auto new_menu = build_system_menu(*taskbar_window);
        if (new_menu.is_error()) {
            warnln("shell: cannot rebuild Start menu: {}", new_menu.error());
            return;
        }
        auto menu = new_menu.release_value();
        menu->realize_menu_if_needed();
        taskbar_window->add_system_menu(menu);
        dbgln("shell: Start menu rebuilt");
    });

    auto system_menu = TRY(build_system_menu(*taskbar_window));
    system_menu->realize_menu_if_needed();
    taskbar_window->add_system_menu(system_menu);
    taskbar_window->show();

    taskbar_window->make_window_manager(
        WindowServer::WMEventMask::WindowStateChanges
        | WindowServer::WMEventMask::WindowRemovals
        | WindowServer::WMEventMask::WindowIconChanges
        | WindowServer::WMEventMask::WorkspaceChanges);

    dbgln("shell: desktop {} + taskbar {} in one process, {} apps in catalog",
        desktop_window->rect(), taskbar_window->rect(), AppCatalog::the().apps().size());
    dbgln("shell: UI language '{}', {} translations, titles '{}' / '{}'",
        GUI::current_ui_language(), GUI::ui_translation_count(), desktop_window->title(), taskbar_window->title());

    return app->exec();
}
