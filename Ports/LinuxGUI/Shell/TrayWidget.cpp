/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TrayWidget.h"
#include "StatusNotifierWatcher.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Palette.h>

TrayWidget::TrayWidget()
{
    set_fill_with_background_color(false);
    set_tooltip("Linux 托盘"_string);
    refresh();
}

int TrayWidget::content_width() const
{
    auto count = static_cast<int>(StatusNotifierWatcher::the().items().size());
    return count == 0 ? 0 : count * s_slot + s_spacing;
}

void TrayWidget::set_max_view_width(int width)
{
    m_max_view_width = max(s_slot + s_spacing, width);
    refresh();
}

void TrayWidget::refresh()
{
    // Never grow past the budget: the rest of the taskbar belongs to the
    // window buttons. Extra icons are reachable by scrolling instead.
    int wanted = min(content_width(), m_max_view_width);
    if (this->width() != wanted)
        set_fixed_width(wanted);
    m_scroll_offset = clamp(m_scroll_offset, 0, max(0, content_width() - wanted));
    update();
    if (on_items_changed)
        on_items_changed();
}

void TrayWidget::scroll_by(int delta)
{
    int maximum = max(0, content_width() - view_width());
    int offset = clamp(m_scroll_offset + delta, 0, maximum);
    if (offset == m_scroll_offset)
        return;
    m_scroll_offset = offset;
    m_hovered = -1;
    update();
}

Gfx::IntRect TrayWidget::item_rect(int index) const
{
    return { s_spacing + index * s_slot - m_scroll_offset, (height() - s_icon_size) / 2, s_icon_size, s_icon_size };
}

int TrayWidget::item_at(Gfx::IntPoint position) const
{
    auto const& items = StatusNotifierWatcher::the().items();
    for (size_t i = 0; i < items.size(); ++i) {
        auto rect = item_rect(static_cast<int>(i));
        if (rect.contains(position))
            return static_cast<int>(i);
    }
    return -1;
}

void TrayWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    // Clamp to the view: icons scrolled out must not paint over the adjacent
    // taskbar buttons.
    painter.add_clip_rect(rect());

    auto const& items = StatusNotifierWatcher::the().items();
    for (size_t i = 0; i < items.size(); ++i) {
        auto rect = item_rect(static_cast<int>(i));
        if (!rect.intersects(this->rect()))
            continue;
        if (static_cast<int>(i) == m_hovered)
            painter.fill_rect(rect.inflated(1, 1), palette().hover_highlight().with_alpha(0x60));

        auto const& item = items[i];
        if (item.icon) {
            painter.draw_scaled_bitmap(rect, *item.icon, item.icon->rect());
        } else {
            // No pixmap yet: a neutral plate keeps the slot visible instead of
            // shifting the layout once the icon arrives.
            painter.fill_rect(rect, palette().button());
            painter.draw_rect(rect, palette().threed_shadow1());
        }
    }
}

void TrayWidget::mousedown_event(GUI::MouseEvent& event)
{
    int index = item_at(event.position());
    if (index < 0) {
        GUI::Widget::mousedown_event(event);
        return;
    }

    auto const& items = StatusNotifierWatcher::the().items();
    if (index >= static_cast<int>(items.size()))
        return;

    auto screen_position = screen_relative_rect().location().translated(event.position().x(), event.position().y());
    if (event.button() == GUI::Primary)
        StatusNotifierWatcher::the().activate(items[index], screen_position.x(), screen_position.y());
    else if (event.button() == GUI::Middle)
        StatusNotifierWatcher::the().secondary_activate(items[index], screen_position.x(), screen_position.y());
    else if (event.button() == GUI::Secondary)
        StatusNotifierWatcher::the().context_menu(items[index], screen_position.x(), screen_position.y());
    event.accept();
}

void TrayWidget::mousemove_event(GUI::MouseEvent& event)
{
    int index = item_at(event.position());
    if (index != m_hovered) {
        m_hovered = index;
        update();
    }
    if (index >= 0) {
        auto const& items = StatusNotifierWatcher::the().items();
        auto title = items[static_cast<size_t>(index)].title;
        auto text = title.is_empty() ? items[static_cast<size_t>(index)].id : title;
        set_tooltip(MUST(String::from_byte_string(text)));
    }
    GUI::Widget::mousemove_event(event);
}

void TrayWidget::mousewheel_event(GUI::MouseEvent& event)
{
    if (can_scroll()) {
        scroll_by(event.wheel_delta_y() > 0 ? s_slot : -s_slot);
        event.accept();
        return;
    }
    GUI::Widget::mousewheel_event(event);
}

void TrayWidget::leave_event(Core::Event& event)
{
    if (m_hovered != -1) {
        m_hovered = -1;
        update();
    }
    GUI::Widget::leave_event(event);
}
