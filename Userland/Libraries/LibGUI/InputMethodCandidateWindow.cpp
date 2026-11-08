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

    /* 双行：第一行拼音 preedit（+ 引擎给的 aux），第二行候选。
     * 单行时拼音和候选挤在一起，preedit 会被候选挤没（历史上干脆被删掉过），
     * 这里恢复成经典输入法的两行观感。 */
    bool const two_line = !m_preedit.is_empty();
    int const preedit_row_height = two_line ? line_height : 0;

    int width = 10; // 左右内边距（含 2px 阴影预留）
    if (!m_aux.is_empty() && !two_line)
        width += measure(m_aux.bytes_as_string_view()) + 10;

    for (size_t i = 0; i < m_texts.size(); ++i) {
        StringView label = i < m_labels.size() ? m_labels[i].bytes_as_string_view() : ""sv;
        width += measure(label) + 6 + measure(m_texts[i].bytes_as_string_view()) + 14;
    }

    if (m_has_prev || m_has_next)
        width += 30;

    if (two_line) {
        /* 拼音行的宽度：和候选行共用同一个左起点（见 paint_event 的 preedit_x）。 */
        int preedit_row = 11 + measure(m_preedit.bytes_as_string_view()) + 8;
        if (!m_aux.is_empty())
            preedit_row += measure(m_aux.bytes_as_string_view()) + 6;
        width = max(width, preedit_row);
    }

    return { max(width, 48), line_height + preedit_row_height + 4 }; // +4: 上边 1px + 下边 2px 阴影 + 余量
}

InputMethodPanelWidget::RowLayout InputMethodPanelWidget::compute_layout() const
{
    RowLayout layout;
    int const line_height = static_cast<int>(font().preferred_line_height()) + 6;

    bool const two_line = !m_preedit.is_empty();
    int const row_y = two_line ? line_height + 1 : 1;

    /* 双行时 aux 画在第一行（拼音后面），候选行必须从最左边开始，
     * 否则第二行整体右移一段，和拼音左边缘对不齐。 */
    int x = 4;
    if (!m_aux.is_empty() && !two_line)
        x += static_cast<int>(font().width(m_aux.bytes_as_string_view())) + 10;

    for (size_t i = 0; i < m_texts.size(); ++i) {
        StringView label = i < m_labels.size() ? m_labels[i].bytes_as_string_view() : ""sv;
        int const item_width = static_cast<int>(font().width(label)) + 6
            + static_cast<int>(font().width(m_texts[i].bytes_as_string_view())) + 14;
        layout.candidate_rows.append(Gfx::IntRect { x, row_y, item_width, line_height });
        x += item_width;
    }

    if (m_has_prev || m_has_next) {
        layout.has_pager = true;
        layout.prev_rect = Gfx::IntRect { x + 2, row_y, 13, line_height };
        layout.next_rect = Gfx::IntRect { x + 15, row_y, 13, line_height };
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

    /* 第一行：拼音 preedit（带内联光标）。aux（首选词预览）跟在它后面用弱色。
     * 以前这一行被删掉、指望在编辑器里内联绘制拼音，但那条路没实现，
     * 结果就是「打字看不到拼音」。 */
    /* 11 = 候选行 label 的绘制起点（row.x()+7，row.x()==4），保证两行左对齐。 */
    int x = 11;
    if (!m_preedit.is_empty()) {
        auto preedit = m_preedit.bytes_as_string_view();
        int const w = static_cast<int>(font().width(preedit));
        painter.draw_text(Gfx::IntRect { x, 1, w, line_height }, preedit,
            Gfx::TextAlignment::CenterLeft, text_normal);

        /* 内联光标：m_preedit_cursor 是字符下标，按前缀宽度定位。 */
        int const cursor_index = clamp(m_preedit_cursor, 0, static_cast<int>(preedit.length()));
        int const caret_x = x + static_cast<int>(font().width(preedit.substring_view(0, cursor_index)));
        painter.fill_rect(Gfx::IntRect { caret_x, 3, 1, line_height - 6 }, text_normal);
        x += w + 6;
    }
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
