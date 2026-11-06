/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Windows XP "Luna"-style Control Panel for the Linux GUI port.
//
// Upstream SerenityOS ships a flat Win9x icon grid. This rewrite keeps every
// applet reachable but presents it the way XP did: a blue gradient task pane on
// the left, category rows with underlined task links on the right, plus a
// "classic view" that falls back to a plain icon grid.
//
// Two rules matter for the Linux port and were the reason the old panel felt
// broken:
//   * Only applets whose host binary is actually installed are listed. The
//     /res/apps/*.af catalog contains entries for apps this build skips
//     (BrowserSettings, ...), and launching those only produced an error box.
//   * Launching goes through PortHostify so "/bin/DisplaySettings" resolves to
//     "serenity-display-settings" on the host, exactly like the start menu.

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Try.h>
#include <LibCore/System.h>
#include <LibDesktop/AppFile.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Icon.h>
#include <LibGUI/IconView.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/Label.h>
#include <LibGUI/LinkLabel.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Model.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/ScrollableContainerWidget.h>
#include <LibGUI/StackWidget.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/UIDimensions.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Orientation.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <stdio.h>
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

GUI::Icon icon_or_fallback(StringView name)
{
    if (auto icon = GUI::Icon::try_create_default_icon(name); !icon.is_error())
        return icon.release_value();
    // default_icon() VERIFYs on a missing icon; try_create_default_icon() is
    // the safe form. "app-settings" always ships with the port.
    return MUST(GUI::Icon::try_create_default_icon("app-settings"sv));
}

// The Linux port ships only a handful of hand-named settings icons, so
// AppFile::icon()'s generic "app-<kebab-case>" guesses miss most of them and
// return an empty icon. Map the rest explicitly: an XP control panel full of
// blank tiles looks broken, and calling AppFile::icon() also floods the log
// with "Default icon not found" lines.
static StringView icon_name_for_key(StringView key)
{
    if (key == "DisplaySettings"sv) return "app-display-settings"sv;
    if (key == "TerminalSettings"sv) return "app-terminal"sv;
    if (key == "GamesSettings"sv) return "games"sv;
    if (key == "NetworkSettings"sv) return "network"sv;
    if (key == "MailSettings"sv) return "app-mail"sv;
    if (key == "CertificateSettings"sv) return "certificate"sv;
    if (key == "MapsSettings"sv) return "app-maps"sv;
    if (key == "UsersSettings"sv) return "app-user-settings"sv;
    if (key == "ClockSettings"sv) return "app-analog-clock"sv;
    if (key == "CalendarSettings"sv) return "app-calendar"sv;
    if (key == "KeyboardSettings"sv) return "app-keyboard-settings"sv;
    if (key == "ImeSettings"sv) return "app-keyboard-settings"sv;
    if (key == "MouseSettings"sv) return "app-mouse"sv;
    if (key == "FileAssociations"sv) return "app-file-manager"sv;
    return "app-settings"sv;
}

// --- Applets ---------------------------------------------------------------

struct Applet {
    NonnullRefPtr<Desktop::AppFile> app_file;
    ByteString key;        // basename of the Serenity executable, e.g. DisplaySettings
    ByteString name;       // short display name ("Display Settings" -> "Display")
    ByteString executable; // host-resolved path we actually spawn
    ByteString description;
    GUI::Icon icon;
    bool requires_root { false };
};

struct Task {
    StringView label;
    StringView target; // applet key
};

struct Category {
    StringView title;
    StringView icon_name;
    Vector<Task> tasks;
    Vector<StringView> members;
};

