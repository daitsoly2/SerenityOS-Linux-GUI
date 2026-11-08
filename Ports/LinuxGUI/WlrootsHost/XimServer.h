/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * XIM 服务端（X11 客户端的输入法入口）。
 *
 * 为什么需要它：WordCraft / Wine / 老 Qt/GTK2 这些 X11 程序只认 XIM。
 * 桥自己就是输入法前端（ImeService），所以 XIM 只是「再挂一个前端适配器」：
 * XIM 的按键/光标/上屏 都转给 ImeService，候选窗还是那一套 LibGUI 控件。
 *
 * 服务名 `@server=serenity`，客户端用 `XMODIFIERS=@im=serenity`。
 *
 * 当前阶段：注册 + 传输层（能收到客户端的 XIM ClientMessage 并解码出类型），
 * 握手/IC 尚未实现 —— 见 HANDOFF_2026-11-07_BRIDGE_IME.md §8。
 */
#pragma once

#include <AK/Function.h>
#include <LibGfx/Rect.h>

namespace XimServer {

/* 打开独立的 X 连接并抢占 XIM_SERVERS selection。XIM_SERVER=0 可关掉。
 * 返回 false 表示 X 不可用（此时输入法只剩 Wayland 通路）。 */
bool initialize();

/* 由主循环驱动（和 XWM::pump 同一拍）。 */
void pump();

/* 关闭连接、放掉 selection。 */
void shutdown();

/* 供 ImeService 的上屏回调分流：XIM 客户端当前有焦点时，commit 要走
 * IMCommitString，而不是 text_input_v3.commit_string。 */
bool is_active();
/* WS 的键盘焦点是否落在一个 X11 窗口上。
 * XIM 的 IC 焦点是「X 服务器」级的：WS 焦点切到 Wayland 窗口（gedit 等）后，
 * X 侧的 SET_IC_FOCUS 不会自己撤销，commit 仍然会投给那个 X11 客户端。
 * 焦点切换时由桥显式同步这个标志，is_active() 才算真正「这台客户端在收字」。 */
void set_ws_x11_focus(bool);
void commit(char const* utf8_text);

/* X11 root 坐标 → WS 屏幕坐标的换算器。
 * XIM 的 XNSpotLocation 经 xcb_translate_coordinates 得到的是 X root 坐标，
 * 而 WS 把同一个 X 窗口摆在了别处（窗口装饰、级联位置都不同），所以必须由
 * 桥按 (窗口 X root 原点 → 该窗口内容区屏幕原点) 做一次平移。
 * 参数：client window XID、光标的 X root 坐标 (x,y)。 */
using CaretResolver = Function<Gfx::IntRect(unsigned long xid, int root_x, int root_y)>;
void set_caret_resolver(CaretResolver);

}
