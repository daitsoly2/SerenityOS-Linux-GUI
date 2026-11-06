/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TTSkin.h"
#include <AK/Function.h>
#include <LibGUI/Widget.h>

// Flat 1px-bevel slider used for the seek bar and the volume strip. The stock
// GUI::HorizontalSlider paints a chunky 3D knob that does not fit the classic
// 千千静听 skin, so this draws a thin groove with a small steel grip instead.
class FlatSlider final : public GUI::Widget {
    C_OBJECT(FlatSlider)

public:
    virtual ~FlatSlider() override = default;

    void set_range(int min, int max);
    int min_value() const { return m_min; }
    int max_value() const { return m_max; }

    int value() const { return m_value; }
    void set_value(int value, bool notify = false);

    void set_fill_color(Gfx::Color color)
    {
        m_fill_color = color;
        m_has_fill_color = true;
        update();
    }
    void set_show_grip(bool show) { m_show_grip = show; update(); }
    bool is_dragging() const { return m_dragging; }

    Function<void(int)> on_change;
    Function<void(int)> on_drag_end;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void mouseup_event(GUI::MouseEvent& event) override;
    virtual void leave_event(Core::Event& event) override;

private:
    FlatSlider()
    {
        set_fixed_height(14);
    }

    int value_at(Gfx::IntPoint) const;
    Gfx::IntRect groove_rect() const;

    int m_min { 0 };
    int m_max { 100 };
    int m_value { 0 };
    bool m_dragging { false };
    bool m_hovered { false };
    bool m_show_grip { true };
    Gfx::Color m_fill_color {};
    bool m_has_fill_color { false };
};