static Category s_categories[] = {
    { "外观和主题"sv, "app-display-settings"sv,
        { { "更改桌面背景"sv, "DisplaySettings"sv },
          { "设置屏幕分辨率"sv, "DisplaySettings"sv },
          { "更改终端外观"sv, "TerminalSettings"sv } },
        { "DisplaySettings"sv, "TerminalSettings"sv, "GamesSettings"sv } },
    { "网络和 Internet 连接"sv, "network"sv,
        { { "配置网络连接"sv, "NetworkSettings"sv },
          { "配置邮件"sv, "MailSettings"sv },
          { "管理证书"sv, "CertificateSettings"sv } },
        { "NetworkSettings"sv, "MailSettings"sv, "CertificateSettings"sv, "MapsSettings"sv, "BrowserSettings"sv } },
    { "添加或删除程序"sv, "app-settings"sv,
        { { "管理 Windows 程序"sv, "WinAppsSettings"sv },
          { "游戏设置"sv, "GamesSettings"sv } },
        { "WinAppsSettings"sv, "GamesSettings"sv } },
    { "用户帐户"sv, "app-user-settings"sv,
        { { "管理用户"sv, "UsersSettings"sv } },
        { "UsersSettings"sv } },
    { "日期、时间、语言和区域设置"sv, "app-calendar"sv,
        { { "设置日期和时间"sv, "ClockSettings"sv },
          { "更改键盘布局"sv, "KeyboardSettings"sv },
          { "配置中文输入法"sv, "ImeSettings"sv } },
        { "ClockSettings"sv, "CalendarSettings"sv, "KeyboardSettings"sv, "ImeSettings"sv } },
    { "打印机和其它硬件"sv, "app-mouse"sv,
        { { "更改鼠标设置"sv, "MouseSettings"sv },
          { "更改键盘设置"sv, "KeyboardSettings"sv } },
        { "MouseSettings"sv, "KeyboardSettings"sv } },
    { "文件和文件夹"sv, "app-file-manager"sv,
        { { "更改文件类型关联"sv, "FileAssociations"sv } },
        { "FileAssociations"sv } },
    // Filled at runtime with any installed applet not claimed above, so no
    // settings program becomes unreachable just because it is uncategorized.
    { "其它选项"sv, "app-settings"sv,
        {}, {} },
};

// --- Small UI helpers ------------------------------------------------------

GUI::Label& add_label(GUI::Widget& parent, String text, bool white = false, bool bold = false, int height = 20)
{
    auto& label = parent.add<GUI::Label>(move(text));
    label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    label.set_fixed_height(height);
    if (white)
        label.set_foreground_role(Gfx::ColorRole::White);
    if (bold)
        label.set_font(Gfx::FontDatabase::default_font().bold_variant());
    return label;
}

GUI::LinkLabel& add_link(GUI::Widget& parent, String text, Function<void()> on_click, bool white = false, bool bold = false)
{
    auto link = MUST(GUI::LinkLabel::try_create(move(text)));
    link->on_click = move(on_click);
    link->set_text_alignment(Gfx::TextAlignment::CenterLeft);
    link->set_fixed_height(20);
    if (white)
        link->set_foreground_role(Gfx::ColorRole::White);
    if (bold)
        link->set_font(Gfx::FontDatabase::default_font().bold_variant());
    parent.add_child(*link);
    return *link;
}

void add_vspace(GUI::Widget& parent, int height)
{
    auto& widget = parent.add<GUI::Widget>();
    widget.set_fixed_height(height);
}

GUI::Widget& make_content(GUI::ScrollableContainerWidget& scroll)
{
    auto content = GUI::Widget::construct();
    auto& widget = *content;
    scroll.set_widget(content);
    widget.set_fill_with_background_color(true);
    widget.set_background_role(Gfx::ColorRole::Base);
    widget.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10, 12, 10, 12 }, 2);
    return widget;
}

// --- XP Luna chrome --------------------------------------------------------

class LunaHeader final : public GUI::Widget {
    C_OBJECT(LunaHeader)
public:
    virtual ~LunaHeader() override = default;

private:
    LunaHeader() = default;

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.fill_rect_with_gradient(Gfx::Orientation::Horizontal, rect(),
            Gfx::Color::from_rgb(0x14357f), Gfx::Color::from_rgb(0x6b9ce8));
        painter.fill_rect({ 0, height() - 2, width(), 2 }, Gfx::Color::from_rgb(0x0a2358));
    }
};

