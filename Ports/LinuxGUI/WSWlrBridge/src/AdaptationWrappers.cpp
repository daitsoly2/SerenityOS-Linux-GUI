/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "AdaptationWrappers.h"

#include "detail/EquivalentAliases.h" // [别名] 只在桥内部使用
#include "detail/IndependentState.h"

#include <utility>
#include <vector>

namespace WsWlrBridge::detail {

Adaptation::Adaptation(WsApi const& ws, WlrApi const& wlr, BridgeState& state)
    : m_ws(ws)
    , m_wlr(wlr)
    , m_state(state)
{
}

// ── ① 参数翻译（纯函数，无状态、无决策）──────────────────────────────────────

// [适配] wlr damage 用 buffer 像素坐标，WS 用 surface 逻辑坐标。
// 用 ws_ 侧别名调用 impl 里的唯一实现；同一份实现 wlr_ 侧别名也能调。
Rect Adaptation::translate_buffer_rect(Rect const& rect, int scale)
{
    auto size = ws_scale_size({ rect.width, rect.height }, scale);
    int divisor = scale > 0 ? scale : 1;
    return { rect.x / divisor, rect.y / divisor, size.width, size.height };
}

// [适配] wlroots 的 surface 没有屏幕坐标；位置字段填 kOriginUnspecified，
// 明确交给 Serenity 决定。桥若在这里编一个 (0,0) 或做级联，就是布局决策。
Rect Adaptation::geometry_without_origin(Size buffer_size, int scale)
{
    auto logical = wlr_scale_size(buffer_size, scale);
    return { kOriginUnspecified, kOriginUnspecified, logical.width, logical.height };
}

// ── wlr → ws ────────────────────────────────────────────────────────────────

// [适配] ④ 回调时序：wlroots 的 new_surface 早于 map、也早于首个 buffer。
// WS 此时还不知道有窗口，所以这里只记账，绝不提前 ws_window_create。
void Adaptation::on_new_surface(WlrSurfaceHandle surface)
{
    auto& record = m_state.mirror.ensure(surface);
    record.lifecycle = Lifecycle::New;
    record.id = kInvalidWindowId;
}

// [适配] ② ID 映射 + ④ 回调时序：map 仍可能早于首个 commit，
// 因此只登记尺寸/缩放并标 MapPending，真正的 create 放在首个 commit。
// 这是两侧时序差异的转换，不是"要不要建窗口"的策略。
void Adaptation::on_map(WlrSurfaceHandle surface, Size buffer_size, int scale)
{
    auto& record = m_state.mirror.ensure(surface);
    record.scale = scale > 0 ? scale : 1;
    record.buffer_size = buffer_size;
    record.logical_size = wlr_scale_size(buffer_size, record.scale);
    record.lifecycle = record.id == kInvalidWindowId ? Lifecycle::MapPending : Lifecycle::Mapped;
    flush_pending(surface);
}

void Adaptation::on_commit(WlrSurfaceHandle surface, WlrBufferHandle buffer,
    BufferRef const& ref, Rect const* damage, size_t damage_count, WlrSerial serial)
{
    PendingEvent event;
    event.kind = PendingKind::Commit;
    event.surface = surface;
    event.buffer = buffer;
    event.ref = ref;
    event.serial = serial;
    if (damage && damage_count)
        event.rects.assign(damage, damage + damage_count);

    auto const* record = m_state.mirror.find(surface);
    bool waiting_for_map = !record || record->lifecycle == Lifecycle::New;
    m_state.pending.push(surface, std::move(event));

    // [适配] ④ 时序对齐：map 之前到达的 commit 留在 FIFO 里，map 时按原序补发。
    if (waiting_for_map)
        return;
    flush_pending(surface);
}

void Adaptation::on_damage(WlrSurfaceHandle surface, Rect const* rects, size_t count)
{
    if (!rects || count == 0)
        return;
    std::vector<Rect> raw(rects, rects + count);
    auto const* record = m_state.mirror.find(surface);
    if (record && record->id != kInvalidWindowId) {
        apply_damage(surface, record->id, raw);
        return;
    }
    // [适配] ③ 状态同步：窗口尚未建立，先入 FIFO，建立后按原序补发。
    PendingEvent event;
    event.kind = PendingKind::Damage;
    event.surface = surface;
    event.rects = std::move(raw);
    m_state.pending.push(surface, std::move(event));
}

// [适配] 对象归属差异：wlroots 的 unmap 可逆（wlr_surface 对象仍在），
// WS 的 ws_window_destroy 是终态。wrapper 用「销毁 ws 窗口 + 保留 wlr 侧记录，
// 下次 map 重建并换 generation」吸收这个差异，不重新决定任何位置/层级。
void Adaptation::on_unmap(WlrSurfaceHandle surface)
{
    auto* record = m_state.mirror.find(surface);
    if (!record)
        return;
    if (record->id != kInvalidWindowId) {
        m_ws.ws_window_destroy(record->id);
        m_state.handles.unbind(surface);
        record->id = kInvalidWindowId;
    }
    record->lifecycle = Lifecycle::Unmapped;
}

void Adaptation::on_destroy(WlrSurfaceHandle surface)
{
    auto* record = m_state.mirror.find(surface);
    if (record && record->id != kInvalidWindowId)
        m_ws.ws_window_destroy(record->id);
    m_state.handles.unbind(surface);

    // 销毁时兑现所有未决票据，否则 wlr_buffer 引用会泄漏。
    // 这是生命周期清理，不是"丢弃事件"的策略。
    for (auto const& ticket : m_state.mirror.take_releases_for(surface)) {
        if (ticket.wlr_buffer)
            m_wlr.wlr_buffer_unlock(ticket.wlr_buffer);
        m_ws.ws_buffer_release(ticket.ws_buffer_handle);
    }
    m_state.pending.take(surface);
    m_state.mirror.erase(surface);
}

// [适配] ③ 状态同步：title 常早于 ws 窗口建成；先存进镜像，create 时补发。
void Adaptation::on_title_changed(WlrSurfaceHandle surface, char const* title)
{
    auto& record = m_state.mirror.ensure(surface);
    record.title = title ? title : "";
    if (record.id != kInvalidWindowId)
        m_ws.ws_window_set_title(record.id, record.title.c_str());
}

// ── wlr 请求 → ws 上报（请求，绝不是决定）───────────────────────────────────

// [适配] ② ID 映射。只上报，不调用 ws_window_set_focus：谁获得焦点由 Serenity 定。
void Adaptation::relay_focus_request(WlrSurfaceHandle surface, WlrSerial serial)
{
    auto id = m_state.handles.find_by_surface(surface);
    if (id == kInvalidWindowId)
        return; // 尚无对应 ws 窗口，无法上报；桥不擅自创建窗口
    m_state.serials.remember(serial, id);
    m_ws.ws_note_focus_request(id, serial);
}

void Adaptation::relay_resize_request(WlrSurfaceHandle surface, Size size, WlrSerial serial)
{
    auto id = m_state.handles.find_by_surface(surface);
    if (id == kInvalidWindowId)
        return;
    m_state.serials.remember(serial, id);
    m_ws.ws_note_resize_request(id, &size, serial);
}

// ── ws 决定 → wlr 转发（原样，不改写决定）───────────────────────────────────

// [适配] ② ID 映射：ws 窗口 id → wlr surface 指针。
// activated 是 Serenity 的结论，桥不检查"当前谁激活"、也不翻转它。
void Adaptation::forward_focus_decision(WsWindowId window, bool activated, WlrSerial serial)
{
    auto surface = m_state.handles.find_by_id(window);
    if (!surface)
        return; // 陈旧 id（unmap 之后）：忽略，不猜测替代窗口
    m_state.serials.erase(serial);
    if (activated)
        m_wlr.wlr_seat_keyboard_enter(surface, serial);
    else
        m_wlr.wlr_seat_keyboard_clear_focus(serial);
}

// [适配] ① 参数翻译：Serenity 的 Rect 带屏幕位置，xdg_toplevel 只有尺寸；
// x/y 被丢弃是结构差异，不是桥改了 Serenity 的决定。
void Adaptation::forward_configure_decision(WsWindowId window, Rect const& geometry, WlrSerial serial)
{
    auto surface = m_state.handles.find_by_id(window);
    if (!surface)
        return;
    m_state.serials.erase(serial);
    m_wlr.wlr_toplevel_send_configure(surface, geometry.width, geometry.height, serial);
    if (auto* record = m_state.mirror.find(surface)) {
        record->logical_size = { geometry.width, geometry.height };
        record->buffer_size = { geometry.width * record->scale, geometry.height * record->scale };
    }
}

void Adaptation::forward_close_decision(WsWindowId window)
{
    auto surface = m_state.handles.find_by_id(window);
    if (!surface)
        return;
    m_wlr.wlr_toplevel_send_close(surface);
}

// [适配] 对象归属 / 生命周期：WS 消费完缓冲后回调，此刻才能解锁
// wlr_buffer（wlr 侧引用计数）并归还 WS 侧借用视图。
void Adaptation::on_commit_done(WsCookie cookie)
{
    ReleaseTicket ticket;
    if (!m_state.mirror.take_release(cookie, ticket))
        return;
    if (ticket.wlr_buffer)
        m_wlr.wlr_buffer_unlock(ticket.wlr_buffer);
    m_ws.ws_buffer_release(ticket.ws_buffer_handle);
}

// ── 内部：FIFO 补发 / 建窗 / 提交 ───────────────────────────────────────────

void Adaptation::flush_pending(WlrSurfaceHandle surface)
{
    auto events = m_state.pending.take(surface);
    if (events.empty())
        return;

    std::vector<PendingEvent> deferred;
    bool created_window = false;
    for (auto const& event : events) {
        auto const* record = m_state.mirror.find(surface);
        if (!record || record->lifecycle == Lifecycle::Unmapped || record->lifecycle == Lifecycle::New) {
            deferred.push_back(event);
            continue;
        }
        if (event.kind == PendingKind::Commit) {
            bool had_window = record->id != kInvalidWindowId;
            apply_commit(event);
            if (!had_window)
                created_window = true;
        } else {
            if (record->id == kInvalidWindowId) {
                deferred.push_back(event);
                continue;
            }
            apply_damage(surface, record->id, event.rects);
        }
    }

    // 第二遍：首个 commit 刚把窗口建好，把之前"缺 id"的事件按原序补上。
    if (created_window && !deferred.empty()) {
        auto remaining = std::move(deferred);
        deferred.clear();
        for (auto const& event : remaining) {
            if (event.kind == PendingKind::Commit) {
                apply_commit(event);
            } else {
                auto* record = m_state.mirror.find(surface);
                if (record && record->id != kInvalidWindowId)
                    apply_damage(surface, record->id, event.rects);
                else
                    deferred.push_back(event);
            }
        }
    }
    if (!deferred.empty())
        m_state.pending.requeue_front(surface, deferred);
}

void Adaptation::create_window_for(WlrSurfaceHandle surface)
{
    auto* record = m_state.mirror.find(surface);
    if (!record || record->id != kInvalidWindowId)
        return;
    auto geometry = geometry_without_origin(record->buffer_size, record->scale);
    auto id = m_ws.ws_window_create(&geometry, "");
    if (id == kInvalidWindowId)
        return; // WS 明确拒绝；不是桥的决定
    record->id = id;
    record->lifecycle = Lifecycle::Mapped;
    m_state.handles.bind(surface, id);
    // ③ 状态同步：把建窗前收到的 title 补发过去。
    if (!record->title.empty())
        m_ws.ws_window_set_title(id, record->title.c_str());
}

void Adaptation::apply_commit(PendingEvent const& event)
{
    auto* record = m_state.mirror.find(event.surface);
    if (!record)
        return;
    if (record->id == kInvalidWindowId)
        create_window_for(event.surface);
    record = m_state.mirror.find(event.surface);
    if (!record || record->id == kInvalidWindowId)
        return;

    // 生命周期转换：wlr_buffer 引用计数 → WS 借用视图。
    if (event.buffer)
        m_wlr.wlr_buffer_lock(event.buffer);
    auto cookie = m_ws.ws_window_commit(record->id, &event.ref);
    if (cookie == kInvalidCookie) {
        // WS 同步消费或拒绝：没有后续回调，当场释放两端。
        if (event.buffer)
            m_wlr.wlr_buffer_unlock(event.buffer);
        m_ws.ws_buffer_release(event.ref.handle);
    } else {
        m_state.mirror.expect_release(cookie, { event.buffer, event.surface, event.ref.handle });
    }

    if (!event.rects.empty())
        apply_damage(event.surface, record->id, event.rects);
}

void Adaptation::apply_damage(WlrSurfaceHandle surface, WsWindowId window, std::vector<Rect> const& raw_rects)
{
    auto const* record = m_state.mirror.find(surface);
    int scale = record ? record->scale : 1;
    std::vector<Rect> translated;
    translated.reserve(raw_rects.size());
    for (auto const& rect : raw_rects)
        translated.push_back(translate_buffer_rect(rect, scale));
    // 不合并、不裁剪、不丢弃：一进一出保持原序，这些都属于策略。
    m_ws.ws_window_damage(window, translated.data(), translated.size());
}

}
