/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// "File Associations" control-panel applet for the Linux GUI port.
//
// The port has no /etc/LaunchServer.ini that survives hostify_executable()
// (its values are Serenity paths like /bin/ImageViewer, which fail access()),
// so all defaults come from /res/apps/*.af. User overrides are stored in the
// per-user ~/.config/LaunchServer.ini as ABSOLUTE host paths, which LaunchServer
// can access() and spawn directly. After a change we ask LaunchServer over IPC
// to reload its handlers/config in place, so the new default applies without
// logging out.

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibDesktop/AppFile.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Icon.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Model.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibGfx/Orientation.h>
#include <LibMain/Main.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <unistd.h>

namespace {

String ui_string(StringView text)
{
    return MUST(String::from_byte_string(ByteString { text }));
}

String ui_string(ByteString const& text)
{
    return MUST(String::from_byte_string(text));
}

// Same blue gradient band as the WinXP control panel, so the applet looks like
// it belongs to the same family.
class BlueHeader final : public GUI::Widget {
    C_OBJECT(BlueHeader)
public:
    virtual ~BlueHeader() override = default;

private:
    BlueHeader() = default;

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.fill_rect_with_gradient(Gfx::Orientation::Horizontal, rect(),
            Gfx::Color::from_rgb(0x14357f), Gfx::Color::from_rgb(0x6b9ce8));
        painter.fill_rect({ 0, height() - 2, width(), 2 }, Gfx::Color::from_rgb(0x0a2358));
    }
};

struct AppInfo {
    ByteString name;
    ByteString executable;
    Vector<ByteString> file_types;
};

struct Row {
    ByteString extension;
    ByteString handler_name;
    ByteString handler_executable;
    ByteString source;
};

class AssociationModel final : public GUI::Model {
public:
    enum Column {
        Extension,
        Handler,
        Source,
        __Count
    };

    static NonnullRefPtr<AssociationModel> create(Vector<Row> rows)
    {
        return adopt_ref(*new AssociationModel(move(rows)));
    }

    virtual ~AssociationModel() override = default;

    Vector<Row>& rows() { return m_rows; }
    Vector<Row> const& rows() const { return m_rows; }

    virtual int row_count(GUI::ModelIndex const& = GUI::ModelIndex()) const override { return m_rows.size(); }
    virtual int column_count(GUI::ModelIndex const& = GUI::ModelIndex()) const override { return Column::__Count; }

    virtual GUI::ModelIndex index(int row, int column, GUI::ModelIndex const& = GUI::ModelIndex()) const override
    {
        if (row < 0 || row >= static_cast<int>(m_rows.size()) || column < 0 || column >= Column::__Count)
            return {};
        return create_index(row, column);
    }

    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case Column::Extension:
            return "扩展名"_string;
        case Column::Handler:
            return "默认打开方式"_string;
        case Column::Source:
            return "来源"_string;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (!index.is_valid() || role != GUI::ModelRole::Display)
            return {};
        auto const& row = m_rows[index.row()];
        switch (index.column()) {
        case Column::Extension:
            return ByteString::formatted(".{}", row.extension);
        case Column::Handler:
            return row.handler_name;
        case Column::Source:
            return row.source;
        default:
            return {};
        }
    }

private:
    explicit AssociationModel(Vector<Row> rows)
        : m_rows(move(rows))
    {
    }

    Vector<Row> m_rows;
};

// Extensions worth showing even when no .af claims them, so the user can
// assign a program to a format the desktop does not know about yet.
static constexpr StringView common_extensions[] = {
    "mp4"sv, "m4v"sv, "mkv"sv, "webm"sv, "avi"sv, "mov"sv, "qt"sv, "wmv"sv,
    "flv"sv, "f4v"sv, "mpg"sv, "mpeg"sv, "mpe"sv, "m1v"sv, "m2v"sv, "mpv"sv,
    "3gp"sv, "3g2"sv, "ts"sv, "mts"sv, "m2ts"sv, "m2t"sv, "vob"sv, "ogv"sv,
    "ogm"sv, "asf"sv, "rm"sv, "rmvb"sv, "divx"sv, "dv"sv, "mxf"sv,
    "mp3"sv, "flac"sv, "wav"sv, "ogg"sv, "oga"sv, "opus"sv, "m4a"sv, "aac"sv,
    "wma"sv, "aiff"sv, "aif"sv, "ape"sv, "alac"sv, "mid"sv, "midi"sv,
    "m3u"sv, "m3u8"sv, "qoa"sv,
    "png"sv, "jpg"sv, "jpeg"sv, "gif"sv, "bmp"sv, "webp"sv, "tiff"sv, "tif"sv,
    "ico"sv, "svg"sv, "avif"sv, "heic"sv, "tvg"sv, "qoi"sv, "dds"sv,
    "ppm"sv, "pgm"sv, "pbm"sv, "jxl"sv,
    "txt"sv, "md"sv, "markdown"sv, "csv"sv, "tsv"sv, "xml"sv, "yaml"sv, "yml"sv,
    "toml"sv, "ini"sv, "cfg"sv, "conf"sv, "log"sv, "json"sv, "html"sv, "htm"sv,
    "js"sv, "ts"sv, "css"sv, "scss"sv, "sh"sv, "py"sv, "c"sv, "cpp"sv, "h"sv,
    "hpp"sv, "rs"sv, "go"sv, "java"sv, "sql"sv, "diff"sv, "patch"sv, "pdf"sv,
    "zip"sv, "tar"sv, "gz"sv, "bz2"sv, "xz"sv, "7z"sv, "rar"sv, "zst"sv,
    "tgz"sv, "tbz"sv, "txz"sv, "db"sv, "font"sv, "gml"sv, "profile"sv, "sheets"sv,
};

