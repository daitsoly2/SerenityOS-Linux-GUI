/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SkinDocument.h"
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>

namespace Gfx {
class Font;
}
namespace GUI {
class Painter;
}

// Paint/hit-test engine for one native-skin panel. Deliberately NOT a Widget:
// all four player windows already use SkinPanel as their root, so SkinPanel
// owns one of these and delegates to it. That keeps the ~120 existing widget
// member accesses intact - the LibGUI children are simply hidden while a
// native skin is active.
//
// The canvas keeps its OWN COPY of the layout: SkinTheme::rescan() rebuilds
// its skin vector whenever the skin menu is opened, so holding a pointer into
// it would dangle on the next menu popup.
class SkinCanvas {
public:
    SkinCanvas() = default;

    void set_layout(SkinPanelLayout layout);
    bool has_layout() const { return m_layout.has_value(); }

    // Content for text elements (info/status/led/lyric/playlist/...).
    void set_element_text(StringView name, ByteString text);
    // 0..1 for progress/volume/eq sliders.
    void set_element_value(StringView name, float ratio);
    void set_element_enabled(StringView name, bool enabled);

    // Called by the host whenever the visual state changes.
    Function<void()> on_invalidate;
    Function<void(StringView name)> on_action;              // button click
    Function<void(StringView name, float ratio)> on_slider; // slider drag

    void paint(GUI::Painter&, Gfx::IntRect const& clip, Gfx::Font const& default_font, Gfx::IntSize panel_size);

    // Bounds of a named skin element, in panel coordinates. Used to place the
    // app's own content view (playlist rows, lyrics) inside the skin's list box.
    Optional<Gfx::IntRect> element_bounds(StringView name) const;

    // Returns true when the canvas consumed the event (a control was hit).
    bool mousemove(Gfx::IntPoint);
    bool mousedown(Gfx::IntPoint, bool primary_button);
    bool mouseup(Gfx::IntPoint, bool primary_button);
    void leave();

private:
    Optional<Gfx::Color> mask_color() const { return m_layout.has_value() ? m_layout->transparent_color : Optional<Gfx::Color> {}; }

    SkinElement const* hit_test(Gfx::IntPoint) const;
    SkinElement const* find(StringView name) const;
    void paint_background(GUI::Painter&, SkinPanelLayout const&, Gfx::IntSize panel_size);
    bool is_slider(SkinElement const&) const;
    float ratio_at(SkinElement const&, Gfx::IntPoint) const;
    int frame_state(SkinElement const&) const;
    void invalidate();
    void paint_element(GUI::Painter&, Gfx::Font const&, SkinElement const&);
    void paint_led(GUI::Painter&, SkinElement const&);
    void paint_slider(GUI::Painter&, SkinElement const&);
    void paint_text(GUI::Painter&, Gfx::Font const&, SkinElement const&);

    Optional<SkinPanelLayout> m_layout;
    HashMap<ByteString, ByteString> m_texts;
    HashMap<ByteString, float> m_values;
    HashMap<ByteString, bool> m_disabled;
    ByteString m_hovered;
    ByteString m_pressed;
    ByteString m_dragging;
};
