/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinPanel.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Path.h>

namespace {
constexpr int title_strip_height = 14;
constexpr int close_button_size = 13;
}

Gfx::IntRect SkinPanel::close_button_rect() const
{
    return { width() - close_button_size - 3, 2, close_button_size, close_button_size };
}

void SkinPanel::paint_title_strip(GUI::Painter& painter)
{
    if (!m_frameless_style)
        return;

    auto const& colors = TTSkin::colors();
    Gfx::IntRect strip { 0, 0, width(), title_strip_height };
    painter.fill_rect(strip, colors.title_strip);
    painter.draw_line(strip.bottom_left(), strip.bottom_right().translated(-1, 0), colors.bevel_dark);

    auto title = window() ? window()->title() : ByteString {};
    painter.draw_text(Gfx::IntRect { 6, 0, width() - close_button_size - 12, title_strip_height }, title.view(),
        font().bold_variant(), Gfx::TextAlignment::CenterLeft, colors.text);

    // Close button: a small x, brightening on hover is handled by the WM cursor.
    auto rect = close_button_rect();
    TTSkin::paint_button(painter, rect, false, false);
    painter.draw_line(rect.top_left().translated(4, 4), rect.bottom_right().translated(-4, -4), colors.text);
    painter.draw_line(Gfx::IntPoint { rect.right() - 3, rect.y() + 3 }, Gfx::IntPoint { rect.x() + 3, rect.bottom() - 3 }, colors.text);
}

Gfx::Bitmap const* SkinPanel::scaled_background(Gfx::Bitmap const* background, Gfx::IntSize size)
{
    if (!background || background->size() == size)
        return background;

    int skin = SkinTheme::the().current_index();
    if (m_scaled_skin == skin && m_scaled_size == size && m_scaled_background)
        return m_scaled_background.ptr();

    m_scaled_background = nullptr;
    auto scaled = background->scaled_to_size(size);
    if (!scaled.is_error())
        m_scaled_background = scaled.release_value();
    m_scaled_skin = skin;
    m_scaled_size = size;
    return m_scaled_background ? m_scaled_background.ptr() : background;
}

void SkinPanel::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto const* background = scaled_background(SkinTheme::the().background(m_kind), rect().size());
    TTSkin::paint_panel(painter, rect(), background);
    paint_title_strip(painter);
}

void SkinPanel::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }

    if (m_frameless_style && close_button_rect().contains(event.position())) {
        if (on_close_clicked)
            on_close_clicked();
        event.accept();
        return;
    }

    // Dragging anywhere on the skin moves the window, like the original player.
    // The WM takes over the move once we hand it the press position.
    if (auto* w = window()) {
        if (w->is_frameless()) {
            w->start_window_drag(w->position() + event.position());
            event.accept();
            return;
        }
    }

    GUI::Widget::mousedown_event(event);
}

void SkinPanel::context_menu_event(GUI::ContextMenuEvent& event)
{
    if (on_context_menu_request) {
        on_context_menu_request(event);
        event.accept();
        return;
    }
    GUI::Widget::context_menu_event(event);
}
