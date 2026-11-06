/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGUI/Painter.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>

// Colour set of the 千千静听 windows. Skins override these; when no skin is
// selected the built-in "classic" defaults below are used.
struct TTThemeColors {
    Gfx::Color window_background { Gfx::Color::from_rgb(0x0e1a2c) };
    Gfx::Color panel_top { Gfx::Color::from_rgb(0x1d3559) };
    Gfx::Color panel_bottom { Gfx::Color::from_rgb(0x101f36) };
    Gfx::Color inset_background { Gfx::Color::from_rgb(0x060d18) };
    // Self-drawn title strip of the frameless windows. Never derive this by
    // multiplying a light colour: darkened()/lightened() multiply channels and
    // can wrap past 255 (a pale blue turns into bright red).
    Gfx::Color title_strip { Gfx::Color::from_rgb(0x16283f) };
    Gfx::Color bevel_light { Gfx::Color::from_rgb(0x4a7ab5) };
    Gfx::Color bevel_mid { Gfx::Color::from_rgb(0x2a4a74) };
    Gfx::Color bevel_dark { Gfx::Color::from_rgb(0x050a12) };
    Gfx::Color text { Gfx::Color::from_rgb(0xdce9f8) };
    Gfx::Color dim_text { Gfx::Color::from_rgb(0x93aecb) };
    Gfx::Color faint_text { Gfx::Color::from_rgb(0x6d87a6) };
    Gfx::Color accent { Gfx::Color::from_rgb(0x5fe08a) };
    Gfx::Color accent_dim { Gfx::Color::from_rgb(0x1c4a34) };
    Gfx::Color button_face { Gfx::Color::from_rgb(0x24405f) };
    Gfx::Color button_hover { Gfx::Color::from_rgb(0x325c88) };
    Gfx::Color button_pressed { Gfx::Color::from_rgb(0x14283f) };
    Gfx::Color progress_fill { Gfx::Color::from_rgb(0x4fb6f0) };
    // Text drawn on top of the dark inset panels (info box, status strips).
    // Skins with a light background still keep dark insets, so these are kept
    // separate from `text`/`dim_text`.
    Gfx::Color inset_text { Gfx::Color::from_rgb(0xdce9f8) };
    Gfx::Color inset_dim_text { Gfx::Color::from_rgb(0x93aecb) };
    Gfx::Color current_row { Gfx::Color::from_rgb(0xbfd9f2) };
    Gfx::Color current_row_text { Gfx::Color::from_rgb(0x08131f) };
};

// Shared painting helpers. All colours are read from the active theme at paint
// time so switching skins only needs a repaint, not a window rebuild.
namespace TTSkin {

TTThemeColors const& colors();
void set_colors(TTThemeColors const&);

void paint_panel(GUI::Painter&, Gfx::IntRect, Gfx::Bitmap const* background = nullptr);
void paint_inset(GUI::Painter&, Gfx::IntRect);
void paint_button(GUI::Painter&, Gfx::IntRect, bool hovered, bool pressed);
Gfx::Color parse_color(StringView value, Gfx::Color fallback);

}
