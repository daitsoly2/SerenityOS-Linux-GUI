/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Screenshot & Recorder — the control panel for the compositor's capture
// engine. Everything heavy (grabbing frames, encoding, notifications, global
// hotkeys) lives in WindowServer; this window is only a front end for the
// /tmp/serenity-recorder.sock control channel, the same split gpurec uses
// between its UI process and gpu-screen-recorder.

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/GroupBox.h>
#include <LibGUI/Label.h>
#include <LibGUI/SpinBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

constexpr auto control_socket_path = "/tmp/serenity-recorder.sock"sv;

String to_string(ByteString const& value)
{
    return String::from_utf8(value.view()).release_value_but_fixme_should_propagate_errors();
}

Optional<ByteString> send_command(StringView command)
{
    auto socket_or_error = Core::LocalSocket::connect(control_socket_path);
    if (socket_or_error.is_error())
        return {};
    auto& socket = *socket_or_error.value();

    // Never block the GUI event loop on the recorder: if the reply does not
    // arrive within 150ms the caller just gets nothing and keeps polling.
    if (auto fd = socket.fd(); fd.has_value()) {
        struct timeval timeout {
            0, 150 * 1000
        };
        setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    auto payload = ByteString::formatted("{}\n", command);
    if (socket.write_some(payload.bytes()).is_error())
        return {};

    StringBuilder reply;
    auto buffer = ByteBuffer::create_uninitialized(512).release_value_but_fixme_should_propagate_errors();
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto result = socket.read_some(buffer.bytes());
        if (result.is_error())
            break;
        auto bytes = result.release_value();
        if (bytes.is_empty())
            break;
        reply.append(StringView { reinterpret_cast<char const*>(bytes.data()), bytes.size() });
        if (reply.string_view().contains('\n'))
            break;
    }
    if (reply.is_empty())
        return {};
    return ByteString { reply.string_view() }.trim_whitespace();
}

struct Status {
    ByteString state { "unknown"sv };
    unsigned elapsed { 0 };
    int fps { 30 };
    ByteString path;
};

Status parse_status(ByteString const& line)
{
    Status status;
    auto parts = line.split(' ');
    if (parts.size() < 5 || parts[0] != "status"sv)
        return status;
    status.state = parts[1];
    if (auto value = parts[2].to_number<unsigned>(); value.has_value())
        status.elapsed = *value;
    if (auto value = parts[3].to_number<int>(); value.has_value())
        status.fps = *value;
    // Everything after the fourth field is the path, which may contain spaces.
    status.path = line.substring(parts[0].length() + parts[1].length() + parts[2].length() + parts[3].length() + 4);
    return status;
}

