/*
 * Copyright (c) 2026, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HostdClient.h"
#include "UsersTab.h"
#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/Icon.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/SettingsWindow.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto app_icon = TRY(GUI::Icon::try_create_default_icon("app-user-settings"sv));

    auto window = TRY(GUI::SettingsWindow::create("Users Settings"));
    window->set_icon(app_icon.bitmap_for_size(16));

    // 不再要求本进程是 root：所有会改动 Linux 账户的操作都交给 root 守护
    // serenity-hostd，GUI 只负责展示。启动前先确认代理可达，免得用户改完
    // 一堆设置才在点「应用」时失败。
    if (auto result = UsersSettings::hostd_ping(); result.is_error()) {
        auto message = ByteString::formatted("用户管理服务不可用：{}", result.error());
        GUI::MessageBox::show_error(window, message.view());
        return Error::from_string_literal("serenity-hostd 不可达");
    }

    auto users_tab = TRY(UsersSettings::UsersTab::try_create());
    // GML 生成的 try_create() 只构建控件树；显式的 initialize() 才会拉用户列表、
    // 绑定增删改按钮。漏掉这一步界面能打开但完全不工作。
    TRY(users_tab->initialize());
    TRY(window->add_tab(users_tab, "Users"_string, "users"sv));

    window->show();
    return app->exec();
}
