/*
 * Copyright (c) 2026, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "UserDetailsWidget.h"
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/ListView.h>
#include <LibGUI/SettingsWindow.h>

namespace UsersSettings {

class UsersTab final : public GUI::SettingsWindow::Tab {
    C_OBJECT(UsersTab)

public:
    static ErrorOr<NonnullRefPtr<UsersTab>> try_create();

    // GML 编译器生成的 try_create() 只搭控件树，不会调用这个手写初始化。
    // 必须由调用方显式执行，否则用户列表、按钮回调全都不会接上。
    ErrorOr<void> initialize();

    virtual void apply_settings() override;

private:
    UsersTab() = default;

    // GML 生成链路上初始化可能被引发多次；没有这道保护会重复挂占位 label、
    // 重复绑定按钮回调（点一次 Add User 弹两个对话框）。
    bool m_initialized { false };

    ErrorOr<void> refresh_users();
    ErrorOr<void> add_user();
    ErrorOr<void> delete_user(StringView username);

    RefPtr<GUI::ListView> m_users_list;
    Vector<String> m_usernames;
    RefPtr<GUI::Button> m_add_button;
    RefPtr<GUI::Button> m_delete_button;
    RefPtr<GUI::Widget> m_details_container;
    RefPtr<GUI::Label> m_no_selection_label;
    RefPtr<UserDetailsWidget> m_user_details_widget;
};

}
