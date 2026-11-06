/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransportBar.h"
#include "TTSkin.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Path.h>

namespace {

void fill_triangle(GUI::Painter& painter, Gfx::FloatPoint a, Gfx::FloatPoint b, Gfx::FloatPoint c, Gfx::Color color)
{
    Gfx::Path path;
    path.move_to(a);
    path.line_to(b);
    path.line_to(c);
    path.close();
    painter.fill_path(path, color);
}

}

void TransportBar::set_playing(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    update();
}

void TransportBar::set_has_track(bool has_track)
{
    if (m_has_track == has_track)
        return;
    m_has_track = has_track;
    update();
}

Gfx::IntRect TransportBar::button_rect(Button button) const
{
    int index = static_cast<int>(button);
    return { 1 + index * (button_width + button_gap), 1, button_width, height() - 2 };
}

Optional<TransportBar::Button> TransportBar::button_at(Gfx::IntPoint position) const
{
    for (int i = 0; i < 6; i++) {
        auto button = static_cast<Button>(i);
        if (button_rect(button).contains(position))
            return button;
    }
    return {};
}

bool TransportBar::is_enabled(Button button) const
{
    switch (button) {
    case Button::Eject:
        return true;
    case Button::Play:
    case Button::Pause:
    case Button::Stop:
    case Button::Previous:
    case Button::Next:
        return m_has_track;
    }
    return false;
}

void TransportBar::draw_glyph(GUI::Painter& painter, Button button, Gfx::IntRect rect, Gfx::Color color) const
{
    auto center = rect.center();
    int cx = center.x();
    int cy = center.y();

    switch (button) {
    case Button::Previous:
        // "|◀◀": bar on the left, two triangles pointing left.
        painter.fill_rect({ cx - 9, cy - 5, 2, 11 }, color);
        fill_triangle(painter, { cx - 1.f, cy - 5.f }, { cx - 1.f, cy + 5.f }, { cx - 7.f, cy + 0.f }, color);
        fill_triangle(painter, { cx + 7.f, cy - 5.f }, { cx + 7.f, cy + 5.f }, { cx + 1.f, cy + 0.f }, color);
        break;
    case Button::Play:
        fill_triangle(painter, { cx - 4.f, cy - 6.f }, { cx - 4.f, cy + 6.f }, { cx + 7.f, cy + 0.f }, color);
        break;
    case Button::Pause:
        painter.fill_rect({ cx - 5, cy - 6, 3, 12 }, color);
        painter.fill_rect({ cx + 2, cy - 6, 3, 12 }, color);
        break;
    case Button::Stop:
        painter.fill_rect({ cx - 5, cy - 5, 11, 11 }, color);
        break;
    case Button::Next:
        // "▶▶|": two triangles pointing right, bar on the right.
        fill_triangle(painter, { cx - 7.f, cy - 5.f }, { cx - 7.f, cy + 5.f }, { cx - 1.f, cy + 0.f }, color);
        fill_triangle(painter, { cx + 1.f, cy - 5.f }, { cx + 1.f, cy + 5.f }, { cx + 7.f, cy + 0.f }, color);
        painter.fill_rect({ cx + 9, cy - 5, 2, 11 }, color);
        break;
    case Button::Eject:
        fill_triangle(painter, { cx - 7.f, cy - 1.f }, { cx + 7.f, cy - 1.f }, { cx + 0.f, cy - 7.f }, color);
        painter.fill_rect({ cx - 6, cy + 1, 13, 3 }, color);
        break;
    }
}

void TransportBar::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    for (int i = 0; i < 6; i++) {
        auto button = static_cast<Button>(i);
        auto rect = button_rect(button);
        bool const enabled = is_enabled(button);
        bool const hovered = m_hovered.has_value() && *m_hovered == button;
        bool const pressed = m_pressed.has_value() && *m_pressed == button;

        TTSkin::paint_button(painter, rect, enabled && hovered, enabled && pressed);

        auto color = enabled ? TTSkin::colors().text : TTSkin::colors().faint_text;
        if (button == Button::Play && m_playing && enabled)
            color = TTSkin::colors().accent;
        draw_glyph(painter, button, rect, color);
    }
}

void TransportBar::mousemove_event(GUI::MouseEvent& event)
{
    auto hovered = button_at(event.position());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
    GUI::Widget::mousemove_event(event);
}

void TransportBar::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }
    auto button = button_at(event.position());
    if (button.has_value() && is_enabled(*button)) {
        m_pressed = button;
        update();
        event.accept();
        return;
    }
    GUI::Widget::mousedown_event(event);
}

void TransportBar::mouseup_event(GUI::MouseEvent& event)
{
    if (m_pressed.has_value()) {
        auto button = *m_pressed;
        m_pressed.clear();
        update();
        if (button_at(event.position()) == button) {
            switch (button) {
            case Button::Previous:
                if (on_previous)
                    on_previous();
                break;
            case Button::Play:
                if (on_play)
                    on_play();
                break;
            case Button::Pause:
                if (on_pause)
                    on_pause();
                break;
            case Button::Stop:
                if (on_stop)
                    on_stop();
                break;
            case Button::Next:
                if (on_next)
                    on_next();
                break;
            case Button::Eject:
                if (on_eject)
                    on_eject();
                break;
            }
        }
        event.accept();
        return;
    }
    GUI::Widget::mouseup_event(event);
}

void TransportBar::leave_event(Core::Event& event)
{
    if (m_hovered.has_value()) {
        m_hovered.clear();
        update();
    }
    GUI::Widget::leave_event(event);
}