ByteString format_elapsed(unsigned seconds)
{
    return ByteString::formatted("{:02}:{:02}", (seconds / 60) % 60, seconds % 60);
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Screenshot & Recorder"sv);
    window->resize(470, 350);
    window->set_resizable(false);

    auto main_widget = window->set_main_widget<GUI::Widget>();
    auto& root = *main_widget;
    root.set_layout<GUI::VerticalBoxLayout>(8, 10);
    root.set_fill_with_background_color(true);

    // ── Screenshot ──────────────────────────────────────────────────────────
    auto& screenshot_group = root.add<GUI::GroupBox>("Screenshot"_string);
    screenshot_group.set_layout<GUI::VerticalBoxLayout>(4, 6);

    auto& screenshot_row = screenshot_group.add<GUI::Widget>();
    screenshot_row.set_layout<GUI::HorizontalBoxLayout>(6);
    screenshot_row.set_fixed_height(26);

    auto& screenshot_button = screenshot_row.add<GUI::Button>("Take screenshot"_string);
    screenshot_button.set_fixed_width(150);
    screenshot_button.on_click = [](auto) {
        (void)send_command("screenshot"sv);
    };

    auto& screenshot_hint = screenshot_row.add<GUI::Label>("PrintScreen  /  Super+Shift+S"_string);
    screenshot_hint.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    // ── Recording ───────────────────────────────────────────────────────────
    auto& record_group = root.add<GUI::GroupBox>("Recording"_string);
    record_group.set_layout<GUI::VerticalBoxLayout>(4, 6);

    auto& record_row = record_group.add<GUI::Widget>();
    record_row.set_layout<GUI::HorizontalBoxLayout>(6);
    record_row.set_fixed_height(26);

    auto& record_button = record_row.add<GUI::Button>("Start recording"_string);
    record_button.set_fixed_width(150);
    record_button.on_click = [](auto) {
        (void)send_command("toggle"sv);
    };

    auto& pause_button = record_row.add<GUI::Button>("Pause"_string);
    pause_button.set_fixed_width(90);
    pause_button.on_click = [](auto) {
        (void)send_command("pause"sv);
    };

    auto& record_hint = record_row.add<GUI::Label>("Super+Shift+V  /  Super+Shift+P"_string);
    record_hint.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    auto& fps_row = record_group.add<GUI::Widget>();
    fps_row.set_layout<GUI::HorizontalBoxLayout>(6);
    fps_row.set_fixed_height(26);

    auto& fps_label = fps_row.add<GUI::Label>("Frame rate:"_string);
    fps_label.set_fixed_width(80);
    auto& fps_spinbox = fps_row.add<GUI::SpinBox>();
    fps_spinbox.set_fixed_width(70);
    fps_spinbox.set_range(1, 120, GUI::AllowCallback::No);
    fps_spinbox.set_value(30, GUI::AllowCallback::No);
    fps_spinbox.on_change = [](int value) {
        (void)send_command(ByteString::formatted("set fps {}", value));
    };
    auto& fps_note = fps_row.add<GUI::Label>("host ffmpeg encodes to H.264 / mp4"_string);
    fps_note.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    auto& path_label = record_group.add<GUI::Label>("Idle"_string);
    path_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    path_label.set_fixed_height(20);

    // ── Notifications ───────────────────────────────────────────────────────
    auto& notify_group = root.add<GUI::GroupBox>("Notifications"_string);
    notify_group.set_layout<GUI::HorizontalBoxLayout>(6, 6);
    notify_group.set_fixed_height(52);

    auto& notify_button = notify_group.add<GUI::Button>("Send test notification"_string);
    notify_button.set_fixed_width(180);
    notify_button.on_click = [](auto) {
        (void)send_command("toast Serenity notification system is up"sv);
    };
    auto& notify_note = notify_group.add<GUI::Label>("org.freedesktop.Notifications"_string);
    notify_note.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    // ── Poll the recorder ───────────────────────────────────────────────────
    Function<void()> refresh = [&] {
        auto line = send_command("status"sv);
        if (!line.has_value()) {
            path_label.set_text("WindowServer recorder is not reachable"_string);
            record_button.set_enabled(false);
            pause_button.set_enabled(false);
            screenshot_button.set_enabled(false);
            return;
        }
        record_button.set_enabled(true);
        screenshot_button.set_enabled(true);

        auto status = parse_status(*line);
        if (status.state == "recording"sv) {
            record_button.set_text("Stop recording"_string);
            pause_button.set_text("Pause"_string);
            pause_button.set_enabled(true);
            path_label.set_text(to_string(ByteString::formatted("REC {}  ->  {}", format_elapsed(status.elapsed), status.path)));
        } else if (status.state == "paused"sv) {
            record_button.set_text("Stop recording"_string);
            pause_button.set_text("Resume"_string);
            pause_button.set_enabled(true);
            path_label.set_text(to_string(ByteString::formatted("PAUSED {}  ->  {}", format_elapsed(status.elapsed), status.path)));
        } else {
            record_button.set_text("Start recording"_string);
            pause_button.set_text("Pause"_string);
            pause_button.set_enabled(false);
            if (status.path.is_empty())
                path_label.set_text("Idle"_string);
            else
                path_label.set_text(to_string(ByteString::formatted("Last: {}", status.path)));
        }
        if (status.fps > 0 && fps_spinbox.value() != status.fps)
            fps_spinbox.set_value(status.fps, GUI::AllowCallback::No);
    };

    // Keep `refresh` alive in this scope and give the timer its own wrapper;
    // moving it into the timer would leave the direct call below empty, and
    // calling an empty AK::Function is a hard VERIFY failure.
    auto timer = Core::Timer::create_repeating(500, [&] { refresh(); });
    timer->start();

    window->on_close = [] {
        Core::EventLoop::current().quit(0);
    };

    window->show();
    refresh();
    return app->exec();
}
