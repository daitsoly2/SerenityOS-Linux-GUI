/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "WsWlrBridge/Classification.h"

#include <cstring>

namespace WsWlrBridge {

char const* to_string(BridgeKind kind)
{
    switch (kind) {
    case BridgeKind::EquivalentAlias:
        return "[别名] EquivalentAlias";
    case BridgeKind::AdaptationWrapper:
        return "[适配] AdaptationWrapper";
    case BridgeKind::IndependentImplementation:
        return "[独立] IndependentImplementation";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// 全桥唯一的分类登记表。新增任何入口都必须在这里补一行，写清归类理由；
// selftest 会检查「对外入口齐全 + 三类都非空 + 对外头无别名泄漏」。
// ─────────────────────────────────────────────────────────────────────────────
static BridgeEntry const k_entries[] = {
    // ── [别名] 语义/参数/生命周期/所有权完全一致，共用 impl 单份实现 ──
    { "ws_rect_contains_point / wlr_rect_contains_point", BridgeKind::EquivalentAlias,
        "纯值判定，无对象、无回调；static_assert 锁死签名与函数地址相同" },
    { "ws_rect_intersect / wlr_rect_intersect", BridgeKind::EquivalentAlias,
        "纯值计算，两侧 Rect 同为排他右下边界" },
    { "ws_scale_size / wlr_scale_size", BridgeKind::EquivalentAlias,
        "纯单位换算，不涉及 buffer 所有权" },
    { "ws_bytes_per_pixel / wlr_bytes_per_pixel", BridgeKind::EquivalentAlias,
        "fourcc 查表，无状态" },

    // ── [适配] 语义 / 时序 / 对象归属有差异 ──
    { "Adaptation::translate_buffer_rect", BridgeKind::AdaptationWrapper,
        "① 参数翻译：buffer 像素坐标 → surface 逻辑坐标" },
    { "Adaptation::geometry_without_origin", BridgeKind::AdaptationWrapper,
        "① 参数翻译：wlr surface 无屏幕坐标，位置留哨兵交 Serenity" },
    { "Adaptation::on_new_surface", BridgeKind::AdaptationWrapper,
        "④ 回调时序：wlr new_surface 早于 map/首个 buffer，只记账不建窗" },
    { "Adaptation::on_map", BridgeKind::AdaptationWrapper,
        "④ 回调时序 + ③ 状态同步：map 早于首个 commit，延迟到首帧建窗" },
    { "Adaptation::on_commit", BridgeKind::AdaptationWrapper,
        "④ 回调时序 + ② ID 映射：FIFO 排队，按原序补发" },
    { "Adaptation::on_damage", BridgeKind::AdaptationWrapper,
        "③ 状态同步 + ① 参数翻译：窗口未建时先排队，随后按原序补发" },
    { "Adaptation::on_unmap", BridgeKind::AdaptationWrapper,
        "对象归属差异：wlr unmap 可逆、ws_window_destroy 终态，remap 时换 generation 重建" },
    { "Adaptation::on_destroy", BridgeKind::AdaptationWrapper,
        "生命周期清理：兑现未决 buffer 票据，防止 wlr_buffer 引用泄漏" },
    { "Adaptation::on_title_changed", BridgeKind::AdaptationWrapper,
        "③ 状态同步：title 早于建窗，先存镜像、建窗后补发" },
    { "Adaptation::relay_focus_request", BridgeKind::AdaptationWrapper,
        "② ID 映射：客户端请求只上报，桥不调用 ws_window_set_focus" },
    { "Adaptation::relay_resize_request", BridgeKind::AdaptationWrapper,
        "② ID 映射：请求只上报，桥不调用 ws_window_set_geometry" },
    { "Adaptation::forward_focus_decision", BridgeKind::AdaptationWrapper,
        "② ID 映射：Serenity 的 activated 原样转发，不检查/不翻转" },
    { "Adaptation::forward_configure_decision", BridgeKind::AdaptationWrapper,
        "① 参数翻译：Serenity Rect 的 x/y 在 xdg_toplevel 无对应字段" },
    { "Adaptation::forward_close_decision", BridgeKind::AdaptationWrapper,
        "② ID 映射：窗口 id → wlr surface 指针" },
    { "Adaptation::on_commit_done", BridgeKind::AdaptationWrapper,
        "对象所有权：WS 消费完成才解锁 wlr_buffer（引用计数 ↔ 借用视图）" },
    { "Adaptation::apply_commit", BridgeKind::AdaptationWrapper,
        "生命周期 + 状态同步：lock / commit / 登记待释放票据" },
    { "Adaptation::apply_damage", BridgeKind::AdaptationWrapper,
        "① 参数翻译：逐矩形缩放，不合并、不裁剪、不丢弃" },
    { "Adaptation::flush_pending", BridgeKind::AdaptationWrapper,
        "④ 回调时序：严格 FIFO 两遍补发，保持事件原序" },
    { "Adaptation::create_window_for", BridgeKind::AdaptationWrapper,
        "② ID 映射 + ③ 状态同步：建窗并绑定双身份" },

    // ── 对外门面：属于适配层的对外入口，职责只有转发（规则 3）──
    { "Bridge::on_wlr_new_surface", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_map", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_commit", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_damage", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_unmap", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_destroy", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_title_changed", BridgeKind::AdaptationWrapper, "门面：纯转发，零判断" },
    { "Bridge::on_wlr_focus_request", BridgeKind::AdaptationWrapper, "门面：请求上报，非决定" },
    { "Bridge::on_wlr_resize_request", BridgeKind::AdaptationWrapper, "门面：请求上报，非决定" },
    { "Bridge::on_ws_focus_decided", BridgeKind::AdaptationWrapper, "门面：转发 Serenity 的决定" },
    { "Bridge::on_ws_configure_decided", BridgeKind::AdaptationWrapper, "门面：转发 Serenity 的决定" },
    { "Bridge::on_ws_close_decided", BridgeKind::AdaptationWrapper, "门面：转发 Serenity 的决定" },
    { "Bridge::on_ws_commit_done", BridgeKind::AdaptationWrapper, "门面：生命周期回调转发" },

    // ── [独立] 两侧都没有对应接口，桥自持的记账状态 ──
    { "HandleMap", BridgeKind::IndependentImplementation,
        "wlr 对象指针 ↔ ws 窗口 id 双向映射；只记账，不拥有对象" },
    { "SerialMap", BridgeKind::IndependentImplementation,
        "未决请求 serial ↔ 窗口 id 配对；不据此推断焦点" },
    { "PendingQueue", BridgeKind::IndependentImplementation,
        "回调时序 FIFO；刻意不合并/不去重（那属于策略）" },
    { "StateMirror", BridgeKind::IndependentImplementation,
        "生命周期镜像 + wlr_buffer 待释放票据，吸收两侧所有权差异" },
    { "BridgeState / Bridge::Impl", BridgeKind::IndependentImplementation,
        "桥自持状态聚合，pImpl 隔离，不对外暴露" },
};

BridgeEntry const* classification_table() { return k_entries; }

size_t classification_table_size() { return sizeof(k_entries) / sizeof(k_entries[0]); }

bool bridge_kind_of(char const* name, BridgeKind& out)
{
    for (auto const& entry : k_entries) {
        if (std::strcmp(entry.name, name) == 0) {
            out = entry.kind;
            return true;
        }
    }
    return false;
}

}
