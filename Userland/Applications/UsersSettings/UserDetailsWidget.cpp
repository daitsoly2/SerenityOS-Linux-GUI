/*
 * Copyright (c) 2026, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UserDetailsWidget.h"
#include "ChangePasswordDialog.h"
#include "Constants.h"
#include "HostdClient.h"
#include <AK/String.h>
#include <LibCore/Account.h>
#include <LibCore/System.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>

namespace UsersSettings {

ErrorOr<NonnullRefPtr<UserDetailsWidget>> UserDetailsWidget::create(Core::Account const& account)
{
    auto widget = TRY(try_create());
    TRY(widget->initialize(account));
    return widget;
}

// GECOS field is "Full Name,Room,Work,Home,Other" - extract just the full name part.
static ByteString gecos_full_name(ByteString const& gecos)
{
    auto comma = gecos.find(',');
    if (comma.has_value())
        return gecos.substring(0, comma.value());
    return gecos;
}

ErrorOr<void> UserDetailsWidget::initialize(Core::Account const& account)
{
    m_account = account;

    m_username_textbox = find_descendant_of_type_named<GUI::TextBox>("username_textbox");
    m_full_name_textbox = find_descendant_of_type_named<GUI::TextBox>("full_name_textbox");
    m_shell_textbox = find_descendant_of_type_named<GUI::TextBox>("shell_textbox");
    m_home_dir_textbox = find_descendant_of_type_named<GUI::TextBox>("home_dir_textbox");
    m_uid_gid_textbox = find_descendant_of_type_named<GUI::TextBox>("uid_gid_textbox");
    m_account_type_combobox = find_descendant_of_type_named<GUI::ComboBox>("account_type_combobox");
    m_change_password_button = find_descendant_of_type_named<GUI::Button>("change_password_button");

    m_username_textbox->set_text(account.username());

    m_full_name_textbox->set_text(gecos_full_name(account.gecos()));
    m_full_name_textbox->on_change = [this] {
        if (on_modified)
            on_modified();
    };

    m_shell_textbox->set_text(account.shell());
    m_shell_textbox->on_change = [this] {
        if (on_modified)
            on_modified();
    };

    m_home_dir_textbox->set_text(account.home_directory());
    m_uid_gid_textbox->set_text(ByteString::formatted("{} / {}", account.uid(), account.gid()));

    // Linux 上管理员组是 sudo（或 wheel），取宿主机实际存在的那个来回显。
    bool is_admin = false;
    for (auto group_name : ADMIN_GROUP_NAMES) {
        auto maybe_group = Core::System::getgrnam(group_name);
        if (maybe_group.is_error() || !maybe_group.value().has_value())
            continue;
        is_admin = account.extra_gids().contains_slow(maybe_group.value()->gr_gid);
        break;
    }
    m_account_type_combobox->set_model(*GUI::ItemListModel<StringView, Array<StringView, 2>>::create(account_type_names()));
    m_account_type_combobox->set_only_allow_values_from_model(true);
    m_account_type_combobox->set_selected_index(is_admin ? 1 : 0);
    m_account_type_combobox->on_change = [this](auto&, auto) {
        if (on_modified)
            on_modified();
    };

    m_change_password_button->on_click = [this](auto) {
        if (auto result = change_password(); result.is_error())
            GUI::MessageBox::show_error(window(), "Failed to change password"sv);
    };

    return {};
}

ErrorOr<void> UserDetailsWidget::change_password()
{
    VERIFY(m_account.has_value());

    auto dialog = TRY(GUI::Dialog::try_create(window()));
    dialog->set_title(ByteString::formatted("Change Password for {}", m_account->username()));
    dialog->resize(280, 85);
    dialog->set_resizable(false);

    auto widget = TRY(ChangePasswordDialog::try_create());
    dialog->set_main_widget(widget);

    auto& new_password_textbox = *widget->find_descendant_of_type_named<GUI::PasswordBox>("new_password_textbox");
    auto& confirm_password_textbox = *widget->find_descendant_of_type_named<GUI::PasswordBox>("confirm_password_textbox");

    auto& ok_button = *widget->find_descendant_of_type_named<GUI::Button>("ok_button");
    ok_button.on_click = [&](auto) {
        if (new_password_textbox.text() != confirm_password_textbox.text()) {
            GUI::MessageBox::show(window(), "Passwords do not match."sv, "Error"sv, GUI::MessageBox::Type::Error);
            return;
        }
        // 密码走 root 代理的 chpasswd：不进 argv，也不碰 /etc/shadow 格式。
        auto password = new_password_textbox.text();
        auto result = hostd_set_password(m_account->username(), password.view());
        if (result.is_error()) {
            auto message = ByteString::formatted("Failed to set password: {}", result.error());
            GUI::MessageBox::show(window(), message.view(), "Error"sv, GUI::MessageBox::Type::Error);
            return;
        }

        dialog->done(GUI::Dialog::ExecResult::OK);
    };
    ok_button.set_default(true);

    auto& cancel_button = *widget->find_descendant_of_type_named<GUI::Button>("cancel_button");
    cancel_button.on_click = [dialog](auto) {
        dialog->done(GUI::Dialog::ExecResult::Cancel);
    };

    dialog->exec();
    return {};
}

ErrorOr<void> UserDetailsWidget::apply_changes()
{
    VERIFY(m_account.has_value());

    // 保留 GECOS 里逗号之后的 Room / Work / Home / Other 字段。
    auto existing_gecos = m_account->gecos();
    auto full_name = m_full_name_textbox->text();
    auto shell = m_shell_textbox->text();
    if (auto first_comma = existing_gecos.find(','); first_comma.has_value())
        full_name = ByteString::formatted("{}{}", full_name, existing_gecos.substring_view(first_comma.value()));

    // 全名、登录 Shell、管理员资格统一交给 root 代理落盘。绝不能再调
    // Core::Account::sync()：它会按 Serenity 格式重刷 /etc/passwd、
    // /etc/group、/etc/shadow，足以破坏 Linux 系统登录。
    TRY(hostd_modify_user(m_account->username(),
        full_name.view(),
        shell.view(),
        m_account_type_combobox->selected_index() == 1));

    return {};
}

}
