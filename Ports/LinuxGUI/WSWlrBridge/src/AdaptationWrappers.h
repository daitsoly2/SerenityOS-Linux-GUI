/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// [适配] 适配转换 wrapper —— 规则 2
//
// 只有当两侧在「语义 / 触发时机 / 对象归属 / 回调时序」上不一致时才写这里。
// 每个 wrapper 允许做且只做四件事：
//   ① 参数翻译（坐标系、单位、枚举、结构形状）
//   ② ID 映射（wlr 对象指针 ↔ ws 窗口 id）
//   ③ 状态同步（对象未建立时先记账，建立后补发）
//   ④ 回调时序对齐（先到的事件排队，按原序补发）
//
// 明确禁止（规则 3）：焦点归属、层叠顺序、窗口位置、尺寸策略、事件合并/丢弃。
// 尤其注意两类入口的区别：
//   on_wlr_*_request  → 只是客户端「请求」，一律上报给 Serenity 去决定；
//   forward_*_decision → Serenity 已经决定好了，桥只把决定翻译成 wlr_* 调用。
// ─────────────────────────────────────────────────────────────────────────────

#include "WsWlrBridge/SideApis.h"
#include "detail/IndependentState.h"

#include <cstddef>
#include <vector>

namespace WsWlrBridge::detail {

class Adaptation {
public:
    Adaptation(WsApi const& ws, WlrApi const& wlr, BridgeState& state);

    // [适配] ① 参数翻译：纯函数，无状态、无决策。
    // wlr 的 damage 是 buffer 像素坐标，WS 要的是 surface 逻辑坐标。
    static Rect translate_buffer_rect(Rect const& rect, int scale);
    // [适配] ① 参数翻译：wlroots 的 surface 没有屏幕坐标，位置留哨兵交 Serenity。
    static Rect geometry_without_origin(Size buffer_size, int scale);

    // —— wlr → ws ——
    void on_new_surface(WlrSurfaceHandle surface);
    void on_map(WlrSurfaceHandle surface, Size buffer_size, int scale);
    void on_commit(WlrSurfaceHandle surface, WlrBufferHandle buffer,
        BufferRef const& ref, Rect const* damage, size_t damage_count, WlrSerial serial);
    void on_damage(WlrSurfaceHandle surface, Rect const* rects, size_t count);
    void on_unmap(WlrSurfaceHandle surface);
    void on_destroy(WlrSurfaceHandle surface);
    void on_title_changed(WlrSurfaceHandle surface, char const* title);

    // —— wlr 请求 → ws 上报（不是决定）——
    void relay_focus_request(WlrSurfaceHandle surface, WlrSerial serial);
    void relay_resize_request(WlrSurfaceHandle surface, Size size, WlrSerial serial);

    // —— ws 决定 → wlr 转发（原样，不改写决定内容）——
    void forward_focus_decision(WsWindowId window, bool activated, WlrSerial serial);
    void forward_configure_decision(WsWindowId window, Rect const& geometry, WlrSerial serial);
    void forward_close_decision(WsWindowId window);
    void on_commit_done(WsCookie cookie);

private:
    void flush_pending(WlrSurfaceHandle surface);
    void apply_commit(PendingEvent const& event);
    void apply_damage(WlrSurfaceHandle surface, WsWindowId window, std::vector<Rect> const& raw_rects);
    void create_window_for(WlrSurfaceHandle surface);

    WsApi m_ws;
    WlrApi m_wlr;
    BridgeState& m_state;
};

}
