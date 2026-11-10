/*
 * Copyright (c) 2026, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UsersTab.h"
#include "Constants.h"
#include "HostdClient.h"
#include "UserAddDialog.h"
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibCore/Account.h>
#include <LibGUI/AbstractView.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>

namespace UsersSettings {

ErrorOr<void> UsersTab::initialize()
{
    if (m_initialized)
        return {};
    m_initialized = true;

    m_users_list = *find_descendant_of_type_named<GUI::ListView>("users_list");
    m_add_button = *find_descendant_of_type_named<GUI::Button>("add_button");
    m_delete_button = *find_descendant_of_type_named<GUI::Button>("delete_button");
    m_details_container = *find_descendant_of_type_named<GUI::Widget>("details_container");

    m_no_selection_label = TRY(GUI::Label::try_create());
    m_no_selection_label->set_text("Select a user to view details"_string);
    m_no_selection_label->set_text_alignment(Gfx::TextAlignment::Center);
    m_details_container->add_child(*m_no_selection_label);

    TRY(refresh_users());

    m_users_list->on_selection_change = [this]() {
        m_details_container->remove_all_children();
        m_user_details_widget = nullptr;
        ArmedScopeGuard on_error_path = [this]() {
            m_details_container->add_child(*m_no_selection_label);
        };

        auto index = m_users_list->selection().first();
        if (!index.is_valid())
            return;

        auto account_or_error = Core::Account::from_name(m_usernames[index.row()].bytes_as_string_view());
        if (account_or_error.is_error())
            return;

        auto widget = UserDetailsWidget::create(account_or_error.value());
        if (widget.is_error()) {
            GUI::MessageBox::show_error(window(), "Failed to load user details"sv);
            return;
        }

        on_error_path.disarm();
        m_user_details_widget = widget.release_value();
        m_user_details_widget->on_modified = [this] { set_modified(true); };
        m_details_container->add_child(*m_user_details_widget);
    };

    m_add_button->on_click = [this](auto) {
        if (auto result = add_user(); result.is_error())
            GUI::MessageBox::show_error(window(), "Failed to add user"sv);
    };

    m_delete_button->on_click = [this](auto) {
        auto index = m_users_list->selection().first();
        if (!index.is_valid())
            return;
        if (auto result = delete_user(m_usernames[index.row()].bytes_as_string_view()); result.is_error())
            GUI::MessageBox::show_error(window(), "Failed to delete user"sv);
    };

    return {};
}

ErrorOr<void> UsersTab::refresh_users()
{
    // 列表走代理：只读 /etc/passwd 的工作放到 root 侧，GUI 不需要 shadow 权限，
    // 也和「普通用户只能看、不能写」的边界保持一致。
    auto rows = TRY(hostd_list_users());
    dbgln("UsersSettings: 从 hostd 载入 {} 个账户", rows.size());
    m_usernames.clear();
    for (auto& row : rows) {
        auto fields = row.view().split_view('\t');
        if (!fields.is_empty())
            m_usernames.append(TRY(String::from_utf8(fields[0])));
    }
    if (m_users_list)
        m_users_list->set_model(*GUI::ItemListModel<String>::create(m_usernames));
    return {};
}

ErrorOr<void> UsersTab::add_user()
{
    auto username = TRY(UserAddDialog::show(window()));
    if (!username.has_value())
        return {};

    TRY(refresh_users());

    // Select the newly created user.
    auto it = m_usernames.find_if([&username](auto const& name) { return name == username.value(); });
    if (!it.is_end()) {
        auto index = m_users_list->model()->index((int)it.index());
        m_users_list->set_cursor(index, GUI::AbstractView::SelectionUpdate::Set);
    }
    return {};
}

ErrorOr<void> UsersTab::delete_user(StringView username)
{
    auto result = GUI::MessageBox::show(window(),
        TRY(String::formatted("Are you sure you want to delete user \"{}\"?", username)),
        "Confirm Deletion"sv,
        GUI::MessageBox::Type::Warning,
        GUI::MessageBox::InputType::YesNo);
    if (result != GUI::MessageBox::ExecResult::Yes)
        return {};

    // 删除交给 root 代理；它会挡掉系统账户和当前桌面会话用户。
    TRY(hostd_delete_user(username));

    m_details_container->remove_all_children();
    m_user_details_widget = nullptr;
    m_details_container->add_child(*m_no_selection_label);
    TRY(refresh_users());
    return {};
}

void UsersTab::apply_settings()
{
    if (!m_user_details_widget)
        return;

    if (auto result = m_user_details_widget->apply_changes(); result.is_error())
        GUI::MessageBox::show_error(window(), "Failed to apply settings"sv);
}

}
