/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "WsWlrBridge/Bridge.h"

#include "AdaptationWrappers.h"
#include "detail/IndependentState.h"

// ─────────────────────────────────────────────────────────────────────────────
// 门面实现。本文件只有一行行的转发：不含 if 判断、不含排序、不含任何
// 「谁该激活 / 放哪 / 谁在上面」的逻辑。规则 3 的落点就在这里。
//
// pImpl 同时保证规则 4：detail::* 的类型、等价别名、内部记账状态一律不出现在
// 对外头文件里，外部只能看到 Bridge.h 的 POD 契约和函数表。
// ─────────────────────────────────────────────────────────────────────────────

namespace WsWlrBridge {

struct Bridge::Impl {
    Impl(WsApi const& ws_api, WlrApi const& wlr_api)
        : ws(ws_api)
        , wlr(wlr_api)
        , adapt(ws, wlr, state)
    {
    }

    WsApi ws;
    WlrApi wlr;
    detail::BridgeState state;
    detail::Adaptation adapt;
};

Bridge::Bridge(WsApi const& ws, WlrApi const& wlr)
    : m_impl(std::make_unique<Impl>(ws, wlr))
{
}

Bridge::~Bridge() = default;

// ── wlr → 适配 wrapper ───────────────────────────────────────────────────────
void Bridge::on_wlr_new_surface(WlrSurfaceHandle surface) { m_impl->adapt.on_new_surface(surface); }

void Bridge::on_wlr_map(WlrSurfaceHandle surface, Size buffer_size, int scale)
{
    m_impl->adapt.on_map(surface, buffer_size, scale);
}

void Bridge::on_wlr_commit(WlrSurfaceHandle surface, WlrBufferHandle buffer,
    BufferRef const& ref, Rect const* damage, size_t damage_count, WlrSerial serial)
{
    m_impl->adapt.on_commit(surface, buffer, ref, damage, damage_count, serial);
}

void Bridge::on_wlr_damage(WlrSurfaceHandle surface, Rect const* rects, size_t count)
{
    m_impl->adapt.on_damage(surface, rects, count);
}

void Bridge::on_wlr_unmap(WlrSurfaceHandle surface) { m_impl->adapt.on_unmap(surface); }
void Bridge::on_wlr_destroy(WlrSurfaceHandle surface) { m_impl->adapt.on_destroy(surface); }

void Bridge::on_wlr_title_changed(WlrSurfaceHandle surface, char const* title)
{
    m_impl->adapt.on_title_changed(surface, title);
}

void Bridge::on_wlr_focus_request(WlrSurfaceHandle surface, WlrSerial serial)
{
    m_impl->adapt.relay_focus_request(surface, serial);
}

void Bridge::on_wlr_resize_request(WlrSurfaceHandle surface, Size size, WlrSerial serial)
{
    m_impl->adapt.relay_resize_request(surface, size, serial);
}

// ── ws 决定 → 适配 wrapper ───────────────────────────────────────────────────
void Bridge::on_ws_focus_decided(WsWindowId window, bool activated, WlrSerial serial)
{
    m_impl->adapt.forward_focus_decision(window, activated, serial);
}

void Bridge::on_ws_configure_decided(WsWindowId window, Rect const& geometry, WlrSerial serial)
{
    m_impl->adapt.forward_configure_decision(window, geometry, serial);
}

void Bridge::on_ws_close_decided(WsWindowId window) { m_impl->adapt.forward_close_decision(window); }
void Bridge::on_ws_commit_done(WsCookie cookie) { m_impl->adapt.on_commit_done(cookie); }

// ── 只读诊断 ────────────────────────────────────────────────────────────────
size_t Bridge::mapped_window_count() const { return m_impl->state.mirror.mapped_count(); }
size_t Bridge::pending_event_count() const { return m_impl->state.pending.total(); }
size_t Bridge::pending_release_count() const { return m_impl->state.mirror.pending_release_count(); }

}
