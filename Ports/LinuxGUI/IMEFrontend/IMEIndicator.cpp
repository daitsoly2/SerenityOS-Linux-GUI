/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Client side of the input method tray indicator. It is a plain GUI::Button that
// lives in the taskbar: it reads the current IME state from a status file the
// serenityime addon publishes and asks the addon (through a control socket) to
// switch or toggle input methods. No fcitx code is linked here.

#include "IMEIndicator.h"
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibGUI/Action.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace GUI {

ByteString IMEIndicator::status_directory()
{
    if (auto* from_env = getenv("SERENITY_IME_STATUS_DIR"))
        return ByteString { from_env };
    return "/tmp"sv;
}

bool IMEIndicator::is_keyboard_layout(StringView unique_name)
{
    // fcitx5's keyboard/layout input methods are named "keyboard-<layout>" or
    // "english"; those are direct Latin input, not a CJK input method.
    return unique_name.starts_with("keyboard-"sv)
        || unique_name == "english"sv
        || unique_name == "kbd"sv;
}

IMEIndicator::IMEIndicator()
{
    set_button_style(Gfx::ButtonStyle::Coolbar);
    set_fixed_size(26, 21);
    set_tooltip(String::from_utf8("Input Method"sv).release_value_but_fixme_should_propagate_errors());

    m_timer = Core::Timer::create_repeating(500, [this] {
        refresh();
    });
    m_timer->start();

    on_click = [this](unsigned) {
        open_switch_menu();
    };

    refresh();
}

void IMEIndicator::refresh()
{
    // Status file: "<active 0|1>\t<unique_name>\t<display_name>".
    if (auto status_file = Core::File::open(status_path(), Core::File::OpenMode::Read); !status_file.is_error()) {
        auto buffer = status_file.value()->read_until_eof();
        if (!buffer.is_error()) {
            auto text = String::from_utf8_without_validation(buffer.value().bytes());
            auto lines = text.bytes_as_string_view().split_view('\n');
            auto line_view = lines.is_empty() ? ""sv : lines[0];
            auto fields = line_view.split_view('\t');
            if (fields.size() >= 2) {
                m_active = fields[0].to_number<int>().value_or(0) != 0;
                m_unique_name = fields[1].to_byte_string();
                m_display_name = fields.size() >= 3 ? fields[2].to_byte_string() : m_unique_name;
            }
        }
    }

    // Methods file: one "<unique_name>\t<display_name>" per input method.
    m_methods.clear();
    if (auto methods_file = Core::File::open(methods_path(), Core::File::OpenMode::Read); !methods_file.is_error()) {
        auto buffer = methods_file.value()->read_until_eof();
        if (!buffer.is_error()) {
            auto text = String::from_utf8_without_validation(buffer.value().bytes());
            for (auto const& line_view : text.bytes_as_string_view().split_view('\n')) {
                auto fields = line_view.split_view('\t');
                if (fields.size() == 2)
                    m_methods.append({ fields[0].to_byte_string(), fields[1].to_byte_string() });
            }
        }
    }

    m_tag = (m_active && !is_keyboard_layout(m_unique_name.view())) ? "中"sv : "EN"sv;

    auto tooltip = String::formatted("Input Method: {} ({})", m_display_name.view(), m_active ? "on"sv : "off"sv);
    if (!tooltip.is_error())
        set_tooltip(tooltip.release_value());
    repaint();
}

void IMEIndicator::paint_event(PaintEvent& event)
{
    Painter painter(*this);
    painter.add_clip_rect(event.rect());

    Button::paint_event(event);

    if (m_tag.is_empty())
        return;

    auto const& font = this->font();
    int text_width = static_cast<int>(font.width(m_tag.view()));
    int lh = static_cast<int>(font.preferred_line_height());
    Gfx::IntRect box { (width() - text_width) / 2 - 3, (height() - lh) / 2 - 1, text_width + 6, lh + 2 };

    bool cjk_active = m_active && !is_keyboard_layout(m_unique_name.view());
    if (cjk_active) {
        painter.fill_rect(box, palette().selection());
        painter.draw_text(box, m_tag.view(), Gfx::TextAlignment::Center, palette().selection_text());
    } else {
        painter.draw_text(Gfx::IntRect { 0, 0, width(), height() }, m_tag.view(), Gfx::TextAlignment::Center, palette().disabled_text_front());
    }
}

void IMEIndicator::open_switch_menu()
{
    m_switch_menu = GUI::Menu::construct();

    for (auto const& method : m_methods) {
        auto action = GUI::Action::create(method.display_name, [this, unique = method.unique_name](auto const&) {
            send_command(String::formatted("set {}", unique.view()).release_value_but_fixme_should_propagate_errors());
        });
        m_switch_menu->add_action(*action);
    }

    if (!m_methods.is_empty())
        m_switch_menu->add_separator();

    auto off_action = GUI::Action::create("Turn IME off"sv, [this](auto const&) {
        send_command("toggle off"sv);
    });
    m_switch_menu->add_action(*off_action);

    m_switch_menu->popup(screen_relative_rect().top_left());
}

void IMEIndicator::send_command(StringView command)
{
    auto path = control_socket_path();
    if (path.length() >= sizeof(sockaddr_un::sun_path))
        return;

    auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.characters(), path.length());

    // Fire-and-forget: the addon applies the change and rewrites the status
    // file; the indicator picks it up on the next refresh. A missing addon (no
    // fcitx5 running) is not an error worth surfacing.
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return;
    }

    auto formatted = String::formatted("{}\n", command);
    if (formatted.is_error()) {
        ::close(fd);
        return;
    }
    auto bytes = formatted.value().bytes_as_string_view();
    size_t written = 0;
    while (written < bytes.length()) {
        auto result = ::send(fd, bytes.characters_without_null_termination() + written, bytes.length() - written, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        written += static_cast<size_t>(result);
    }
    ::close(fd);
}

}
