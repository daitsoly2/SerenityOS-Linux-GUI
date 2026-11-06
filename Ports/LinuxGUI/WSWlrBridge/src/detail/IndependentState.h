/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// [独立] 独立实现 —— 规则 2 后半 / 规则 3
//
// 这一层在 ws_* 和 wlr_* 两侧**都没有对应接口**：它既不是别名，也不是 wrapper，
// 而是桥为了完成「ID 映射 / 状态同步 / 回调时序对齐」必须自己持有的记账状态。
//
// 边界：这里只记账，不决策。
//   * 不保存层叠顺序、不保存"哪个窗口该激活"；
//   * 不推导窗口位置（位置由 Serenity 决定，见 kOriginUnspecified）；
//   * 不排序、不合并、不丢弃事件（合并/丢弃属于策略，桥无权做）。
// ─────────────────────────────────────────────────────────────────────────────

#include "WsWlrBridge/BridgeTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace WsWlrBridge::detail {

// [独立] wlr 对象指针 ↔ ws 窗口 id。桥不拥有任何一方的对象，只持有映射。
class HandleMap {
public:
    void bind(WlrSurfaceHandle surface, WsWindowId id);
    void unbind(WlrSurfaceHandle surface);
    WsWindowId find_by_surface(WlrSurfaceHandle surface) const;
    WlrSurfaceHandle find_by_id(WsWindowId id) const; // 陈旧 id 返回 nullptr
    uint32_t generation_of(WsWindowId id) const;

private:
    struct Binding {
        WsWindowId id;
        uint32_t generation;
    };
    std::unordered_map<WlrSurfaceHandle, Binding> m_by_surface;
    std::unordered_map<WsWindowId, WlrSurfaceHandle> m_by_id;
    std::unordered_map<WsWindowId, uint32_t> m_generations;
    uint32_t m_next_generation { 1 };
};

// [独立] 未决客户端请求的 serial ↔ 窗口 id。
// wlroots 的 request 带 serial，Serenity 稍后才给决定；桥需要把两者对上，
// 但**不根据 serial 推断谁该获得焦点**——它只是配对表。
class SerialMap {
public:
    void remember(WlrSerial serial, WsWindowId window);
    bool resolve(WlrSerial serial, WsWindowId& out) const;
    void erase(WlrSerial serial);

private:
    std::unordered_map<WlrSerial, WsWindowId> m_by_serial;
};

enum class PendingKind : uint8_t { Commit, Damage };

struct PendingEvent {
    PendingKind kind { PendingKind::Commit };
    WlrSurfaceHandle surface { nullptr };
    WlrBufferHandle buffer { nullptr };
    BufferRef ref {};
    WlrSerial serial { 0 };
    std::vector<Rect> rects; // 原样保存，桥不合并、不裁剪
};

// [独立] 回调时序缓冲。wlroots 的 new_surface / map / commit 顺序与
// WindowServer 期望的 create / commit 顺序不一致时，用它保持 FIFO 原序。
class PendingQueue {
public:
    void push(WlrSurfaceHandle surface, PendingEvent event);
    std::vector<PendingEvent> take(WlrSurfaceHandle surface);
    void requeue_front(WlrSurfaceHandle surface, std::vector<PendingEvent> const& events);
    size_t count(WlrSurfaceHandle surface) const;
    size_t total() const;

private:
    std::unordered_map<WlrSurfaceHandle, std::vector<PendingEvent>> m_queues;
};

enum class Lifecycle : uint8_t { New, MapPending, Mapped, Unmapped };

struct WindowRecord {
    WsWindowId id { kInvalidWindowId };
    Lifecycle lifecycle { Lifecycle::New };
    Size buffer_size {};
    Size logical_size {};
    int scale { 1 };
    std::string title;
};

struct ReleaseTicket {
    WlrBufferHandle wlr_buffer { nullptr };
    WlrSurfaceHandle surface { nullptr };
    uintptr_t ws_buffer_handle { 0 };
};

// [独立] 生命周期镜像 + 待释放票据。wlr_buffer 是引用计数对象，
// WS 的 BufferRef 是借用视图，两者释放时机不同，桥必须自己记住中间态。
class StateMirror {
public:
    WindowRecord& ensure(WlrSurfaceHandle surface);
    WindowRecord* find(WlrSurfaceHandle surface);
    WindowRecord const* find(WlrSurfaceHandle surface) const;
    void erase(WlrSurfaceHandle surface);

    void expect_release(WsCookie cookie, ReleaseTicket ticket);
    bool take_release(WsCookie cookie, ReleaseTicket& out);
    // surface 销毁时把尚未兑现的票据取出来，避免 wlr_buffer 引用泄漏。
    std::vector<ReleaseTicket> take_releases_for(WlrSurfaceHandle surface);

    size_t mapped_count() const;
    size_t pending_release_count() const;

private:
    std::unordered_map<WlrSurfaceHandle, WindowRecord> m_windows;
    std::unordered_map<WsCookie, ReleaseTicket> m_releases;
};

// 桥自持状态的聚合，只在 src/*.cpp 里可见。
struct BridgeState {
    HandleMap handles;
    SerialMap serials;
    PendingQueue pending;
    StateMirror mirror;
};

}
