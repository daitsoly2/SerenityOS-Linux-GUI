/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TTSkin.h"
#include <AK/CharacterTypes.h>

namespace {

TTThemeColors s_colors;

}

namespace TTSkin {

TTThemeColors const& colors()
{
    return s_colors;
}

void set_colors(TTThemeColors const& colors)
{
    s_colors = colors;
}

Gfx::Color parse_color(StringView value, Gfx::Color fallback)
{
    // Accepts "#rrggbb", "rrggbb", "0xrrggbb" and "#aarrggbb".
    value = value.trim_whitespace();
    if (value.starts_with('#'))
        value = value.substring_view(1);
    else if (value.starts_with("0x"sv) || value.starts_with("0X"sv))
        value = value.substring_view(2);

    if (value.length() != 6 && value.length() != 8)
        return fallback;

    u32 result = 0;
    for (size_t i = 0; i < value.length(); i++) {
        if (!is_ascii_hex_digit(value[i]))
            return fallback;
        result = (result << 4) | static_cast<u32>(parse_ascii_hex_digit(value[i]));
    }
    if (value.length() == 6)
        return Gfx::Color::from_rgb(result);
    return Gfx::Color::from_argb(result);
}

void paint_panel(GUI::Painter& painter, Gfx::IntRect rect, Gfx::Bitmap const* background)
{
    auto const& c = colors();
    if (background) {
        // Skins supply a fixed-size backdrop; paint it 1:1 from the top-left
        // and fill any remaining area with the skin's window colour.
        painter.fill_rect(rect, c.window_background);
        auto source = background->rect().intersected(Gfx::IntRect { {}, rect.size() });
        painter.blit(rect.location(), *background, source);
    } else {
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect, c.panel_top, c.panel_bottom);
    }
    painter.draw_line(rect.top_left(), rect.top_right().translated(-1, 0), c.bevel_light);
    painter.draw_line(rect.top_left(), rect.bottom_left().translated(0, -1), c.bevel_mid);
    painter.draw_line(rect.bottom_left().translated(1, -1), rect.bottom_right().translated(-1, -1), c.bevel_dark);
    painter.draw_line(rect.top_right().translated(-1, 0), rect.bottom_right().translated(-1, -1), c.bevel_dark);
}

void paint_inset(GUI::Painter& painter, Gfx::IntRect rect)
{
    auto const& c = colors();
    painter.fill_rect(rect, c.inset_background);
    painter.draw_line(rect.top_left(), rect.top_right().translated(-1, 0), c.bevel_dark);
    painter.draw_line(rect.top_left(), rect.bottom_left().translated(0, -1), c.bevel_dark);
    painter.draw_line(rect.bottom_left().translated(1, -1), rect.bottom_right().translated(-1, -1), c.bevel_mid);
    painter.draw_line(rect.top_right().translated(-1, 0), rect.bottom_right().translated(-1, -1), c.bevel_mid);
}

void paint_button(GUI::Painter& painter, Gfx::IntRect rect, bool hovered, bool pressed)
{
    auto const& c = colors();
    auto face = pressed ? c.button_pressed : (hovered ? c.button_hover : c.button_face);
    painter.fill_rect(rect, face);
    if (pressed) {
        painter.draw_line(rect.top_left(), rect.top_right().translated(-1, 0), c.bevel_dark);
        painter.draw_line(rect.top_left(), rect.bottom_left().translated(0, -1), c.bevel_dark);
        painter.draw_line(rect.bottom_left().translated(1, -1), rect.bottom_right().translated(-1, -1), c.bevel_mid);
        painter.draw_line(rect.top_right().translated(-1, 0), rect.bottom_right().translated(-1, -1), c.bevel_mid);
    } else {
        painter.draw_line(rect.top_left(), rect.top_right().translated(-1, 0), c.bevel_light);
        painter.draw_line(rect.top_left(), rect.bottom_left().translated(0, -1), c.bevel_light);
        painter.draw_line(rect.bottom_left().translated(1, -1), rect.bottom_right().translated(-1, -1), c.bevel_dark);
        painter.draw_line(rect.top_right().translated(-1, 0), rect.bottom_right().translated(-1, -1), c.bevel_dark);
    }
}

}
