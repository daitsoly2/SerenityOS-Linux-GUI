/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 桥接层五条规则（本目录所有代码必须遵守）
//
//  1. 功能语义、参数、生命周期、对象所有权**完全一致**的接口 → 等价别名 /
//     inline 薄包装，共用一份实现；用 static_assert 锁死签名一致。
//  2. 语义、触发时机、对象归属、回调时序**差异大**的 → 独立适配 wrapper，
//     只做参数翻译 / ID 映射 / 状态同步 / 回调时序对齐。
//  3. 桥只翻译、映射、转发；**不做窗口策略、焦点、布局决策**。所有决策由
//     Serenity WindowServer 产生，桥只把决定原样转发给 wlr 侧。
//  4. 别名只在桥内部（src/detail，PRIVATE include path）使用，不向外暴露；
//     对外只见 Bridge.h 的 on_wlr_* / on_ws_* 入口。
//  5. 现阶段只修改桥接层，不动 WindowServer 核心代码。
//
// 三类标记（每个函数/类型都要带，便于 review 与自检）：
//   [别名]  EquivalentAlias           —— 等价别名，共用实现
//   [适配]  AdaptationWrapper         —— 适配转换 wrapper
//   [独立]  IndependentImplementation —— 两侧都无对应物，桥自持的记账状态
//
// 分类不是文档装饰：classification_table() 是唯一登记处，自检会断言
// 每个对外入口都已登记、三类都非空、且对外头文件里没有任何别名泄漏。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>

namespace WsWlrBridge {

enum class BridgeKind {
    EquivalentAlias,          // [别名]
    AdaptationWrapper,        // [适配]
    IndependentImplementation // [独立]
};

char const* to_string(BridgeKind kind);

struct BridgeEntry {
    char const* name;
    BridgeKind kind;
    char const* rationale; // 为什么归到这一类；差异点写清楚
};

BridgeEntry const* classification_table();
size_t classification_table_size();

// 查不到返回 false。测试用它验证「所有对外入口都已分类」。
bool bridge_kind_of(char const* name, BridgeKind& out);

}
