/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGUI/Widget.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace GUI {

// 候选字体大小（磅）。设置界面写、候选面板每次刷新时重读，所以改完设置
// 下一次打字就生效，不需要重启应用。三个使用者（原生 LibGUI 候选窗、
// Wayland 桥候选窗、输入法设置里的预览）都是 InputMethodPanelWidget，
// 统一在 set_content() 里应用。
constexpr int default_candidate_font_size = 14;
constexpr int min_candidate_font_size = 8;
constexpr int max_candidate_font_size = 48;

ByteString candidate_font_size_config_path();
int candidate_font_size();
bool set_candidate_font_size(int point_size);

// 仅供输入法设置界面的即时预览：进程级临时覆盖，传 -1 取消。
// 桥和原生应用不会调用它，因此始终读配置文件。
void set_candidate_font_size_preview_override(int point_size);

// The little panel that shows the preedit string and the candidate list.
// Drawn by us (the engine is configured for client-side UI), so it follows the
// active Serenity theme instead of looking like an fcitx window.
//
// Layout (top to bottom):
//   1. aux text  - faint syllable/tone guide from the engine, if any
//   2. preedit   - the in-progress spelling, with an in-string caret
//   3. candidates - one <label> <text> per line, the active one highlighted
//   4. pager     - "<" / ">" hints when more pages exist
//
// The panel is interactive: clicking a candidate row selects it (the input
// method commits it) and clicking a pager hint pages the candidate list. Both
// round-trips go through InputMethod, which forwards them to the engine.
class InputMethodPanelWidget final : public Widget {
    C_OBJECT(InputMethodPanelWidget)
public:
    virtual ~InputMethodPanelWidget() override = default;

    void set_content(String preedit, int preedit_cursor, Vector<String> labels, Vector<String> texts, int cursor_index, String aux, bool has_prev, bool has_next);
    Gfx::IntSize preferred_size() const;

    // Wired by InputMethodCandidateWindow. `on_candidate_clicked` receives the
    // 0-based candidate row index; `on_pager_clicked` receives true for the
    // next-page hint and false for the previous-page hint.
    Function<void(size_t)> on_candidate_clicked;
    Function<void(bool)> on_pager_clicked;

protected:
    virtual void paint_event(PaintEvent&) override;
    virtual void mousedown_event(MouseEvent&) override;

private:
    InputMethodPanelWidget() = default;

    // Geometry shared by paint_event() and mouse_down_event() so hit-testing
    // always matches what is drawn.
    struct RowLayout {
        Vector<Gfx::IntRect> candidate_rows;
        Gfx::IntRect prev_rect;
        Gfx::IntRect next_rect;
        bool has_pager { false };
    };
    RowLayout compute_layout() const;

    String m_preedit;
    int m_preedit_cursor { 0 };
    Vector<String> m_labels;
    Vector<String> m_texts;
    int m_cursor_index { -1 };
    String m_aux;
    bool m_has_prev { false };
    bool m_has_next { false };
};

// Overlay that draws the panel *inside* the editor widget rather than in a
// separate window. A separate window is focusable in the WindowServer, so
// clicking it moved focus off the editor; the resulting focusout made fcitx
// commit the raw preedit instead of the clicked candidate. As a NoFocus child
// of the editor, clicks reach the panel without disturbing keyboard focus.
class InputMethodCandidateWindow {
public:
    explicit InputMethodCandidateWindow(Widget& editor);
    ~InputMethodCandidateWindow();

    void update(String const& preedit, int preedit_cursor, Vector<String> const& labels, Vector<String> const& texts, int cursor_index, Gfx::IntRect const& cursor_rect, String const& aux, bool has_prev, bool has_next);
    void hide();
    bool is_visible() const;

    // Mouse interaction hooks. InputMethod sets these so a click on the panel
    // can be turned into an engine command (select candidate / page).
    void set_on_candidate_selected(Function<void(int)>);
    void set_on_page_requested(Function<void(bool)>);

private:
    Widget* m_editor { nullptr };
    // Owned by the editor widget (add<>()); removed again in the destructor.
    InputMethodPanelWidget* m_panel { nullptr };
};

}
