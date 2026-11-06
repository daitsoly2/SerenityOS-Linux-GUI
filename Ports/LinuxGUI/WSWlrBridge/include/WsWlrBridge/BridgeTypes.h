/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 桥的「共同词汇表」。
//
// 这是桥自己定义的 POD，用来在 ws_* (Serenity WindowServer) 与
// wlr_* (labwc/wlroots) 之间传值。刻意不包含任何一侧的头文件：
//   * 桥不应该把 wlr_surface / WindowServer::Window 的类型泄漏给对方；
//   * 现阶段只改桥接层，不修改 WindowServer 核心，也就不依赖它的内部类型。
// 生产接线时，两侧的 extern "C" shim 负责把这些 POD 翻成各自的原生结构。
// ─────────────────────────────────────────────────────────────────────────────

#include <climits>
#include <cstddef>
#include <cstdint>

namespace WsWlrBridge {

// wlr 侧对象指针。桥只把它当不透明 key 使用，**绝不持有其所有权**：
// 创建/销毁都由 labwc/wlroots 负责，桥里存的只是映射关系。
using WlrSurfaceHandle = void*;
using WlrBufferHandle = void*;

// ws 侧对象 id。由 ws_window_create() 签发，桥同样只做记录。
using WsWindowId = uint32_t;
using WsCookie = uint32_t;
using WlrSerial = uint32_t;

constexpr WsWindowId kInvalidWindowId = 0;
constexpr WsCookie kInvalidCookie = 0;

// 位置未知哨兵。wlroots 的 surface 本身没有屏幕坐标，位置只能由
// Serenity 决定；桥在 ws_window_create 里填这个哨兵，表示「此处请勿决定位置」。
// 这是规则 3（桥不做布局决策）在数据类型上的体现。
constexpr int kOriginUnspecified = INT_MIN;

struct Rect {
    int x { 0 };
    int y { 0 };
    int width { 0 };
    int height { 0 };
};

struct Size {
    int width { 0 };
    int height { 0 };
};

// 一次提交的缓冲描述。borrowed view，不拥有 wlr_buffer。
struct BufferRef {
    uintptr_t handle { 0 };
    int width { 0 };
    int height { 0 };
    int stride { 0 };
    uint32_t format { 0 };
};

inline bool operator==(Rect const& a, Rect const& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

inline bool operator!=(Rect const& a, Rect const& b) { return !(a == b); }

inline bool operator==(Size const& a, Size const& b)
{
    return a.width == b.width && a.height == b.height;
}

}
