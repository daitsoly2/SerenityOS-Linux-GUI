/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinButton.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>

void SkinButton::set_text(ByteString text)
{
    if (m_text == text)
        return;
    m_text = move(text);
    update();
}

void SkinButton::set_checkable(bool checkable)
{
    m_checkable = checkable;
    update();
}

void SkinButton::set_checked(bool checked)
{
    if (m_checked == checked)
        return;
    m_checked = checked;
    update();
}

void SkinButton::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    bool const pressed = m_pressed || (m_checkable && m_checked);
    TTSkin::paint_button(painter, rect(), m_hovered, pressed);

    auto color = m_has_text_color ? m_text_color : TTSkin::colors().text;
    if (m_checkable && m_checked)
        color = TTSkin::colors().accent;
    else if (m_hovered)
        color = color.lightened();

    if (m_icon_only)
        return;

    painter.draw_text(rect(), m_text, font(), Gfx::TextAlignment::Center, color);
}

void SkinButton::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }
    m_pressed = true;
    update();
    event.accept();
}

void SkinButton::mouseup_event(GUI::MouseEvent& event)
{
    if (!m_pressed) {
        GUI::Widget::mouseup_event(event);
        return;
    }
    m_pressed = false;
    update();
    if (rect().contains(event.position())) {
        if (m_checkable)
            set_checked(!m_checked);
        if (on_click)
            on_click();
    }
    event.accept();
}

void SkinButton::mousemove_event(GUI::MouseEvent& event)
{
    if (!m_hovered) {
        m_hovered = true;
        update();
    }
    GUI::Widget::mousemove_event(event);
}

void SkinButton::leave_event(Core::Event& event)
{
    m_hovered = false;
    m_pressed = false;
    update();
    GUI::Widget::leave_event(event);
}
