/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaylistWindow.h"
#include "SkinPanel.h"
#include "SkinTheme.h"
#include "TTSkin.h"
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/ScrollableContainerWidget.h>
#include <LibGUI/Scrollbar.h>

// ---------------------------------------------------------------------------
// PlaylistListWidget
// ---------------------------------------------------------------------------

void PlaylistListWidget::reload_rows()
{
    int rows = m_core ? m_core->store().size() : 0;
    int height = max(rows * row_height, 1);
    if (this->height() != height)
        set_fixed_height(height);
    update();
}

void PlaylistListWidget::set_current_index(Optional<int> index)
{
    if (m_current == index)
        return;
    m_current = index;
    update();
}

void PlaylistListWidget::select_index(int index)
{
    if (m_selected == index)
        return;
    m_selected = index;
    update();
}

int PlaylistListWidget::row_at(Gfx::IntPoint position) const
{
    if (!m_core)
        return -1;
    int row = position.y() / row_height;
    if (row < 0 || row >= m_core->store().size())
        return -1;
    return row;
}

void PlaylistListWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    painter.fill_rect(rect(), TTSkin::colors().inset_background);
    if (!m_core)
        return;

    auto const& entries = m_core->store().entries();
    if (entries.is_empty()) {
        painter.draw_text(rect(), "播放列表是空的 · 用「添加」导入音乐"sv, font(),
            Gfx::TextAlignment::Center, TTSkin::colors().faint_text);
        return;
    }

    int first_row = event.rect().y() / row_height;
    int last_row = min(static_cast<int>(entries.size()) - 1, (event.rect().bottom() / row_height) + 1);

    auto const& c = TTSkin::colors();
    for (int row = max(0, first_row); row <= last_row; row++) {
        auto const& entry = entries[static_cast<size_t>(row)];
        Gfx::IntRect row_rect { 0, row * row_height, width(), row_height };
        bool const is_current = m_current.has_value() && *m_current == row;
        bool const is_selected = m_selected == row;

        Gfx::Color background;
        Gfx::Color foreground;
        if (is_current) {
            background = TTSkin::colors().current_row;
            foreground = TTSkin::colors().current_row_text;
        } else if (is_selected) {
            background = c.button_hover;
            foreground = c.inset_text;
        } else {
            background = (row % 2 == 0) ? c.inset_background : c.inset_background.lightened(1.35f);
            foreground = c.inset_text;
        }
        painter.fill_rect(row_rect, background);

        // "214." style track number, right aligned in a narrow gutter.
        auto number = ByteString::formatted("{}.", row + 1);
        painter.draw_text(Gfx::IntRect { 4, row_rect.y(), 30, row_height }, number.view(),
            is_current ? font().bold_variant() : font(), Gfx::TextAlignment::CenterRight, foreground);

        auto title = entry.display_title.value_or(LexicalPath::title(entry.path));
        Gfx::IntRect title_rect { 38, row_rect.y(), row_rect.width() - 38 - 52, row_height };
        painter.draw_text(title_rect, title.view(), is_current ? font().bold_variant() : font(),
            Gfx::TextAlignment::CenterLeft, foreground);

        if (entry.duration_seconds.has_value()) {
            auto duration = human_readable_digital_time(*entry.duration_seconds).to_byte_string();
            painter.draw_text(Gfx::IntRect { row_rect.right() - 50, row_rect.y(), 44, row_height }, duration.view(), font(),
                Gfx::TextAlignment::CenterRight, foreground);
        }

        if (is_current) {
            // Small playing marker in front of the title.
            painter.fill_rect({ row_rect.x() + 34, row_rect.y() + 7, 2, 4 }, foreground);
        }
    }
}

void PlaylistListWidget::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }
    int row = row_at(event.position());
    if (row >= 0) {
        select_index(row);
        event.accept();
        return;
    }
    GUI::Widget::mousedown_event(event);
}

void PlaylistListWidget::doubleclick_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::doubleclick_event(event);
        return;
    }
    int row = row_at(event.position());
    if (row >= 0) {
        select_index(row);
        if (on_activate)
            on_activate(row);
        event.accept();
        return;
    }
    GUI::Widget::doubleclick_event(event);
}

