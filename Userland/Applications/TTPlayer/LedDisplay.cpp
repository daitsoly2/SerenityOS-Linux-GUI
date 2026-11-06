/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LedDisplay.h"
#include "TTSkin.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <stdio.h>

namespace {

// Seven-segment encoding, bits: a=bit6 … g=bit0.
constexpr u8 segment_bits(char digit)
{
    switch (digit) {
    case '0': return 0x7E;
    case '1': return 0x30;
    case '2': return 0x6D;
    case '3': return 0x79;
    case '4': return 0x33;
    case '5': return 0x5B;
    case '6': return 0x5F;
    case '7': return 0x70;
    case '8': return 0x7F;
    case '9': return 0x7B;
    default: return 0;
    }
}

}

LedDisplay::LedDisplay()
{
    set_fixed_size(104, 32);
    start_timer(500);
}

void LedDisplay::set_times(int elapsed_seconds, int total_seconds)
{
    if (m_elapsed == elapsed_seconds && m_total == total_seconds)
        return;
    m_elapsed = elapsed_seconds;
    m_total = total_seconds;
    update();
}

void LedDisplay::set_mode(Mode mode)
{
    m_mode = mode;
    update();
}

void LedDisplay::cycle_mode()
{
    switch (m_mode) {
    case Mode::Elapsed:
        m_mode = Mode::Remaining;
        break;
    case Mode::Remaining:
        m_mode = Mode::Total;
        break;
    case Mode::Total:
        m_mode = Mode::Elapsed;
        break;
    }
    on_mode_change();
    update();
}

void LedDisplay::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() == GUI::Primary) {
        cycle_mode();
        event.accept();
        return;
    }
    GUI::Frame::mousedown_event(event);
}

void LedDisplay::timer_event(Core::TimerEvent&)
{
    m_colon_visible = !m_colon_visible;
    update();
}

void LedDisplay::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto const& c = TTSkin::colors();
    auto inner = frame_inner_rect();
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, inner,
        c.accent_dim, c.inset_background);
    painter.draw_rect(inner, c.accent_dim);

    int shown_seconds = m_elapsed;
    if (m_mode == Mode::Remaining)
        shown_seconds = max(0, m_total - m_elapsed);
    else if (m_mode == Mode::Total)
        shown_seconds = m_total;

    int minutes = shown_seconds / 60;
    int seconds = shown_seconds % 60;
    if (minutes > 99)
        minutes = 99;

    char text[6];
    snprintf(text, sizeof(text), "%02d%02d", minutes, seconds);

    // Derive the segment geometry from the panel size so the display can be
    // used at different window scales.
    int digit_height = max(10, inner.height() - 8);
    int digit_width = max(6, digit_height * 3 / 5);
    int thickness = max(2, digit_height / 7);
    int gap = max(2, digit_width / 5);
    int colon_width = max(4, digit_width / 2);
    int content_width = digit_width * 4 + gap * 3 + colon_width + gap * 2;
    int x = inner.x() + (inner.width() - content_width) / 2;
    int y = inner.y() + (inner.height() - digit_height) / 2;

    auto on_color = c.accent;
    auto off_color = c.accent_dim.darkened(0.6f);

    auto draw_digit = [&](char digit, int dx, int dy) {
        u8 bits = segment_bits(digit);
        int w = digit_width;
        int h = digit_height;
        int t = thickness;

        auto segment_rect = [&](int segment) {
            switch (segment) {
            case 0: // a
                return Gfx::IntRect(dx + t / 2, dy, w - t, t);
            case 1: // b
                return Gfx::IntRect(dx + w - t, dy + t / 2, t, h / 2 - t);
            case 2: // c
                return Gfx::IntRect(dx + w - t, dy + h / 2 + t / 2, t, h / 2 - t);
            case 3: // d
                return Gfx::IntRect(dx + t / 2, dy + h - t, w - t, t);
            case 4: // e
                return Gfx::IntRect(dx, dy + h / 2 + t / 2, t, h / 2 - t);
            case 5: // f
                return Gfx::IntRect(dx, dy + t / 2, t, h / 2 - t);
            case 6: // g
                return Gfx::IntRect(dx + t / 2, dy + h / 2 - t / 2, w - t, t);
            }
            return Gfx::IntRect {};
        };

        for (int s = 0; s < 7; s++) {
            bool on = (bits & (1 << (6 - s))) != 0;
            painter.fill_rect(segment_rect(s), on ? on_color : off_color);
        }
    };

    draw_digit(text[0], x, y);
    draw_digit(text[1], x + digit_width + gap, y);

    int colon_x = x + 2 * digit_width + 2 * gap + gap;
    Gfx::Color dot_color = m_colon_visible ? on_color : off_color;
    painter.fill_rect(Gfx::IntRect(colon_x, y + digit_height / 4, thickness + 1, thickness + 1), dot_color);
    painter.fill_rect(Gfx::IntRect(colon_x, y + digit_height - digit_height / 4 - thickness - 1, thickness + 1, thickness + 1), dot_color);

    int x2 = colon_x + colon_width + gap;
    draw_digit(text[2], x2, y);
    draw_digit(text[3], x2 + digit_width + gap, y);
}
