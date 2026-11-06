/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Windows 程序管理 - a Win9x-style Control Panel applet (omikuji-inspired) for
// installing, launching and uninstalling Windows games/programs. Each installed
// program gets its own isolated WINEPREFIX and a launcher wrapper; the registry
// and artifact helpers live in Ports/LinuxGUI/Common/WinApps.h.

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/Icon.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Model.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>
#include <Ports/LinuxGUI/Common/WinApps.h>

namespace {

ByteString mode_display(ByteString const& mode)
{
    if (mode == "game")
        return "游戏";
    if (mode == "app")
        return "应用";
    return "自动";
}

ByteString mode_token(StringView label)
{
    if (label == "游戏"sv)
        return "game";
    if (label == "应用"sv)
        return "app";
    return "auto";
}

// Asks the user to classify a freshly added program as a game or an
// application (with "auto" as a fallback). The choice is baked into the
// registry entry and the generated wrapper, so it must be made at install
// time rather than guessed.
class ModeChoiceDialog final : public GUI::Dialog {
    C_OBJECT(ModeChoiceDialog)
public:
    ByteString selected_mode() const { return m_mode; }

private:
    explicit ModeChoiceDialog(GUI::Window* parent)
        : GUI::Dialog(parent, ScreenPosition::CenterWithinParent)
    {
        set_title("选择程序类型"sv);
        resize(320, 140);

        auto main = set_main_widget<GUI::Widget>();
        main->set_layout<GUI::VerticalBoxLayout>(8, 12);
        main->set_fill_with_background_color(true);

        auto& prompt = main->add<GUI::Label>("这是一个游戏还是普通应用？"_string);
        prompt.set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& buttons = main->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>(6);
        buttons.add_spacer();
        auto& game_button = buttons.add<GUI::Button>("游戏"_string);
        game_button.set_fixed_width(80);
        auto& app_button = buttons.add<GUI::Button>("应用"_string);
        app_button.set_fixed_width(80);
        auto& auto_button = buttons.add<GUI::Button>("自动"_string);
        auto_button.set_fixed_width(80);
        buttons.add_spacer();

        game_button.on_click = [this](auto) {
            m_mode = "game"sv;
            done(ExecResult::OK);
        };
        app_button.on_click = [this](auto) {
            m_mode = "app"sv;
            done(ExecResult::OK);
        };
        auto_button.on_click = [this](auto) {
            m_mode = "auto"sv;
            done(ExecResult::OK);
        };
    }

    ByteString m_mode { "auto"sv };
};

class WinAppsModel final : public GUI::Model {
public:
    explicit WinAppsModel(Vector<WinApps::Program> programs)
        : m_programs(move(programs))
    {
    }

    virtual int row_count(GUI::ModelIndex const&) const override { return m_programs.size(); }
    virtual int column_count(GUI::ModelIndex const& index) const override { return index.is_valid() ? 0 : 3; }

    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case 0:
            return "名称"_string;
        case 1:
            return "模式"_string;
        case 2:
            return "程序文件"_string;
        default:
            return "?"_string;
        }
    }

    virtual GUI::ModelIndex index(int row, int column, GUI::ModelIndex const&) const override
    {
        if (row < 0 || row >= (int)m_programs.size())
            return {};
        return create_index(row, column, &m_programs[row]);
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (index.row() < 0 || index.row() >= (int)m_programs.size())
            return {};
        auto& p = m_programs[index.row()];
        if (role == GUI::ModelRole::Search)
            return ByteString::formatted("{} {}", p.name, p.exe);
        if (role == GUI::ModelRole::Display) {
            if (index.column() == 0)
                return p.name;
            if (index.column() == 1)
                return mode_display(p.mode);
            if (index.column() == 2)
                return p.exe;
        }
        return {};
    }