void PlaylistListWidget::keydown_event(GUI::KeyEvent& event)
{
    if (!m_core) {
        GUI::Widget::keydown_event(event);
        return;
    }
    int count = m_core->store().size();
    if (count == 0) {
        GUI::Widget::keydown_event(event);
        return;
    }
    if (event.key() == Key_Down) {
        select_index(min(count - 1, m_selected + 1));
        event.accept();
        return;
    }
    if (event.key() == Key_Up) {
        select_index(max(0, m_selected - 1));
        event.accept();
        return;
    }
    if (event.key() == Key_Return && m_selected >= 0) {
        if (on_activate)
            on_activate(m_selected);
        event.accept();
        return;
    }
    GUI::Widget::keydown_event(event);
}

// ---------------------------------------------------------------------------
// PlaylistWindow
// ---------------------------------------------------------------------------

PlaylistWindow::PlaylistWindow(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
    : m_core(move(core))
{
    set_title(ByteString { "播放列表" });
    set_obey_widget_min_size(false);
    set_frameless(true);
    set_minimum_size(280, 180);
    restore_size_and_position("TTPlayer"sv, "PlaylistWindow"sv, Gfx::IntSize { 360, 300 }, fallback_position);
    save_size_and_position_on_close("TTPlayer"sv, "PlaylistWindow"sv);
    build_ui();
}

NonnullRefPtr<PlaylistWindow> PlaylistWindow::create(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
{
    return adopt_ref(*new PlaylistWindow(move(core), fallback_position));
}

void PlaylistWindow::build_ui()
{
    auto root_widget = set_main_widget<SkinPanel>();
    root_widget->set_kind(SkinTheme::WindowKind::Playlist);
    root_widget->on_close_clicked = [this] { hide(); };
    root_widget->on_context_menu_request = [this](auto& event) {
        if (on_context_menu_request)
            on_context_menu_request(event);
    };
    auto& root = *root_widget;
    root.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14, 5, 5 }, 4);

    auto& toolbar = root.add<GUI::Widget>();
    toolbar.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 3);
    toolbar.set_fixed_height(22);

    auto add_button = [&](String text, Function<void()> on_click) {
        auto& button = toolbar.add<SkinButton>();
        button.set_text(text.to_byte_string());
        button.set_fixed_size(44, 20);
        button.on_click = move(on_click);
        return &button;
    };

    add_button("添加"_string, [this] { m_core->add_files(this); });
    add_button("目录"_string, [this] { m_core->add_folder(this); });
    add_button("删除"_string, [this] {
        if (auto selected = m_list->selected_index(); selected.has_value())
            m_core->store().remove_at(*selected);
    });
    add_button("清空"_string, [this] { m_core->store().clear(); });
    add_button("载入"_string, [this] { m_core->open_playlist(this); });
    add_button("保存"_string, [this] { m_core->save_playlist(this); });

    m_mode_button = &toolbar.add<SkinButton>();
    m_mode_button->set_text(ByteString { "循环" });
    m_mode_button->set_fixed_size(52, 20);
    m_mode_button->on_click = [this] {
        m_core->cycle_play_mode();
        refresh();
    };

    auto& list_container = root.add<SkinPanel>();
    list_container.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 0 }, 0);

    m_scroller = list_container.add<GUI::ScrollableContainerWidget>();
    m_scroller->set_should_hide_unnecessary_scrollbars(true);

    m_list = PlaylistListWidget::construct();
    m_list->set_core(*m_core);
    m_list->set_fixed_height(1);
    m_scroller->set_widget(m_list.ptr());
    m_list->on_activate = [this](int index) { m_core->play_index(index); };

    m_status_label = &root.add<SkinText>(SkinText::Role::Dim, 18);
    m_status_label->set_text("共 0 首"sv);

    refresh();
}

void PlaylistWindow::update_status()
{
    auto count = m_core->store().size();
    auto text = ByteString::formatted("共 {} 首", count);
    if (count > 0 && m_core->current_index().has_value()) {
        auto title = m_core->title();
        if (!title.is_empty())
            text = ByteString::formatted("共 {} 首 · 正在播放：{}", count, title);
    }
    m_status_label->set_text(text);
}

void PlaylistWindow::refresh()
{
    m_list->reload_rows();
    m_list->set_current_index(m_core->current_index());
    if (!m_list->selected_index().has_value() && m_core->current_index().has_value())
        m_list->select_index(*m_core->current_index());
    m_mode_button->set_text(PlaylistStore::mode_name(m_core->play_mode()).to_byte_string());
    update_status();
    set_title(ByteString::formatted("播放列表 - {} 首", m_core->store().size()));
}
