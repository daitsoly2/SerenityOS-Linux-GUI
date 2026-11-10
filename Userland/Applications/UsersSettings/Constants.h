/*
 * Copyright (c) 2026, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibGUI/UITranslation.h>

namespace UsersSettings {

static constexpr u32 MIN_NORMAL_UID = 100;
static constexpr Array ACCOUNT_TYPE_NAMES = { "Standard"sv, "Administrator"sv };
// Linux 宿主的管理员组是 sudo，部分发行版用 wheel；取系统里实际存在的那个。
// 真正增删组成员由 serenity-hostd 完成，这里只用来回显账户类型。
static constexpr Array ADMIN_GROUP_NAMES = { "sudo"sv, "wheel"sv };

// ComboBox 的值列表由模型直接绘制，不经过 GUI::translate_ui()（那条路径只覆盖
// set_text 一类 setter），所以这里自己翻译一次。
// 两个要点：
//   * 用函数内 static：翻译表是懒加载的，静态初始化期还读不到；
//   * 返回引用：ItemListModel 存的是 `Container const&`，容器必须比模型活得久，
//     绝不能返回临时 Array（否则下拉框第一次打开就是 UAF）。
inline Array<StringView, 2> const& account_type_names()
{
    static Array<ByteString, 2> const storage {
        GUI::translate_ui(ByteString { ACCOUNT_TYPE_NAMES[0] }),
        GUI::translate_ui(ByteString { ACCOUNT_TYPE_NAMES[1] }),
    };
    static Array<StringView, 2> const names {
        storage[0].view(),
        storage[1].view(),
    };
    return names;
}

}
