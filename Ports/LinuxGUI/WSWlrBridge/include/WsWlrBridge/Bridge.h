/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 桥的**唯一对外门面**。
//
// 两类入口，方向相反，但都是转发：
//   on_wlr_*(...)  labwc/wlroots 的事件 → 翻译成 ws_* 操作 / 上报为「请求」
//   on_ws_*(...)   Serenity 已经做出的决定 → 翻译成 wlr_* 调用
//
// 规则 3：这里没有、也不允许有「谁该获得焦点 / 窗口放哪 / 谁在上面」的判断。
// 规则 4：pImpl 把 detail 命名空间、别名、内部状态全部挡在 .cpp 里；
//         本头文件只包含 BridgeTypes / SideApis / Classification 三份契约头。
// ─────────────────────────────────────────────────────────────────────────────

#include "BridgeTypes.h"
#include "Classification.h"
#include "SideApis.h"

#include <cstddef>
#include <memory>

namespace WsWlrBridge {

class Bridge {
public:
    Bridge(WsApi const& ws, WlrApi const& wlr);
    ~Bridge();

    Bridge(Bridge const&) = delete;
    Bridge& operator=(Bridge const&) = delete;

    // —— wlr 侧事件（由 labwc/wlroots 的 listener 直接挂接）——
    void on_wlr_new_surface(WlrSurfaceHandle surface);
    void on_wlr_map(WlrSurfaceHandle surface, Size buffer_size, int scale);
    void on_wlr_commit(WlrSurfaceHandle surface, WlrBufferHandle buffer,
        BufferRef const& ref, Rect const* damage, size_t damage_count, WlrSerial serial);
    void on_wlr_damage(WlrSurfaceHandle surface, Rect const* rects, size_t count);
    void on_wlr_unmap(WlrSurfaceHandle surface);
    void on_wlr_destroy(WlrSurfaceHandle surface);
    void on_wlr_title_changed(WlrSurfaceHandle surface, char const* title);
    // 客户端「请求」，不是决定：桥只上报给 Serenity。
    void on_wlr_focus_request(WlrSurfaceHandle surface, WlrSerial serial);
    void on_wlr_resize_request(WlrSurfaceHandle surface, Size size, WlrSerial serial);

    // —— ws 侧决定（Serenity WindowServer 已决定，桥原样转发到 wlr 侧）——
    void on_ws_focus_decided(WsWindowId window, bool activated, WlrSerial serial);
    void on_ws_configure_decided(WsWindowId window, Rect const& geometry, WlrSerial serial);
    void on_ws_close_decided(WsWindowId window);
    // WS 消费完缓冲后的回调：用来解锁 wlr_buffer（生命周期状态同步）。
    void on_ws_commit_done(WsCookie cookie);

    // 只读诊断，不暴露内部状态类型。
    size_t mapped_window_count() const;
    size_t pending_event_count() const;
    size_t pending_release_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
