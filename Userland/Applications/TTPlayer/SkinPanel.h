/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SkinTheme.h"
#include "TTSkin.h"
#include <AK/Function.h>
#include <LibGUI/Event.h>
#include <LibGUI/Widget.h>

// Backdrop shared by the 千千静听 windows: paints the active skin's bitmap for
// this window when one is available, otherwise the built-in gradient.
class SkinPanel final : public GUI::Widget {
    C_OBJECT(SkinPanel)

public:
    virtual ~SkinPanel() override = default;

    void set_kind(SkinTheme::WindowKind kind)
    {
        m_kind = kind;
        update();
    }

    // Frameless windows paint their own chrome: a slim title strip with a
    // close button, and the whole background acts as a drag handle.
    void set_frameless_style(bool frameless)
    {
        m_frameless_style = frameless;
        update();
    }

    Function<void()> on_close_clicked;

    // Frameless windows have no system menu bar, so the player exposes its own
    // main menu from a right-click anywhere on the skin. ContextMenuEvent
    // bubbles up through the widget tree, which makes the root panel the one
    // place that needs to handle it.
    Function<void(GUI::ContextMenuEvent&)> on_context_menu_request;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void context_menu_event(GUI::ContextMenuEvent& event) override;

private:
    SkinPanel()
    {
        set_fill_with_background_color(true);
    }

    Gfx::IntRect close_button_rect() const;
    void paint_title_strip(GUI::Painter&);
    Gfx::Bitmap const* scaled_background(Gfx::Bitmap const* background, Gfx::IntSize size);

    SkinTheme::WindowKind m_kind { SkinTheme::WindowKind::Main };
    bool m_frameless_style { true };

    // Skin bitmaps have a fixed size. Windows can be restored at any size, so
    // keep one scaled copy per panel instead of blitting 1:1 and leaving an
    // unpainted band at the right/bottom.
    RefPtr<Gfx::Bitmap> m_scaled_background;
    Gfx::IntSize m_scaled_size;
    int m_scaled_skin { -2 };
};
