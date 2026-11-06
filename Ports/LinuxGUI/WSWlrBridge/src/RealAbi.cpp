/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "WsWlrBridge/SideApis.h"

// ─────────────────────────────────────────────────────────────────────────────
// 生产接线（默认关闭）。
//
// 规则 5：现阶段只改桥接层，不动 WindowServer 核心，也不把 wlroots 的内部类型
// 拖进桥。真实 ws_* / wlr_* 由两侧各自的 extern "C" shim 提供，参数是桥自己的
// POD（BridgeTypes.h）。shim 负责把 POD 翻成 struct wlr_surface* / Window&。
//
// 独立测试分支上不定义 WSWLR_BIND_REAL_SYMBOLS，测试注入记录用 stub 表即可，
// 不需要装 wlroots，也不需要编译 WindowServer。
// ─────────────────────────────────────────────────────────────────────────────

#if defined(WSWLR_BIND_REAL_SYMBOLS)
extern "C" {
WsWlrBridge::WsWindowId ws_window_create(WsWlrBridge::Rect const*, char const*);
void ws_window_destroy(WsWlrBridge::WsWindowId);
void ws_window_set_title(WsWlrBridge::WsWindowId, char const*);
void ws_window_damage(WsWlrBridge::WsWindowId, WsWlrBridge::Rect const*, size_t);
WsWlrBridge::WsCookie ws_window_commit(WsWlrBridge::WsWindowId, WsWlrBridge::BufferRef const*);
void ws_buffer_release(uintptr_t);
void ws_note_focus_request(WsWlrBridge::WsWindowId, WsWlrBridge::WlrSerial);
void ws_note_resize_request(WsWlrBridge::WsWindowId, WsWlrBridge::Size const*, WsWlrBridge::WlrSerial);
void ws_window_set_focus(WsWlrBridge::WsWindowId, bool);
void ws_window_set_geometry(WsWlrBridge::WsWindowId, WsWlrBridge::Rect const*);

void wlr_buffer_lock(WsWlrBridge::WlrBufferHandle);
void wlr_buffer_unlock(WsWlrBridge::WlrBufferHandle);
void wlr_toplevel_send_configure(WsWlrBridge::WlrSurfaceHandle, int, int, WsWlrBridge::WlrSerial);
void wlr_toplevel_send_close(WsWlrBridge::WlrSurfaceHandle);
void wlr_seat_keyboard_enter(WsWlrBridge::WlrSurfaceHandle, WsWlrBridge::WlrSerial);
void wlr_seat_keyboard_clear_focus(WsWlrBridge::WlrSerial);
void wlr_surface_send_frame_done(WsWlrBridge::WlrSurfaceHandle, uint32_t);
}
#endif

namespace WsWlrBridge {

bool bind_real_apis(WsApi& ws, WlrApi& wlr)
{
#if defined(WSWLR_BIND_REAL_SYMBOLS)
    ws = WsApi {
        &::ws_window_create,
        &::ws_window_destroy,
        &::ws_window_set_title,
        &::ws_window_damage,
        &::ws_window_commit,
        &::ws_buffer_release,
        &::ws_note_focus_request,
        &::ws_note_resize_request,
        &::ws_window_set_focus,
        &::ws_window_set_geometry,
    };
    wlr = WlrApi {
        &::wlr_buffer_lock,
        &::wlr_buffer_unlock,
        &::wlr_toplevel_send_configure,
        &::wlr_toplevel_send_close,
        &::wlr_seat_keyboard_enter,
        &::wlr_seat_keyboard_clear_focus,
        &::wlr_surface_send_frame_done,
    };
    return true;
#else
    ws = WsApi {};
    wlr = WlrApi {};
    return false;
#endif
}

}
