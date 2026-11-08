/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * 桥内建输入法（IME）服务 —— 前端无关的中间层。
 *
 *   Wayland 客户端 ─ text_input_v3 ─┐
 *   X11 客户端     ─ XIM ───────────┤→ ImeService ─→ serenityime socket ─→ fcitx5(引擎)
 *                                   │        │
 *                                   │        └→ Serenity 候选窗（LibGUI 画）
 *                                   └ 上屏文本原路返回（commit_string / XIM commit）
 *
 * 为什么放在桥里：候选窗只实现一次，外观与原生 Serenity 应用完全一致；
 * 不再依赖 fcitx5 的 classicui/xcb/waylandim 决定窗口长什么样、摆在哪。
 * 光标位置由**客户端协议自己报**（text_input_v3 的 cursor_rectangle / XIM 的
 * spot），所以不存在「X root 坐标与 WS 窗口坐标错位」的问题。
 *
 * 引擎侧刻意只用 socket 行协议（`Ports/LinuxGUI/IMEFrontend/README.md`），
 * 不链接 libFcitx5Core：换引擎（ibus/自研）只改 addon，桥不需要重编。
 */
#pragma once

#include <AK/Function.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>

namespace ImeService {

struct Panel {
    String aux;
    String preedit;
    int preedit_cursor { 0 };
    Vector<String> labels;
    Vector<String> texts;
    int cursor_index { -1 };
    bool has_prev { false };
    bool has_next { false };
};

/* 引擎 socket 是否存在（只查文件，不连接）。 */
bool is_available();

/* 焦点进入一个可输文本的区域。caret 是**屏幕坐标**下的光标矩形，
 * 候选窗贴在它下方；尺寸为 0 时退化为「贴着这个点」。 */
void focus_in(Gfx::IntRect const& caret_screen_rect);
/* 放弃当前组合（XIM_RESET_IC / 焦点切换）。 */
void reset();
void focus_out();
void update_caret(Gfx::IntRect const& caret_screen_rect);

/* 把一个按键交给引擎。返回 true 表示引擎消费了它（调用方不得再转发给客户端）。 */
bool handle_key(unsigned keysym, unsigned states, bool release);

/* 候选窗上的鼠标交互。 */
void select_candidate(int index);
void request_page(bool next);

/* 引擎回来的事件。commit/delete 由前端适配层转成 text_input_v3 / XIM 动作。 */
void set_on_commit(Function<void(String const&)>);
void set_on_delete(Function<void(int offset, int size)>);
void set_on_forward_key(Function<void(unsigned keysym, unsigned states, bool release)>);

}
