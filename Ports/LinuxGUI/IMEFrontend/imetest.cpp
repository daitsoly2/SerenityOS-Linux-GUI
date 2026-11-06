/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Minimal native LibGUI app used to exercise the IME client path in isolation.
//
// It opens exactly one window whose main widget is a TextEditor and gives it
// focus immediately, so TextEditor::focusin_event drives
// GUI::InputMethod::focus_in() without any window-manager / z-order games. That
// is enough to connect the client to the engine, publish the status/methods
// files, and (once you type) show the candidate window.
//
// Launch it with the session environment, e.g. from the VM:
//   env SERENITY_BIN=/serenity/bin SERENITY_PREFIX=/serenity \
//       SERENITY_RESOURCE_ROOT=/res DISPLAY=:100 \
//       WAYLAND_DISPLAY=wayland-serenity-0 XDG_RUNTIME_DIR=/run/user/1000 \
//       LD_LIBRARY_PATH=/serenity/lib /serenity/bin/serenity-imetest

#include <LibGUI/Application.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("IME Test"sv);
    window->resize(640, 220);
    window->center_on_screen();

    auto editor = window->set_main_widget<GUI::TextEditor>();
    editor->set_text("ime> "sv);

    window->show();
    // Focusing the editor makes TextEditor::focusin_event() run, which connects
    // the IME client to the engine and publishes the taskbar status.
    window->set_focused_widget(editor);

    return app->exec();
}
