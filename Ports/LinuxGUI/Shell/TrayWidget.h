/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibGUI/Widget.h>

// The StatusNotifierItem strip inside the taskbar tray.
//
// It has a bounded view width: apps may register as many icons as they like,
// but the tray is only allowed a slice of the taskbar (the window buttons need
// the rest). Beyond that width the strip scrolls, by wheel or with the small
// arrow buttons the parent puts on either side.
class TrayWidget final : public GUI::Widget {
    C_OBJECT(TrayWidget)

public:
    virtual ~TrayWidget() override = default;

    void refresh();

    // Largest number of pixels the strip may take. The parent chooses this
    // from its own budget; the view width is always min(content, max).
    void set_max_view_width(int width);

    int content_width() const;
    int view_width() const { return width(); }
    bool can_scroll() const { return content_width() > view_width(); }
    void scroll_by(int delta);

    Function<void()> on_items_changed;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void mousewheel_event(GUI::MouseEvent& event) override;
    virtual void leave_event(Core::Event& event) override;

private:
    TrayWidget();

    Gfx::IntRect item_rect(int index) const;
    int item_at(Gfx::IntPoint) const;

    static constexpr int s_icon_size = 16;
    static constexpr int s_spacing = 2;
    static constexpr int s_slot = s_icon_size + s_spacing;

    int m_max_view_width { 5 * s_slot + s_spacing };
    int m_scroll_offset { 0 };
    int m_hovered { -1 };
};
