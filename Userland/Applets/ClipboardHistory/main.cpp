/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardHistoryModel.h"
#include <LibConfig/Client.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/Margins.h>
#include <LibGUI/Menu.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath unix cpath wpath"));
    auto app = TRY(GUI::Application::create(arguments));

    // --record: 常驻后台，只监听剪贴板变化并写历史文件，不开窗口。
    // 会话启动时跑一份，Win+V 再打开窗口读同一份文件。
    bool record_only = false;
    Core::ArgsParser parser;
    parser.add_option(record_only, "Record clipboard changes in the background without windows", "record", 'r');
    bool open_window = false;
    parser.add_option(open_window, "Show the history window instead of the tray applet (Win+V)", "window", 'w');
    parser.parse(arguments);
    auto clipboard_config = TRY(Core::ConfigFile::open_for_app("ClipboardHistory"));

    auto const default_path = ByteString::formatted("{}/{}", Core::StandardPaths::data_directory(), "Clipboard/ClipboardHistory.json"sv);
    auto const clipboard_file_path = clipboard_config->read_entry("Clipboard", "ClipboardFilePath", default_path);
    auto const parent_path = LexicalPath(clipboard_file_path);
    TRY(Core::Directory::create(parent_path.dirname(), Core::Directory::CreateDirectories::Yes));

    Config::pledge_domain("ClipboardHistory");
    Config::monitor_domain("ClipboardHistory");

    TRY(Core::System::pledge("stdio recvfd sendfd rpath cpath wpath"));
    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil(parent_path.dirname(), "rwc"sv));

    TRY(Core::System::unveil(nullptr, nullptr));
    auto app_icon = TRY(GUI::Icon::try_create_default_icon("edit-copy"sv));

    auto main_window = GUI::Window::construct();
    main_window->set_title("Clipboard History");
    // 小一点的弹出面板，贴在屏幕右下（Win+V 的用法），不要占半屏。
    main_window->set_rect(800, 460, 460, 460);
    main_window->set_icon(app_icon.bitmap_for_size(16));

    auto main_widget = main_window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

    auto& table_view = main_widget->add<GUI::TableView>();
    auto model = ClipboardHistoryModel::create();

    TRY(model->read_from_file(clipboard_file_path));

    if (record_only) {
        GUI::Clipboard::the().on_change = [model](ByteString const&) {
            auto current = GUI::Clipboard::the().fetch_data_and_type();
            if (current.data.is_empty() && current.mime_type.is_empty())
                return;
            model->add_item(current);
        };
        return app->exec();
    }

    auto data_and_type = GUI::Clipboard::the().fetch_data_and_type();
    if (!(data_and_type.data.is_empty() && data_and_type.mime_type.is_empty() && data_and_type.metadata.is_empty()))
        model->add_item(data_and_type);

    table_view.set_model(model);

    table_view.on_activation = [&](GUI::ModelIndex const& index) {
        auto& data_and_type = model->item_at(index.row()).data_and_type;
        GUI::Clipboard::the().set_data(data_and_type.data, data_and_type.mime_type, data_and_type.metadata);
    };

    // 图片条目预览（仅选中 image/x-serenityos 时可见）
    auto& image_preview = main_widget->add<GUI::ImageWidget>();
    image_preview.set_frame_style(Gfx::FrameStyle::SunkenPanel);
    image_preview.set_auto_resize(false);
    image_preview.set_should_stretch(false);
    image_preview.set_fixed_height(110);
    image_preview.set_visible(false);

    auto delete_selected = [&] {
        if (table_view.selection().is_empty())
            return;

        auto index = table_view.selection().first();
        model->remove_item(index.row());
        if (model->is_empty()) {
            GUI::Clipboard::the().clear();
        } else if (index.row() == 0) {
            auto const& data_and_type = model->item_at(index.row()).data_and_type;
            GUI::Clipboard::the().set_data(data_and_type.data, data_and_type.mime_type, data_and_type.metadata);
        }
    };

    auto clear_all = [&] {
        model->clear();
        GUI::Clipboard::the().clear();
    };

    // 底部按钮栏：删除所选 / 全部清空（右键菜单依然保留）
    auto& button_bar = main_widget->add<GUI::Widget>();
    button_bar.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    // Without this the button bar (default preferred Grow) eats half the extra
    // vertical space and the table gets squeezed. Fit = size to the buttons.
    button_bar.set_preferred_height(GUI::SpecialDimension::Fit);
    auto& delete_button = button_bar.add<GUI::Button>("删除所选"_string);
    delete_button.set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/delete.png"sv)));
    auto& clear_button = button_bar.add<GUI::Button>("全部清空"_string);
    clear_button.set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/trash-can.png"sv)));
    clear_button.set_tooltip("清空剪贴板历史并清空系统剪贴板"_string);

    auto update_panel = [&] {
        bool const has_selection = !table_view.selection().is_empty();
        delete_button.set_enabled(has_selection);
        clear_button.set_enabled(!model->is_empty());

        bool preview_shown = false;
        if (has_selection) {
            auto& item = model->item_at(table_view.selection().first().row());
            if (item.data_and_type.mime_type == "image/x-serenityos") {
                if (auto bitmap = item.data_and_type.as_bitmap()) {
                    constexpr float preview_max_width = 420;
                    constexpr float preview_max_height = 100;
                    float scale = min(1.0f, min(preview_max_width / bitmap->width(), preview_max_height / bitmap->height()));
                    if (scale < 1.0f) {
                        Gfx::IntSize fit { max(1, (int)(bitmap->width() * scale)), max(1, (int)(bitmap->height() * scale)) };
                        if (auto scaled_or_error = bitmap->scaled_to_size(fit); !scaled_or_error.is_error())
                            image_preview.set_bitmap(scaled_or_error.value().ptr());
                        else
                            image_preview.set_bitmap(bitmap.ptr());
                    } else {
                        image_preview.set_bitmap(bitmap.ptr());
                    }
                    image_preview.set_visible(true);
                    preview_shown = true;
                }
            }
        }
        if (!preview_shown) {
            image_preview.set_bitmap(nullptr);
            image_preview.set_visible(false);
        }
    };

    delete_button.on_click = [&](unsigned) {
        delete_selected();
        update_panel();
    };
    clear_button.on_click = [&](unsigned) {
        clear_all();
        update_panel();
    };
    table_view.on_selection_change = [&] { update_panel(); };

    auto delete_action = GUI::CommonActions::make_delete_action([&](GUI::Action const&) {
        delete_selected();
        update_panel();
    });

    auto debug_dump_action = GUI::Action::create("Dump to Debug Console", [&](GUI::Action const&) {
        table_view.selection().for_each_index([&](GUI::ModelIndex& index) {
            dbgln("{}", model->data(index, GUI::ModelRole::Display).as_string());
        });
    });

    auto clear_action = GUI::Action::create("Clear History", TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/trash-can.png"sv)), [&](GUI::Action const&) {
        clear_all();
        update_panel();
    });

    update_panel();

    auto entry_context_menu = GUI::Menu::construct();
    entry_context_menu->add_action(delete_action);
    entry_context_menu->add_action(debug_dump_action);
    entry_context_menu->add_separator();
    entry_context_menu->add_action(clear_action);
    table_view.on_context_menu_request = [&](GUI::ModelIndex const&, GUI::ContextMenuEvent const& event) {
        delete_action->set_enabled(!table_view.selection().is_empty());
        debug_dump_action->set_enabled(!table_view.selection().is_empty());
        clear_action->set_enabled(!model->is_empty());
        entry_context_menu->popup(event.screen_position());
    };

    // Win+V 走这条：直接显示列表，不建托盘 applet（这套移植里 applet 点不到）。
    if (open_window) {
        main_window->show();
        main_window->move_to_front();
        return app->exec();
    }

    auto applet_window = GUI::Window::construct();
    applet_window->set_title("ClipboardHistory");
    applet_window->set_window_type(GUI::WindowType::Applet);
    applet_window->set_has_alpha_channel(true);
    auto icon_widget = applet_window->set_main_widget<GUI::ImageWidget>();
    icon_widget->set_tooltip("Clipboard History"_string);
    icon_widget->load_from_file("/res/icons/16x16/edit-copy.png"sv);
    icon_widget->on_click = [&main_window = *main_window] {
        main_window.show();
        main_window.move_to_front();
    };
    applet_window->resize(16, 16);
    applet_window->show();

    return app->exec();
}
