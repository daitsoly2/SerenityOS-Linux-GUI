/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FlatSlider.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>

void FlatSlider::set_range(int min, int max)
{
    if (max <= min)
        max = min + 1;
    m_min = min;
    m_max = max;
    set_value(clamp(m_value, m_min, m_max));
}

void FlatSlider::set_value(int value, bool notify)
{
    value = clamp(value, m_min, m_max);
    if (m_value == value) {
        update();
        return;
    }
    m_value = value;
    update();
    if (notify && on_change)
        on_change(m_value);
}

Gfx::IntRect FlatSlider::groove_rect() const
{
    auto rect = this->rect();
    return { rect.x() + 3, rect.y() + rect.height() / 2 - 2, rect.width() - 6, 4 };
}

int FlatSlider::value_at(Gfx::IntPoint position) const
{
    auto groove = groove_rect();
    if (groove.width() <= 0)
        return m_min;
    float ratio = static_cast<float>(position.x() - groove.x()) / static_cast<float>(groove.width());
    ratio = clamp(ratio, 0.0f, 1.0f);
    return m_min + static_cast<int>(ratio * static_cast<float>(m_max - m_min) + 0.5f);
}

void FlatSlider::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto groove = groove_rect();
    // Recessed track.
    painter.fill_rect(groove, TTSkin::colors().inset_background);
    painter.draw_line(groove.top_left(), groove.top_right().translated(-1, 0), TTSkin::colors().bevel_dark);
    painter.draw_line(groove.bottom_left().translated(1, -1), groove.bottom_right().translated(-1, -1), TTSkin::colors().bevel_mid);

    float ratio = m_max > m_min ? static_cast<float>(m_value - m_min) / static_cast<float>(m_max - m_min) : 0.0f;
    int fill_width = static_cast<int>(ratio * static_cast<float>(groove.width()));
    if (fill_width > 0) {
        auto fill_color = m_has_fill_color ? m_fill_color : TTSkin::colors().progress_fill;
        painter.fill_rect({ groove.x(), groove.y() + 1, fill_width, groove.height() - 1 },
            m_dragging ? fill_color.lightened() : fill_color);
    }

    if (m_show_grip) {
        int grip_x = groove.x() + fill_width;
        Gfx::IntRect grip { grip_x - 3, rect().y() + 1, 7, rect().height() - 2 };
        grip.intersect(rect());
        TTSkin::paint_button(painter, grip, m_hovered || m_dragging, m_dragging);
    }
}

void FlatSlider::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }
    m_dragging = true;
    set_value(value_at(event.position()), true);
    update();
    event.accept();
}

void FlatSlider::mousemove_event(GUI::MouseEvent& event)
{
    if (m_dragging) {
        set_value(value_at(event.position()), true);
        event.accept();
        return;
    }
    if (!m_hovered) {
        m_hovered = true;
        update();
    }
    GUI::Widget::mousemove_event(event);
}

void FlatSlider::mouseup_event(GUI::MouseEvent& event)
{
    if (m_dragging && event.button() == GUI::Primary) {
        m_dragging = false;
        set_value(value_at(event.position()));
        update();
        if (on_drag_end)
            on_drag_end(m_value);
        event.accept();
        return;
    }
    GUI::Widget::mouseup_event(event);
}

void FlatSlider::leave_event(Core::Event& event)
{
    m_hovered = false;
    update();
    GUI::Widget::leave_event(event);
}
