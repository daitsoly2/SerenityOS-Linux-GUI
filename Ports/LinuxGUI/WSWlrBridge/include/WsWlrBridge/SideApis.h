/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 两侧 API 契约（只声明，不实现；不修改 WindowServer 核心代码）。
//
// WsApi  = 桥「调用」Serenity WindowServer 的 ws_* 接口
// WlrApi = 桥「调用」labwc/wlroots 的 wlr_* 接口
//
// 用函数指针表而不是直接 include 两侧头文件，有三个目的：
//   1. 桥与两侧的原生类型解耦，规则 5（现阶段只改桥）得以成立；
//   2. 自检可以注入记录用的 stub，验证「桥没有偷偷做决策」；
//   3. 生产接线只需一个 extern "C" shim 填表（见 src/RealAbi.cpp）。
// ─────────────────────────────────────────────────────────────────────────────

#include "BridgeTypes.h"

namespace WsWlrBridge {

struct WsApi {
    // —— 桥 → WindowServer：纯操作（不含决策）——
    WsWindowId (*ws_window_create)(Rect const* geometry, char const* title);
    void (*ws_window_destroy)(WsWindowId);
    void (*ws_window_set_title)(WsWindowId, char const* title);
    void (*ws_window_damage)(WsWindowId, Rect const* rects, size_t count);
    // 异步提交：WS 消费完缓冲后会回调 Bridge::on_ws_commit_done(cookie)。
    // wlr_buffer 的解锁必须等这个回调，不能提前（生命周期差异 → 状态同步）。
    WsCookie (*ws_window_commit)(WsWindowId, BufferRef const*);
    void (*ws_buffer_release)(uintptr_t buffer_handle);

    // —— 桥 → WindowServer：只上报「客户端请求」，由 Serenity 决定结果 ——
    void (*ws_note_focus_request)(WsWindowId, WlrSerial);
    void (*ws_note_resize_request)(WsWindowId, Size const*, WlrSerial);

    // —— 禁止调用（WindowServer 内部决策接口）——
    // 列出它们不是为了用，而是为了在自检里检测越权：桥一旦调用这两个函数，
    // 就意味着桥自己决定了焦点或布局，违反规则 3。selftest 会断言调用次数为 0。
    void (*ws_window_set_focus)(WsWindowId, bool activated);
    void (*ws_window_set_geometry)(WsWindowId, Rect const*);
};

struct WlrApi {
    // —— 桥 → labwc/wlroots：转发 Serenity 的决定 / 管理缓冲所有权 ——
    void (*wlr_buffer_lock)(WlrBufferHandle);
    void (*wlr_buffer_unlock)(WlrBufferHandle);
    void (*wlr_toplevel_send_configure)(WlrSurfaceHandle, int width, int height, WlrSerial);
    void (*wlr_toplevel_send_close)(WlrSurfaceHandle);
    void (*wlr_seat_keyboard_enter)(WlrSurfaceHandle, WlrSerial);
    void (*wlr_seat_keyboard_clear_focus)(WlrSerial);
    void (*wlr_surface_send_frame_done)(WlrSurfaceHandle, uint32_t timestamp_ms);
};

// 生产接线：把真实 ws_* / wlr_* 符号填进两张表。
// 未定义 WSWLR_BIND_REAL_SYMBOLS 时返回 false（测试用注入的 stub 表）。
bool bind_real_apis(WsApi& ws, WlrApi& wlr);

}