private:
    Vector<WinApps::Program> m_programs;
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    (void)arguments;
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Windows 程序管理"sv);
    window->resize(660, 420);
    // Keep the Icon alive: bitmap_for_size() returns a pointer owned by the
    // Icon, so a temporary Icon is destroyed before set_icon() dereferences it
    // (that used to crash with a VERIFICATION FAILED in Window::set_icon).
    auto app_icon = GUI::Icon::default_icon("app-brickgame"sv);
    if (auto* icon = app_icon.bitmap_for_size(16))
        window->set_icon(icon);

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_layout<GUI::VerticalBoxLayout>(4, 6);
    // Paint the root widget background: otherwise areas covered only by
    // transparent widgets (e.g. a Label) show the unpainted (black) window buffer.
    main_widget->set_fill_with_background_color(true);

    // Program list: 名称 / 模式 / 程序文件.
    auto& table = main_widget->add<GUI::TableView>();
    table.set_selection_mode(GUI::AbstractView::SelectionMode::SingleSelection);

    // Detail row: the selected program's WINEPREFIX + launch-mode combo. The
    // exe path already has its own table column, so the label is spent on the
    // per-program prefix instead (the thing users actually need to find, and
    // the one piece of state that is not visible anywhere else).
    auto& detail = main_widget->add<GUI::Widget>();
    detail.set_layout<GUI::HorizontalBoxLayout>(4);
    detail.set_fixed_height(24);
    auto& prefix_label = detail.add<GUI::Label>("尚未选择程序"_string);
    prefix_label.set_fixed_width(430);
    auto& mode_combo = detail.add<GUI::ComboBox>();
    mode_combo.set_only_allow_values_from_model(true);
    mode_combo.set_fixed_width(96);
    // ItemListModel stores a *reference* to the container (Container const&
    // m_data), so the vector must outlive the combo box. Passing a temporary
    // Vector<ByteString>{...} left a dangling reference and SIGSEGV'd as soon
    // as set_text() read the model.
    Vector<ByteString> mode_options { "自动"sv, "应用"sv, "游戏"sv };
    mode_combo.set_model(*GUI::ItemListModel<ByteString>::create(mode_options));
    mode_combo.set_text("自动");

    // Action buttons.
    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(4);
    buttons.set_fixed_height(28);
    auto& install_button = buttons.add<GUI::Button>("安装..."_string);
    auto& launch_button = buttons.add<GUI::Button>("启动"_string);
    auto& folder_button = buttons.add<GUI::Button>("运行环境"_string);
    auto& uninstall_button = buttons.add<GUI::Button>("卸载"_string);
    auto& refresh_button = buttons.add<GUI::Button>("刷新"_string);

    auto& statusbar = main_widget->add<GUI::Statusbar>();

    // ---- State + behavior -------------------------------------------------
    RefPtr<WinAppsModel> model;
    Vector<WinApps::Program> programs;
    bool updating_selection = false;

    auto selected_index = [&]() -> GUI::ModelIndex {
        auto const& selection = table.selection();
        if (selection.is_empty())
            return {};
        auto index = selection.first();
        if (!index.is_valid() || index.row() < 0 || index.row() >= (int)programs.size())
            return {};
        return index;
    };

    auto update_selection = [&] {
        auto index = selected_index();
        bool has_selection = index.is_valid();
        launch_button.set_enabled(has_selection);
        folder_button.set_enabled(has_selection);
        uninstall_button.set_enabled(has_selection);
        mode_combo.set_enabled(has_selection);

        updating_selection = true;
        if (!has_selection) {
            mode_combo.set_text("自动");
            prefix_label.set_text("尚未选择程序"_string);
            if (programs.is_empty())
                statusbar.set_text("尚未安装任何程序 — 点击“安装…”添加一个 .exe"_string);
            else
                statusbar.set_text(String::formatted("共 {} 个程序", programs.size()).release_value_but_fixme_should_propagate_errors());
        } else {
            auto& p = programs[index.row()];
            // The exe already has its own table column, so the status bar is
            // spent on the prefix path (which no column shows and which users
            // need in full to inspect or back up a prefix).
            prefix_label.set_text(String::formatted("运行环境：{}", p.slug).release_value_but_fixme_should_propagate_errors());
            mode_combo.set_text(mode_display(p.mode));
            statusbar.set_text(String::formatted("{}  —  {}", p.name, p.prefix).release_value_but_fixme_should_propagate_errors());
        }
        updating_selection = false;
    };

    // Reload the registry into the table. Selection follows the *program id*,
    // not the row index: the list is sorted by id, but rows shift when an entry
    // is added or removed, so index-based reselection picked the wrong program.
    auto reload = [&](Optional<int> select_id) {
        programs = WinApps::list_programs();
        model = adopt_ref(*new WinAppsModel(programs));
        table.set_model(*model);
        table.set_column_width(0, 170);
        table.set_column_width(1, 64);
        table.set_column_width(2, 380);
        auto row = -1;
        if (select_id.has_value()) {
            for (int i = 0; i < (int)programs.size(); ++i) {
                if (programs[i].id == select_id.value()) {
                    row = i;
                    break;
                }
            }
        }
        if (row >= 0)
            // set_cursor(..., Set) instead of selection().set(): it also makes
            // the row the keyboard cursor and scrolls it into view, which is
            // what "the program I just touched stays selected" means in a list
            // that can be longer than the window.
            table.set_cursor(model->index(row, 0, {}), GUI::AbstractView::SelectionUpdate::Set);
        else
            table.selection().clear();
        update_selection();
    };

    auto launch_selected = [&] {
        auto index = selected_index();
        if (!index.is_valid())
            return;
        WinApps::launch_program(programs[index.row()], window);
    };

    table.on_selection_change = [&] { update_selection(); };
    table.on_activation = [&](GUI::ModelIndex const&) { launch_selected(); };

    mode_combo.on_change = [&](ByteString const& text, GUI::ModelIndex const&) {
        if (updating_selection)
            return;
        auto index = selected_index();
        if (!index.is_valid())
            return;
        auto token = mode_token(text);
        auto p = programs[index.row()];
        if (p.mode == token)
            return;
        p.mode = token;
        programs[index.row()] = p;
        if (auto err = WinApps::set_mode(p.id, token); err.is_error()) {
            GUI::MessageBox::show_error(window, ByteString::formatted("无法保存模式: {}", err.error()));
            return;
        }
        if (auto err = WinApps::write_wrapper(p); err.is_error()) {
            GUI::MessageBox::show_error(window, ByteString::formatted("无法更新启动器: {}", err.error()));
            return;
        }
        reload(p.id);
    };

    install_button.on_click = [&](auto) {
        auto exe = GUI::FilePicker::get_open_filepath(window, "选择 Windows 程序"sv,
            Core::StandardPaths::home_directory(), false,
            GUI::Dialog::ScreenPosition::CenterWithinParent,
            Vector<GUI::FileTypeFilter> { GUI::FileTypeFilter { "Windows 可执行文件 (*.exe)"sv, Vector<ByteString> { "exe"sv } } });
        if (!exe.has_value() || exe.value().is_empty())
            return;

        auto default_name = LexicalPath::basename(exe.value());
        if (default_name.ends_with(".exe"sv))
            default_name = default_name.substring(0, default_name.length() - 4);

        // Already managed? Adding it again would create a second isolated prefix
        // and a second set of shortcuts for one program, so point at the entry
        // that is already there instead.
        if (auto existing = WinApps::find_by_exe(exe.value()); existing.has_value()) {
            reload(existing->id);
            GUI::MessageBox::show(window,
                ByteString::formatted("“{}”已在管理列表中（运行环境 {}）。", existing->name, existing->slug),
                "添加程序"sv, GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK);
            return;
        }

        String name = String::from_byte_string(default_name).release_value_but_fixme_should_propagate_errors();
        if (GUI::InputBox::show(window, name, "给这个程序起个名字："sv, "添加程序"sv) != GUI::Dialog::ExecResult::OK)
            return;
        auto trimmed = name.to_byte_string().trim_whitespace();
        if (trimmed.is_empty())
            return;

        // Ask whether this is a game or an application before writing anything:
        // the mode is baked into the registry entry and the generated wrapper,
        // and guessing "auto" hides the game/app distinction from the user.
        auto mode_dialog = ModeChoiceDialog::construct(window);
        if (mode_dialog->exec() != GUI::Dialog::ExecResult::OK)
            return;
        auto mode = mode_dialog->selected_mode();

        auto program = WinApps::add_program(trimmed, exe.value(), mode);
        if (program.is_error()) {
            GUI::MessageBox::show_error(window, ByteString::formatted("无法添加程序: {}", program.error()));
            return;
        }
        auto p = program.release_value();
        if (auto err = WinApps::write_artifacts(p); err.is_error()) {
            // Do not leave a registry entry whose shortcuts never got written:
            // the program would be listed but unlaunchable from the desktop or
            // the start menu. Roll the entry back and let the user retry.
            (void)WinApps::remove_program(p.id);
            GUI::MessageBox::show_error(window, ByteString::formatted("无法创建启动器: {}", err.error()));
            reload({});
            return;
        }
        reload(p.id);

        auto run_now = GUI::MessageBox::show(window,
            ByteString::formatted("已添加 “{}”。\n桌面快捷方式和开始菜单项已创建。\n\n是否立即运行安装程序？", p.name),
            "添加程序"sv, GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
        if (run_now == GUI::MessageBox::ExecResult::Yes)
            WinApps::launch_program(p, window);
    };

    launch_button.on_click = [&](auto) { launch_selected(); };

    folder_button.on_click = [&](auto) {
        auto index = selected_index();
        if (!index.is_valid())
            return;
        auto p = programs[index.row()];
        // Make sure the (possibly not-yet-bootstrapped) prefix exists before
        // handing it to the file manager.
        if (auto err = Core::Directory::create(p.prefix, Core::Directory::CreateDirectories::Yes); err.is_error()) {
            GUI::MessageBox::show_error(window, ByteString::formatted("无法创建目录: {}", err.error()));
            return;
        }
        if (!Desktop::Launcher::open(URL::create_with_file_scheme(p.prefix)))
            GUI::MessageBox::show_error(window, "无法打开文件管理器"sv);
    };

    uninstall_button.on_click = [&](auto) {
        auto index = selected_index();
        if (!index.is_valid())
            return;
        auto p = programs[index.row()];
        auto confirm = GUI::MessageBox::show(window,
            ByteString::formatted("确定要卸载 “{}” 吗？\n\n将移除桌面快捷方式、开始菜单项以及程序注册信息。", p.name),
            "卸载"sv, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo);
        if (confirm != GUI::MessageBox::ExecResult::Yes)
            return;

        // Ask about the prefix before touching anything, so the second question
        // is not asked over a half-deleted install.
        auto del_prefix = GUI::MessageBox::show(window,
            "是否同时删除该程序的 Wine 运行环境（含已安装的游戏数据）？\n选择“否”仅移除启动器，保留运行环境。"sv,
            "删除运行环境"sv, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo);

        // Collect failures instead of aborting on the first one: a shortcut that
        // cannot be removed (read-only desktop, permissions) is worth reporting
        // but must not leave the registry entry stuck forever.
        StringBuilder problems;
        if (auto err = WinApps::remove_artifacts(p); err.is_error())
            problems.appendff("快捷方式: {}\n", err.error());
        if (del_prefix == GUI::MessageBox::ExecResult::Yes) {
            if (auto err = WinApps::remove_prefix(p); err.is_error())
                problems.appendff("运行环境: {}\n", err.error());
        }
        if (auto err = WinApps::remove_program(p.id); err.is_error()) {
            GUI::MessageBox::show_error(window, ByteString::formatted("无法移除注册信息: {}", err.error()));
            return;
        }
        reload({});
        if (!problems.is_empty())
            GUI::MessageBox::show_error(window, ByteString::formatted("“{}”已从列表中移除，但有残留：\n\n{}", p.name, problems.string_view()));
    };

    refresh_button.on_click = [&](auto) {
        reload({});
        statusbar.set_text(String::formatted("已刷新 — 共 {} 个程序", programs.size()).release_value_but_fixme_should_propagate_errors());
    };

    // One-shot migration of legacy start-menu categories; running it on every
    // reload opened every program's .af for nothing.
    WinApps::migrate_menu_category();
    reload({});
    window->show();
    return app->exec();
}
