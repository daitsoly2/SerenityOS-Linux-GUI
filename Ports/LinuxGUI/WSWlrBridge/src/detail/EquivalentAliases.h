/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// [别名] 等价别名 —— 规则 1 / 规则 4
//
// 适用条件（**全部**满足才允许放这里）：
//   语义相同、参数与返回类型相同、无对象所有权、无生命周期、无回调时序。
// 本文件里的四组操作都是纯值计算，两侧没有任何差异，因此共用 impl:: 中
// 的**同一份实现**，两侧名字只是别名，不另写函数体。
//
// 规则 4 的结构性保证：本文件位于 src/detail/，CMake 里该目录是 PRIVATE
// include path，对外头文件根本看不到；任何外部 TU 也无法 include 到它。
// 对外暴露的只有 Bridge.h 的 on_wlr_* / on_ws_* 入口。
// ─────────────────────────────────────────────────────────────────────────────

#include "WsWlrBridge/BridgeTypes.h"

#include <algorithm>
#include <type_traits>

namespace WsWlrBridge::detail {

// 唯一实现。所有别名都指向这里，禁止在桥的其它地方复制这些函数体。
namespace impl {

inline bool rect_contains_point(Rect const& r, int x, int y) noexcept
{
    // 注意：right()/bottom() 语义是排他边界，逐像素比较一律用 <。
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

inline Rect rect_intersect(Rect const& a, Rect const& b) noexcept
{
    int left = std::max(a.x, b.x);
    int top = std::max(a.y, b.y);
    int right = std::min(a.x + a.width, b.x + b.width);
    int bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top)
        return { left, top, 0, 0 };
    return { left, top, right - left, bottom - top };
}

inline Size scale_size(Size size, int scale) noexcept
{
    int divisor = scale > 0 ? scale : 1;
    return { size.width / divisor, size.height / divisor };
}

inline uint32_t bytes_per_pixel(uint32_t format) noexcept
{
    // 两侧对 ARGB8888 / XRGB8888 的 fourcc 定义一致，纯查表，无状态。
    switch (format) {
    case 0x34325241: // DRM_FORMAT_ARGB8888 ('AR24' LE)
    case 0x34325258: // DRM_FORMAT_XRGB8888 ('XR24' LE)
        return 4;
    default:
        return 0;
    }
}

}

// [别名] wlr_* / ws_* 成对别名。名字保留两侧前缀只是为了在适配 wrapper
// 里一眼看出数据来源；它们不是两个 API，是同一个函数。
inline constexpr auto wlr_rect_contains_point = &impl::rect_contains_point;
inline constexpr auto ws_rect_contains_point = &impl::rect_contains_point;
inline constexpr auto wlr_rect_intersect = &impl::rect_intersect;
inline constexpr auto ws_rect_intersect = &impl::rect_intersect;
inline constexpr auto wlr_scale_size = &impl::scale_size;
inline constexpr auto ws_scale_size = &impl::scale_size;
inline constexpr auto wlr_bytes_per_pixel = &impl::bytes_per_pixel;
inline constexpr auto ws_bytes_per_pixel = &impl::bytes_per_pixel;

// 规则 1 的编译期约束：签名不一致就编译失败，杜绝"看起来等价其实不等价"。
static_assert(std::is_same_v<decltype(wlr_rect_contains_point), decltype(ws_rect_contains_point)>);
static_assert(std::is_same_v<decltype(wlr_rect_intersect), decltype(ws_rect_intersect)>);
static_assert(std::is_same_v<decltype(wlr_scale_size), decltype(ws_scale_size)>);
static_assert(std::is_same_v<decltype(wlr_bytes_per_pixel), decltype(ws_bytes_per_pixel)>);

// 共用一份实现：两个名字必须 literally 指向同一个函数。
static_assert(wlr_rect_contains_point == ws_rect_contains_point);
static_assert(wlr_rect_intersect == ws_rect_intersect);
static_assert(wlr_scale_size == ws_scale_size);
static_assert(wlr_bytes_per_pixel == ws_bytes_per_pixel);

}