class LunaTaskPane final : public GUI::Widget {
    C_OBJECT(LunaTaskPane)
public:
    virtual ~LunaTaskPane() override = default;

private:
    LunaTaskPane() = default;

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(),
            Gfx::Color::from_rgb(0x6a9ae4), Gfx::Color::from_rgb(0x1b409f));
        painter.fill_rect({ 0, 0, width(), 2 }, Gfx::Color::from_rgb(0xa9c9f4));
    }
};

// --- Classic-view model ----------------------------------------------------

class SettingsAppsModel final : public GUI::Model {
public:
    explicit SettingsAppsModel(Vector<Applet*> applets)
        : m_applets(move(applets))
    {
    }

    virtual int row_count(GUI::ModelIndex const&) const override { return m_applets.size(); }
    virtual int column_count(GUI::ModelIndex const&) const override { return 1; }

    virtual GUI::ModelIndex index(int row, int column, GUI::ModelIndex const&) const override
    {
        if (row < 0 || row >= static_cast<int>(m_applets.size()))
            return {};
        return create_index(row, column, m_applets[row]);
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        auto& applet = *m_applets[index.row()];
        if (role == GUI::ModelRole::Icon)
            return applet.icon;
        if (role == GUI::ModelRole::Display)
            return applet.name;
        return {};
    }

private:
    Vector<Applet*> m_applets;
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio thread recvfd sendfd rpath cpath wpath unix proc exec"));

    auto app = TRY(GUI::Application::create(arguments));

    TRY(Core::System::pledge("stdio thread recvfd sendfd rpath cpath wpath proc exec"));

    auto app_icon = icon_or_fallback("app-settings"sv);

    // --- Collect installed settings applets --------------------------------
    Vector<Applet> applets;
    Desktop::AppFile::for_each([&](NonnullRefPtr<Desktop::AppFile> app_file) {
        if (app_file->category() != "Settings"sv)
            return;

        auto executable = PortHostify::hostify_executable(app_file->executable());
        if (access(executable.characters(), X_OK) != 0) {
            dbgln("Control Panel: skipping {} ({}) - not installed", app_file->name(), executable);
            return;
        }

        ByteString name = app_file->name();
        if (name.ends_with(" Settings"sv))
            name = name.substring(0, name.length() - " Settings"sv.length());

        auto key = LexicalPath::basename(app_file->executable());
        auto icon = icon_or_fallback(icon_name_for_key(key.view()));
        applets.append({
            app_file,
            move(key),
            move(name),
            move(executable),
            app_file->description(),
            move(icon),
            app_file->requires_root(),
        });
    });
    quick_sort(applets, [](auto& a, auto& b) { return a.name < b.name; });

    // The vector is final now: pointers into it stay valid for the rest of the
    // program, which is what the category/task links capture.
    HashMap<ByteString, Applet*> by_key;
    for (auto& applet : applets)
        by_key.set(applet.key, &applet);

    // --- Build the usable category list ------------------------------------
    Vector<Category> categories;
    for (auto const& category : s_categories)
        categories.append(category);

    // Claim every applet that appears in a real category; the rest go to "其它".
    HashMap<ByteString, bool> claimed;
    for (size_t i = 0; i + 1 < categories.size(); ++i)
        for (auto const& key : categories[i].members)
            claimed.set(ByteString { key }, true);

    auto& other = categories.last();
    for (auto& applet : applets) {
        if (claimed.contains(applet.key))
            continue;
        other.members.append(applet.key.view());
        other.tasks.append({ applet.name.view(), applet.key.view() });
    }