ByteString name_for_executable(Vector<AppInfo> const& apps, ByteString const& executable)
{
    for (auto const& app : apps) {
        if (app.executable == executable)
            return app.name;
    }
    return LexicalPath::basename(executable);
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("文件关联 - 控制面板");
    window->resize(760, 540);
    window->set_resizable(true);
    window->set_obey_widget_min_size(false);
    window->set_minimum_size(560, 380);

    auto app_icon = GUI::Icon::try_create_default_icon("app-file-manager"sv);
    if (!app_icon.is_error())
        window->set_icon(app_icon.value().bitmap_for_size(16));

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_background_role(Gfx::ColorRole::Base);
    main_widget->set_layout<GUI::VerticalBoxLayout>(0, 0);

    auto& header = main_widget->add<BlueHeader>();
    header.set_fixed_height(52);
    header.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 12, 10, 12, 10 }, 8);
    auto& title_label = header.add<GUI::Label>(ui_string("文件关联"sv));
    title_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    title_label.set_foreground_role(Gfx::ColorRole::White);
    auto& subtitle_label = header.add<GUI::Label>(ui_string("选择用于打开各类文件的默认程序"sv));
    subtitle_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    subtitle_label.set_foreground_role(Gfx::ColorRole::White);

    auto& hint = main_widget->add<GUI::Label>(ui_string("双击选中某个扩展名，然后从下方选择程序并点击「设为默认」。更改会立即生效。"sv));
    hint.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    hint.set_fixed_height(28);

    // --- Collect installed .af apps and the extensions they declare ---
    Vector<AppInfo> apps;
    HashTable<ByteString> known_extensions;
    Desktop::AppFile::for_each([&](NonnullRefPtr<Desktop::AppFile> app_file) {
        auto executable = PortHostify::hostify_executable(app_file->executable());
        if (access(executable.characters(), X_OK) != 0)
            return;
        AppInfo info { app_file->name(), move(executable), {} };
        for (auto& type : app_file->launcher_file_types())
            info.file_types.append(type.to_lowercase());
        for (auto const& type : info.file_types)
            known_extensions.set(type);
        apps.append(move(info));
    });
    quick_sort(apps, [](auto& a, auto& b) { return a.name < b.name; });

    for (auto extension : common_extensions)
        known_extensions.set(ByteString { extension }.to_lowercase());

    // open_for_app(..., AllowWriting::Yes) creates ~/.config/LaunchServer.ini
    // when it does not exist (File::OpenMode::ReadWrite carries O_CREAT).
    auto config_or_error = Core::ConfigFile::open_for_app("LaunchServer", Core::ConfigFile::AllowWriting::Yes);
    if (config_or_error.is_error()) {
        GUI::MessageBox::show_error(window.ptr(), "无法打开用户配置文件 ~/.config/LaunchServer.ini"sv);
        return 1;
    }
    auto config = config_or_error.release_value();

    auto build_rows = [&]() {
        Vector<ByteString> sorted_extensions;
        for (auto const& extension : known_extensions)
            sorted_extensions.append(extension);
        quick_sort(sorted_extensions, [](auto& a, auto& b) { return a < b; });

        Vector<Row> rows;
        for (auto const& extension : sorted_extensions) {
            Row row { extension, {}, {}, {} };
            auto override_exe = config->read_entry("FileType", extension, "");
            if (!override_exe.is_empty() && access(override_exe.characters(), X_OK) == 0) {
                row.handler_executable = override_exe;
                row.handler_name = name_for_executable(apps, override_exe);
                row.source = ByteString { "用户设置"sv };
            } else {
                for (auto const& candidate : apps) {
                    if (candidate.file_types.contains_slow(extension)) {
                        row.handler_executable = candidate.executable;
                        row.handler_name = candidate.name;
                        row.source = ByteString { ".af 默认"sv };
                        break;
                    }
                }
                if (row.handler_name.is_empty()) {
                    row.handler_name = ByteString { "（未设置）"sv };
                    row.source = ByteString { "—"sv };
                }
            }
            rows.append(move(row));
        }
        return rows;
    };

    auto model = AssociationModel::create(build_rows());

    auto& table = main_widget->add<GUI::TableView>();
    table.set_preferred_size({ GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow });
    table.set_model(model);

    auto& bottom = main_widget->add<GUI::Widget>();
    bottom.set_fixed_height(52);
    bottom.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 8, 6, 8, 6 }, 6);

    auto& combo_label = bottom.add<GUI::Label>(ui_string("默认程序："sv));
    combo_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    combo_label.set_fixed_width(80);

    Vector<ByteString> app_names;
    for (auto const& candidate : apps)
        app_names.append(candidate.name);
    auto& combo = bottom.add<GUI::ComboBox>();
    combo.set_preferred_size({ GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow });
    combo.set_model(*GUI::ItemListModel<ByteString>::create(app_names));
    combo.set_only_allow_values_from_model(true);
    combo.set_model_column(0);
    if (!app_names.is_empty())
        combo.set_selected_index(0, GUI::AllowCallback::No);

    auto& set_button = bottom.add<GUI::Button>(ui_string("设为默认"sv));
    set_button.set_fixed_width(96);
    auto& reset_button = bottom.add<GUI::Button>(ui_string("恢复默认"sv));
    reset_button.set_fixed_width(96);

    auto& statusbar = main_widget->add<GUI::Statusbar>();

    auto refresh_model = [&]() {
        model->rows() = build_rows();
        model->invalidate();
        table.selection().clear();
    };

    auto report_reload_failure = [&]() {
        auto message = ByteString { "设置已保存，但无法通知 LaunchServer 立即重载；重新登录后生效。"sv };
        statusbar.set_text(ui_string(message));
        GUI::MessageBox::show(window.ptr(), message.view(), "文件关联"sv);
    };

    table.on_selection_change = [&]() {
        auto index = table.selection().first();
        if (!index.is_valid())
            return;
        auto const& row = model->rows()[index.row()];
        for (size_t i = 0; i < apps.size(); ++i) {
            if (apps[i].executable == row.handler_executable) {
                combo.set_selected_index(i, GUI::AllowCallback::No);
                break;
            }
        }
        statusbar.set_text(ui_string(ByteString::formatted(".{} → {}（{}）", row.extension, row.handler_name, row.source)));
    };

    set_button.on_click = [&](unsigned) {
        auto index = table.selection().first();
        if (!index.is_valid()) {
            GUI::MessageBox::show(window.ptr(), "请先在列表中选择一个扩展名。"sv, "文件关联"sv);
            return;
        }
        if (combo.selected_index() >= apps.size())
            return;
        auto const& extension = model->rows()[index.row()].extension;
        auto const& chosen = apps[combo.selected_index()];
        config->write_entry("FileType", extension, chosen.executable);
        if (auto result = config->sync(); result.is_error()) {
            GUI::MessageBox::show_error(window.ptr(), "写入配置文件失败"sv);
            return;
        }
        if (auto result = Desktop::Launcher::reload_config(); result.is_error()) {
            report_reload_failure();
            refresh_model();
            return;
        }
        refresh_model();
        statusbar.set_text(ui_string(ByteString::formatted("已将 .{} 的默认程序设为 {}，并已立即生效", extension, chosen.name)));
    };

    reset_button.on_click = [&](unsigned) {
        auto index = table.selection().first();
        if (!index.is_valid()) {
            GUI::MessageBox::show(window.ptr(), "请先在列表中选择一个扩展名。"sv, "文件关联"sv);
            return;
        }
        auto const& extension = model->rows()[index.row()].extension;
        config->remove_entry("FileType", extension);
        if (auto result = config->sync(); result.is_error()) {
            GUI::MessageBox::show_error(window.ptr(), "写入配置文件失败"sv);
            return;
        }
        if (auto result = Desktop::Launcher::reload_config(); result.is_error()) {
            report_reload_failure();
            refresh_model();
            return;
        }
        refresh_model();
        statusbar.set_text(ui_string(ByteString::formatted("已恢复 .{} 的默认关联", extension)));
    };

    statusbar.set_text(ui_string(ByteString::formatted("共 {} 个扩展名，{} 个可用程序", model->row_count(), apps.size())));

    window->show();
    return app->exec();
}
