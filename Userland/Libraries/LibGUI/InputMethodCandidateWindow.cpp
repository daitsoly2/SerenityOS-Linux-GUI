/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibGUI/Event.h>
#include <LibGUI/InputMethodCandidateWindow.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Palette.h>

namespace GUI {

void InputMethodPanelWidget::set_content(String preedit, int preedit_cursor, Vector<String> labels, Vector<String> texts, int cursor_index, String aux, bool has_prev, bool has_next)
{
    m_preedit = move(preedit);
    m_preedit_cursor = preedit_cursor;
    m_labels = move(labels);
    m_texts = move(texts);
    m_cursor_index = cursor_index;
    m_aux = move(aux);
    m_has_prev = has_prev;
    m_has_next = has_next;
    update();
}

Gfx::IntSize InputMethodPanelWidget::preferred_size() const
{
    // 横向候选条（对齐 gedit / fcitx5 经典候选窗的观感）：所有候选项排成一行，
    // 高度固定为一行，右侧留出翻页箭头的位置。
    int const line_height = static_cast<int>(font().preferred_line_height()) + 6;
    auto measure = [&](StringView s) { return static_cast<int>(font().width(s)); };

    int width = 10; // 左右内边距（含 2px 阴影预留）
    if (!m_aux.is_empty())
        width += measure(m_aux.bytes_as_string_view()) + 10;

    for (size_t i = 0; i < m_texts.size(); ++i) {
        StringView label = i < m_labels.size() ? m_labels[i].bytes_as_string_view() : ""sv;
        width += measure(label) + 6 + measure(m_texts[i].bytes_as_string_view()) + 14;
    }

    if (m_has_prev || m_has_next)
        width += 30;

    return { max(width, 48), line_height + 4 }; // +4: 上边 1px + 下边 2px 阴影 + 余量
}

InputMethodPanelWidget::RowLayout InputMethodPanelWidget::compute_layout() const
{
    RowLayout layout;
    int const line_height = static_cast<int>(font().preferred_line_height()) + 6;

    int x = 4;
    if (!m_aux.is_empty())
        x += static_cast<int>(font().width(m_aux.bytes_as_string_view())) + 10;

    for (size_t i = 0; i < m_texts.size(); ++i) {
        StringView label = i < m_labels.size() ? m_labels[i].bytes_as_string_view() : ""sv;
        int const item_width = static_cast<int>(font().width(label)) + 6
            + static_cast<int>(font().width(m_texts[i].bytes_as_string_view())) + 14;
        layout.candidate_rows.append(Gfx::IntRect { x, 1, item_width, line_height });
        x += item_width;
    }

    if (m_has_prev || m_has_next) {
        layout.has_pager = true;
        layout.prev_rect = Gfx::IntRect { x + 2, 1, 13, line_height };
        layout.next_rect = Gfx::IntRect { x + 15, 1, 13, line_height };
    }

    return layout;
}

void InputMethodPanelWidget::paint_event(PaintEvent& event)
{
    Painter painter(*this);
    painter.add_clip_rect(event.rect());

    // 对齐 gedit 里 fcitx5 浅色候选窗的观感：
    //   - 冷灰面板底色（明显区别于白纸，解决之前 #f4f4f4 几乎与白纸同色的问题）
    //   - 2px 下拉阴影，营造"浮层"感
    //   - 选中项用强调蓝填充 + 白字（之前选中项用白底、比底色还亮，等于看不见）
    auto const shadow = Gfx::Color::from_rgb(0x000000).with_alpha(0x33);
    auto const background = Gfx::Color::from_rgb(0xe8edf3);
    auto const border = Gfx::Color::from_rgb(0xb6c2d4);
    auto const text_normal = Gfx::Color::from_rgb(0x1f2a37);
    auto const text_dim = Gfx::Color::from_rgb(0x6b7785);
    auto const selected_fill = Gfx::Color::from_rgb(0x2f6fd0);
    auto const selected_text = Gfx::Color::from_rgb(0xffffff);
    auto const selected_border = Gfx::Color::from_rgb(0x2560b8);
    auto const separator = Gfx::Color::from_rgb(0xd5dde6);

    int const w = width();
    int const h = height();
    auto const panel_rect = Gfx::IntRect { 0, 0, w - 2, h - 2 };

    painter.fill_rect(Gfx::IntRect { 2, 2, w - 2, h - 2 }, shadow);
    painter.fill_rect(panel_rect, background);
    painter.draw_rect(panel_rect, border);
    painter.add_clip_rect(panel_rect);

    int const line_height = static_cast<int>(font().preferred_line_height()) + 6;

    // aux（音节/声调提示，如有）压缩到候选条最左侧；拼音 preedit 已改到编辑器
    // 输入区光标处内联绘制（见 README §内联预编辑），候选窗不再重复画拼音。
    int x = 6;
    if (!m_aux.is_empty()) {
        auto aux = m_aux.bytes_as_string_view();
        int const w = static_cast<int>(font().width(aux));
        painter.draw_text(Gfx::IntRect { x, 1, w, line_height }, aux,
            Gfx::TextAlignment::CenterLeft, text_dim);
        x += w + 10;
    }

    auto layout = compute_layout();
    for (size_t i = 0; i < m_texts.size(); ++i) {
        StringView label = i < m_labels.size() ? m_labels[i].bytes_as_string_view() : ""sv;
        auto text = m_texts[i].bytes_as_string_view();
        bool const selected = static_cast<int>(i) == m_cursor_index;
        auto const row = layout.candidate_rows[i];

        if (selected) {
            painter.fill_rect(row, selected_fill);
            painter.draw_rect(row, selected_border);
        } else if (i + 1 < m_texts.size()) {
            // 候选项之间的细分隔线（对齐 fcitx5 经典浅色候选窗）
            int const sep_x = row.right();
            painter.draw_line({ sep_x, row.y() + 2 }, { sep_x, row.bottom() - 2 }, separator);
        }

        auto const label_color = selected ? selected_text : text_dim;
        auto const text_color = selected ? selected_text : text_normal;
        int const label_width = static_cast<int>(font().width(label));
        painter.draw_text(Gfx::IntRect { row.x() + 7, row.y(), label_width, line_height }, label,
            Gfx::TextAlignment::CenterLeft, label_color);
        painter.draw_text(Gfx::IntRect { row.x() + 7 + label_width + 6, row.y(),
                             static_cast<int>(font().width(text)), line_height },
            text, Gfx::TextAlignment::CenterLeft, text_color);
    }

    if (layout.has_pager) {
        auto const arrow = m_has_prev ? text_dim : Gfx::Color::from_rgb(0xc8c8c8);
        auto const arrow_next = m_has_next ? text_dim : Gfx::Color::from_rgb(0xc8c8c8);
        painter.draw_text(layout.prev_rect, "<"sv, Gfx::TextAlignment::Center, arrow);
        painter.draw_text(layout.next_rect, ">"sv, Gfx::TextAlignment::Center, arrow_next);
    }

    Widget::paint_event(event);
}

void InputMethodPanelWidget::mousedown_event(MouseEvent& event)
{
    auto layout = compute_layout();
    auto const position = event.position();

    for (size_t i = 0; i < layout.candidate_rows.size(); ++i) {
        if (layout.candidate_rows[i].contains(position)) {
            if (on_candidate_clicked)
                on_candidate_clicked(i);
            return;
        }
    }

    if (layout.has_pager) {
        if (layout.prev_rect.contains(position)) {
            if (on_pager_clicked)
                on_pager_clicked(false);
            return;
        }
        if (layout.next_rect.contains(position)) {
            if (on_pager_clicked)
                on_pager_clicked(true);
            return;
        }
    }

    Widget::mousedown_event(event);
}

InputMethodCandidateWindow::InputMethodCandidateWindow(Widget& editor)
    : m_editor(&editor)
{
    // Add the panel as a child of the editor widget. NoFocus means clicking it
    // never pulls keyboard focus off the editor (which would fire focusout and
    // make the engine commit the raw preedit).
    m_panel = &m_editor->add<InputMethodPanelWidget>();
    m_panel->set_focus_policy(FocusPolicy::NoFocus);
    m_panel->set_visible(false);
}

InputMethodCandidateWindow::~InputMethodCandidateWindow()
{
    if (m_editor && m_panel)
        m_editor->remove_child(*m_panel);
}

void InputMethodCandidateWindow::update(String const& preedit, int preedit_cursor, Vector<String> const& labels, Vector<String> const& texts, int cursor_index, Gfx::IntRect const& cursor_rect, String const& aux, bool has_prev, bool has_next)
{
    if (!m_editor || !m_panel)
        return;

    m_panel->set_content(preedit, preedit_cursor, labels, texts, cursor_index, aux, has_prev, has_next);

    auto size = m_panel->preferred_size();

    // cursor_rect is in screen coordinates; the panel lives inside the editor
    // widget, so convert to editor-local space and clamp to its bounds.
    auto origin = m_editor->screen_relative_rect().location();
    int const local_x = cursor_rect.left() - origin.x();
    int const below_y = cursor_rect.bottom() + 2 - origin.y();
    int const above_y = cursor_rect.top() - 2 - size.height() - origin.y();

    auto const bounds = m_editor->rect();

    int y;
    if (below_y + size.height() <= bounds.bottom())
        y = below_y;
    else if (above_y >= bounds.top())
        y = above_y;
    else
        y = max(bounds.top(), bounds.bottom() - size.height());

    int x = local_x;
    if (x + size.width() > bounds.right())
        x = bounds.right() - size.width();
    if (x < bounds.left())
        x = bounds.left();

    m_panel->set_relative_rect(Gfx::IntRect { x, y, size.width(), size.height() });
    m_panel->set_visible(true);
}

void InputMethodCandidateWindow::set_on_candidate_selected(Function<void(int)> callback)
{
    if (m_panel)
        m_panel->on_candidate_clicked = move(callback);
}

void InputMethodCandidateWindow::set_on_page_requested(Function<void(bool)> callback)
{
    if (m_panel)
        m_panel->on_pager_clicked = move(callback);
}

void InputMethodCandidateWindow::hide()
{
    if (m_panel)
        m_panel->set_visible(false);
}

bool InputMethodCandidateWindow::is_visible() const
{
    return m_panel && m_panel->is_visible();
}

}
