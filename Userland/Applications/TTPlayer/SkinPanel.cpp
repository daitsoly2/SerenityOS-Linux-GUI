/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinPanel.h"
#include <AK/Format.h>
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
        font().bold_variant(), Gfx::TextAlignment::CenterLeft, colors.title_text);

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
    sync_native_layout();
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    if (m_canvas.has_value()) {
        // Native Skin.xml: the canvas owns the whole panel. LibGUI children
        // are hidden (see sync_native_layout) so nothing paints over it, and
        // the hand-drawn title strip is replaced by the skin's own chrome.
        m_canvas->paint(painter, event.rect(), font(), size());
        return;
    }

    auto const* background = scaled_background(SkinTheme::the().background(m_kind), rect().size());
    bool shaped = SkinTheme::the().transparent_color().has_value();
    TTSkin::paint_panel(painter, rect(), background, !shaped);
    paint_title_strip(painter);
}

void SkinPanel::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }

    if (m_canvas.has_value() && m_canvas->mousedown(event.position(), true)) {
        event.accept();
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

void SkinPanel::set_children_visible(bool visible)
{
    bool hidden = !visible;
    if (m_children_hidden == hidden)
        return;
    m_children_hidden = hidden;
    for_each_child_widget([this, visible](GUI::Widget& child) {
        // A content view that belongs to the normal window (playlist rows,
        // lyrics) toggles with the rest, so the classic LibGUI layout keeps
        // working. A native-only overlay (the spectrum, which has no slot in
        // the classic layout) is never shown there.
        if (&child == m_content_widget) {
            child.set_visible(visible && m_content_in_classic_layout);
            return IterationDecision::Continue;
        }
        child.set_visible(visible);
        return IterationDecision::Continue;
    });
    update();
}

void SkinPanel::set_content_widget(GUI::Widget& widget, StringView element_name, ContentFill fill, bool part_of_classic_layout)
{
    m_content_widget = &widget;
    m_content_element = element_name.to_byte_string();
    m_content_fill = fill;
    m_content_in_classic_layout = part_of_classic_layout;
    m_content_rect = {};
    if (!part_of_classic_layout)
        widget.set_visible(false);
    layout_content_widget();
}

// The native list box (`<playlist position="9,50,259,82">`) only marks where
// the rows start and how wide they are; the box itself runs to the panel's
// bottom edge. The left inset doubles as the right/bottom margin, which is
// what the original player does with a resizable list.
void SkinPanel::layout_content_widget()
{
    if (!m_content_widget || !m_canvas.has_value())
        return;
    auto bounds = m_canvas->element_bounds(m_content_element.view());
    if (!bounds.has_value())
        return;

    int inset = max(0, bounds->x());
    Gfx::IntRect target = *bounds;
    if (m_content_fill == ContentFill::ExtendToPanel) {
        target.set_width(max(0, width() - bounds->x() - inset));
        target.set_height(max(0, height() - bounds->y() - inset));
    }
    if (target == m_content_rect && m_content_widget->relative_rect() == target && m_content_widget->is_visible())
        return;
    m_content_rect = target;
    m_content_widget->set_visible(true);
    m_content_widget->set_relative_rect(target);
}

void SkinPanel::did_layout()
{
    GUI::Widget::did_layout();
    layout_content_widget();
}

void SkinPanel::sync_native_layout()
{
    auto const* document = SkinTheme::the().native_document();
    SkinPanelLayout const* layout = nullptr;
    if (document) {
        switch (m_kind) {
        case SkinTheme::WindowKind::Main:
            layout = &document->main;
            break;
        case SkinTheme::WindowKind::Playlist:
            layout = &document->playlist;
            break;
        case SkinTheme::WindowKind::Lyrics:
            layout = &document->lyrics;
            break;
        case SkinTheme::WindowKind::Equalizer:
            layout = &document->equalizer;
            break;
        }
    }

    if (!document || !layout || !layout->valid) {
        if (m_canvas.has_value()) {
            m_canvas.clear();
            set_children_visible(true);
        }
        m_canvas_skin = -2;
        return;
    }

    if (!m_canvas.has_value()) {
        m_canvas.emplace();
        m_canvas->on_invalidate = [this] { update(); };
        m_canvas->on_action = [this](StringView name) {
            if (on_native_action)
                on_native_action(name);
        };
        m_canvas->on_slider = [this](StringView name, float ratio) {
            if (on_native_slider)
                on_native_slider(name, ratio);
        };
        set_children_visible(false);
    }

    int skin = SkinTheme::the().current_index();
    if (m_canvas_skin != skin) {
        // Copy, never a pointer: rescan() rebuilds the skin vector on every
        // skin-menu popup and would leave us holding a dangling reference.
        m_canvas->set_layout(*layout);
        m_canvas_skin = skin;
        m_content_rect = {};
        if (on_native_canvas_setup)
            on_native_canvas_setup(*m_canvas);
    }
    layout_content_widget();
}

void SkinPanel::refresh_native_layout()
{
    m_canvas_skin = -2;
    update();
}

void SkinPanel::mouseup_event(GUI::MouseEvent& event)
{
    if (m_canvas.has_value() && m_canvas->mouseup(event.position(), event.button() == GUI::Primary)) {
        event.accept();
        return;
    }
    GUI::Widget::mouseup_event(event);
}

void SkinPanel::mousemove_event(GUI::MouseEvent& event)
{
    if (m_canvas.has_value() && m_canvas->mousemove(event.position())) {
        event.accept();
        return;
    }
    GUI::Widget::mousemove_event(event);
}

void SkinPanel::leave_event(Core::Event& event)
{
    if (m_canvas.has_value())
        m_canvas->leave();
    GUI::Widget::leave_event(event);
}