    // Drop empty categories (e.g. an applet missing from this build).
    Vector<Category> usable_categories;
    for (auto& category : categories) {
        bool has_installed_member = false;
        for (auto const& key : category.members) {
            if (by_key.contains(ByteString { key })) {
                has_installed_member = true;
                break;
            }
        }
        if (has_installed_member)
            usable_categories.append(move(category));
    }
    if (usable_categories.is_empty()) {
        Category fallback { "控制面板"sv, "app-settings"sv, {}, {} };
        usable_categories.append(move(fallback));
    }

    // --- Window + menus ----------------------------------------------------
    auto window = GUI::Window::construct();
    window->set_title("Control Panel");
    window->resize(820, 570);
    // set_minimum_size() VERIFYs that the window is not obeying the widget's
    // computed minimum (it defaults to true on this port). Terminal does the
    // same; without this the control panel aborts before it ever shows.
    window->set_obey_widget_min_size(false);
    window->set_minimum_size(640, 420);

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::CommonActions::make_quit_action([&](auto&) {
        app->quit();
    }));

    auto view_menu = window->add_menu("&View"_string);
    view_menu->add_action(GUI::CommonActions::make_fullscreen_action([&](auto&) {
        window->set_fullscreen(!window->is_fullscreen());
    }));

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::CommonActions::make_command_palette_action(window));
    help_menu->add_action(GUI::CommonActions::make_about_action("Control Panel"_string, app_icon, window));

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_background_role(Gfx::ColorRole::Base);
    main_widget->set_layout<GUI::VerticalBoxLayout>(0, 0);

    // --- Header band -------------------------------------------------------
    auto& header = main_widget->add<LunaHeader>();
    header.set_fixed_height(56);
    header.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 10, 10, 10, 10 }, 8);

    auto& header_icon = header.add<GUI::ImageWidget>();
    header_icon.set_fixed_width(32);
    header_icon.set_fixed_height(32);
    header_icon.set_bitmap(app_icon.bitmap_for_size(32));

    auto& header_titles = header.add<GUI::Widget>();
    header_titles.set_layout<GUI::VerticalBoxLayout>(0, 0);
    add_label(header_titles, "控制面板"_string, true, true, 22);
    add_label(header_titles, "Control Panel"_string, true, false, 16);

    header.add_spacer();

    auto& back_button = header.add<GUI::Button>("← 后退"_string);
    back_button.set_fixed_width(88);
    back_button.set_enabled(false);
    auto& home_button = header.add<GUI::Button>("主页"_string);
    home_button.set_fixed_width(72);
    home_button.set_enabled(false);
    auto& view_button = header.add<GUI::Button>("切换到经典视图"_string);
    view_button.set_fixed_width(132);

    // --- Body --------------------------------------------------------------
    auto& body = main_widget->add<GUI::Widget>();
    body.set_preferred_size({ GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow });
    body.set_layout<GUI::HorizontalBoxLayout>(0, 0);

    auto& task_pane = body.add<LunaTaskPane>();
    task_pane.set_fixed_width(212);
    task_pane.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14, 12, 12, 12 }, 6);

    auto& stack = body.add<GUI::StackWidget>();
    stack.set_preferred_size({ GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow });

    auto& statusbar = main_widget->add<GUI::Statusbar>();

    // --- Launch helper -----------------------------------------------------
    auto launch = [&](Applet const& applet) {
        statusbar.set_text(ui_string(ByteString::formatted("正在打开 {}…", applet.name)));
        if (applet.requires_root) {
            // "/bin/Escalator" is a Serenity path. On the Linux port the helper
            // is installed as serenity-escalator, so resolve it with the same
            // rule as the applets. If it is not installed, launch directly
            // rather than showing a "file not found" error box.
            auto escalator = PortHostify::hostify_executable(ByteString { "/bin/Escalator" });
            if (access(escalator.characters(), X_OK) == 0)
                GUI::Process::spawn_or_show_error(window.ptr(), escalator.view(), Array { applet.executable });
            else
                GUI::Process::spawn_or_show_error(window.ptr(), applet.executable.view());
        } else {
            GUI::Process::spawn_or_show_error(window.ptr(), applet.executable.view());
        }
    };

    // --- Views -------------------------------------------------------------
    // 0 = home, 1..N = category details, N+1 = classic view.
    Vector<GUI::Widget*> views;
    GUI::LinkLabel* pane_toggle = nullptr;

    // Declared up front: the widgets below capture these by reference and only
    // invoke them at runtime, after the navigation section assigns them.
    Function<void()> show_home;
    Function<void()> show_classic;
    Function<void()> toggle_view;
    Function<void(size_t)> show_category;

    auto& home_scroll = stack.add<GUI::ScrollableContainerWidget>();
    views.append(&home_scroll);
    home_scroll.set_should_hide_unnecessary_scrollbars(true);
    auto& home_content = make_content(home_scroll);

    add_label(home_content, "选择一个类别"_string, false, true, 26);
    add_label(home_content, "单击类别查看其中的设置项，或单击任务直接打开。"_string, false, false, 20);
    add_vspace(home_content, 8);

    for (size_t i = 0; i < usable_categories.size(); ++i) {
        auto const& category = usable_categories[i];

        int task_count = 0;
        for (auto const& task : category.tasks) {
            if (by_key.contains(ByteString { task.target }))
                ++task_count;
        }

        auto& row = home_content.add<GUI::Widget>();
        row.set_fixed_height(max(48, 26 + 20 * task_count));
        row.set_layout<GUI::HorizontalBoxLayout>(0, 8);

        auto& icon_widget = row.add<GUI::ImageWidget>();
        icon_widget.set_fixed_width(32);
        icon_widget.set_fixed_height(32);
        auto category_icon = icon_or_fallback(category.icon_name);
        icon_widget.set_bitmap(category_icon.bitmap_for_size(32));

        auto& column = row.add<GUI::Widget>();
        column.set_layout<GUI::VerticalBoxLayout>(0, 1);

        add_link(column, ui_string(category.title), [&, i] { show_category(i); }, false, true);
        for (auto const& task : category.tasks) {
            auto it = by_key.find(ByteString { task.target });
            if (it == by_key.end())
                continue;
            Applet const* applet = it->value;
            add_link(column, ui_string(task.label), [&, applet] { launch(*applet); });
        }

        add_vspace(home_content, 6);
    }

    // Category detail pages.
    for (size_t i = 0; i < usable_categories.size(); ++i) {
        auto const& category = usable_categories[i];
        auto& scroll = stack.add<GUI::ScrollableContainerWidget>();
        views.append(&scroll);
        scroll.set_should_hide_unnecessary_scrollbars(true);
        auto& content = make_content(scroll);

        add_label(content, ui_string(ByteString::formatted("控制面板 » {}", category.title)), false, true, 26);
        add_label(content, "单击下面的项目以打开设置程序。"_string, false, false, 20);
        add_vspace(content, 8);

        for (auto const& key : category.members) {
            auto it = by_key.find(ByteString { key });
            if (it == by_key.end())
                continue;
            Applet const* applet = it->value;

            auto& row = content.add<GUI::Widget>();
            row.set_fixed_height(50);
            row.set_layout<GUI::HorizontalBoxLayout>(0, 8);

            auto& icon_widget = row.add<GUI::ImageWidget>();
            icon_widget.set_fixed_width(32);
            icon_widget.set_fixed_height(32);
            icon_widget.set_bitmap(applet->icon.bitmap_for_size(32));

            auto& column = row.add<GUI::Widget>();
            column.set_layout<GUI::VerticalBoxLayout>(0, 1);
            add_link(column, ui_string(applet->name.view()), [&, applet] { launch(*applet); }, false, true);
            add_label(column, ui_string(applet->description.view()), false, false, 18);

            add_vspace(content, 4);
        }
    }

    // Classic view: the whole (filtered) applet set as an icon grid.
    auto& classic_container = stack.add<GUI::Widget>();
    views.append(&classic_container);
    classic_container.set_fill_with_background_color(true);
    classic_container.set_background_role(Gfx::ColorRole::Base);
    classic_container.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10, 12, 10, 12 }, 4);
    add_label(classic_container, "经典视图"_string, false, true, 26);
    add_label(classic_container, "双击图标打开对应的设置程序。"_string, false, false, 20);

    Vector<Applet*> classic_applets;
    for (auto& applet : applets)
        classic_applets.append(&applet);

    auto classic_model = adopt_ref(*new SettingsAppsModel(classic_applets));
    auto& icon_view = classic_container.add<GUI::IconView>();
    icon_view.set_should_hide_unnecessary_scrollbars(true);
    icon_view.set_model(*classic_model);
    icon_view.on_activation = [&](GUI::ModelIndex const& index) {
        if (!index.is_valid())
            return;
        launch(*classic_applets[index.row()]);
    };
    icon_view.on_selection_change = [&] {
        auto index = icon_view.selection().first();
        if (!index.is_valid()) {
            statusbar.set_text("经典视图：双击图标打开设置程序。"_string);
            return;
        }
        statusbar.set_text(ui_string(classic_applets[index.row()]->description.view()));
    };

    size_t const classic_index = views.size() - 1;

    // --- Task pane contents ------------------------------------------------
    add_label(task_pane, "控制面板"_string, true, true, 24);
    auto& pane_rule = task_pane.add<GUI::Widget>();
    pane_rule.set_fixed_height(2);
    pane_rule.set_fill_with_background_color(true);
    pane_rule.set_background_role(Gfx::ColorRole::White);

    pane_toggle = &add_link(task_pane, "切换到经典视图"_string, [&] { toggle_view(); }, true);
    add_vspace(task_pane, 10);
    add_label(task_pane, "另请参阅"_string, true, true, 22);

    auto add_see_also = [&](String label, StringView key) {
        auto it = by_key.find(ByteString { key });
        if (it == by_key.end())
            return;
        Applet const* applet = it->value;
        add_link(task_pane, move(label), [&, applet] { launch(*applet); }, true);
    };
    add_see_also("显示属性"_string, "DisplaySettings"sv);
    add_see_also("鼠标属性"_string, "MouseSettings"sv);
    add_see_also("Windows 程序管理"_string, "WinAppsSettings"sv);
    add_see_also("网络连接"_string, "NetworkSettings"sv);

    task_pane.add_spacer();
    add_label(task_pane, "使用分类视图快速查找设置项。"_string, true, false, 34);

    // --- Navigation --------------------------------------------------------
    auto activate = [&](size_t index) {
        stack.set_active_widget(views[index]);
        back_button.set_enabled(index != 0);
        home_button.set_enabled(index != 0);
    };

    show_category = [&](size_t index) {
        activate(1 + index);
        statusbar.set_text(ui_string(ByteString::formatted("“{}”中的设置项", usable_categories[index].title)));
    };

    show_home = [&] {
        view_button.set_text("切换到经典视图"_string);
        pane_toggle->set_text("切换到经典视图"_string);
        statusbar.set_text("选择一个类别或任务以打开对应的设置。"_string);
        activate(0);
    };
    show_classic = [&] {
        view_button.set_text("切换到分类视图"_string);
        pane_toggle->set_text("切换到分类视图"_string);
        statusbar.set_text("经典视图：双击图标打开设置程序。"_string);
        activate(classic_index);
    };
    toggle_view = [&] {
        if (stack.active_widget() == views[classic_index])
            show_home();
        else
            show_classic();
    };

    back_button.on_click = [&](auto) { show_home(); };
    home_button.on_click = [&](auto) { show_home(); };
    view_button.on_click = [&](auto) { toggle_view(); };

    window->set_icon(app_icon.bitmap_for_size(16));

    // The first stack child is already active; make sure its chrome matches.
    show_home();

    window->show();
    return app->exec();
}
