/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TTSkin.h"
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <LibGUI/Widget.h>

// Small hand-painted steel button used all over the 千千静听 windows
// (playlist toolbar, panel toggles, equalizer switches). Supports an optional
// checkable "latched" state for toggles.
class SkinButton final : public GUI::Widget {
    C_OBJECT(SkinButton)

public:
    virtual ~SkinButton() override = default;

    void set_text(ByteString text);
    ByteString const& text() const { return m_text; }

    void set_text_color(Gfx::Color color)
    {
        m_text_color = color;
        m_has_text_color = true;
        update();
    }

    void set_checkable(bool checkable);
    bool is_checkable() const { return m_checkable; }
    bool is_checked() const { return m_checked; }
    void set_checked(bool checked);

    void set_icon_only() { m_icon_only = true; }

    Function<void()> on_click;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mouseup_event(GUI::MouseEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void leave_event(Core::Event& event) override;

private:
    SkinButton() = default;

    ByteString m_text;
    Gfx::Color m_text_color {};
    bool m_has_text_color { false };
    bool m_checkable { false };
    bool m_checked { false };
    bool m_icon_only { false };
    bool m_hovered { false };
    bool m_pressed { false };
};
