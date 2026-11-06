/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Spencer Dixon <spencercdixon@gmail.com>
 * Copyright (c) 2023, David Ganz <david.g.ganz@gmail.com>
 * Copyright (c) 2024, Wellington Santos <nyakonyns@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TaskbarWindow.h"
#include <Ports/LinuxGUI/Common/Translation.h>
#ifndef AK_OS_SERENITY
#    include <Ports/LinuxGUI/Common/hostify.h>
#endif
#include "ClockWidget.h"
#include "QuickLaunchWidget.h"
#include "TaskbarButton.h"
#include "TaskbarFrame.h"
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/DesktopIcons.h>
#include <LibGUI/Action.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ConnectionToWindowManagerServer.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <stdio.h>
#include <unistd.h>

// Menu entries are built up front, but on the Linux port some of the apps they
// point at may not be built (SystemMonitor needs LibSymbolication, which is not
// ported). Offering a dead entry only produces "posix_spawn: No such file or
// directory" popups, so skip it instead.
static bool app_is_installed(StringView serenity_path)
{
#ifdef AK_OS_SERENITY
    return access(ByteString { serenity_path }.characters(), X_OK) == 0;
#else
    auto resolved = PortHostify::hostify_executable(ByteString { serenity_path });
    return access(resolved.characters(), X_OK) == 0;
#endif
}

// Windows 2000 taskbar geometry: one flat grey band, no transparency, no
// protruding start orb. The bar is plain button-face with a 1px white top
// edge, exactly like the classic shell.
static constexpr int s_bar_height = 28;

class TaskbarWidget final : public GUI::Widget {
    C_OBJECT(TaskbarWidget);

public:
    virtual ~TaskbarWidget() override = default;

    static ErrorOr<NonnullRefPtr<TaskbarWidget>> create();

protected:
    virtual void context_menu_event(GUI::ContextMenuEvent& event) override
    {
        size_t visible_windows_count = 0;
        WindowList::the().for_each_window([&](auto& window) {
            if (!window.is_minimized())
                visible_windows_count += 1;
        });
        m_show_desktop_action->set_text(visible_windows_count >= 1 ? "Show Desktop" : "Show open Windows");
        m_context_menu->popup(event.screen_position());
    }

private:
    TaskbarWidget() = default;

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());

        // Flat button-face band, full height. The only ornament is the 1px
        // white line along the top: the classic raised taskbar edge against
        // the desktop.
        painter.fill_rect(rect(), palette().button());
        painter.fill_rect({ 0, 0, width(), 1 }, palette().button().lightened(1.35f));
        painter.fill_rect({ 0, 1, width(), 1 }, palette().button().lightened(1.1f));
    }

    virtual void did_layout() override
    {
        WindowList::the().for_each_window([&](auto& window) {
            if (auto* button = window.button())
                static_cast<TaskbarButton*>(button)->update_taskbar_rect();
        });
    }

    ErrorOr<void> create_context_menu();

    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Action> m_show_desktop_action;
};

ErrorOr<NonnullRefPtr<TaskbarWidget>> TaskbarWidget::create()
{
    auto widget = TaskbarWidget::construct();
    TRY(widget->create_context_menu());
    return widget;
}

ErrorOr<void> TaskbarWidget::create_context_menu()
{
    m_context_menu = GUI::Menu::construct();

    m_show_desktop_action = GUI::Action::create("Show Desktop", Desktop::icon("desktop"sv), [](auto&) {
        GUI::ConnectionToWindowManagerServer::the().async_toggle_show_desktop();
    });

    auto open_settings_action = GUI::Action::create("&Settings", Desktop::icon("settings"sv), [this](auto&) {
        GUI::Process::spawn_or_show_error(window(), "/bin/Settings"sv);
    });

    auto open_system_monitor_action = GUI::Action::create("System &Monitor", Desktop::icon("app-system-monitor"sv), [this](auto&) {
        GUI::Process::spawn_or_show_error(window(), "/bin/SystemMonitor"sv);
    });

    bool added_app_entry = false;
    if (app_is_installed("/bin/SystemMonitor"sv)) {
        m_context_menu->add_action(*open_system_monitor_action);
        added_app_entry = true;
    }
    if (app_is_installed("/bin/Settings"sv)) {
        m_context_menu->add_action(*open_settings_action);
        added_app_entry = true;
    }
    // 音量不再放在这里：托盘里有了独立的音量按钮（见 populate_taskbar）。
    dbgln("taskbar: context menu entries: SystemMonitor={} Settings={}",
        app_is_installed("/bin/SystemMonitor"sv), app_is_installed("/bin/Settings"sv));
    if (added_app_entry)
        m_context_menu->add_separator();
    m_context_menu->add_action(*m_show_desktop_action);

    return {};
}

ErrorOr<NonnullRefPtr<TaskbarWindow>> TaskbarWindow::create()
{
    auto window = TRY(AK::adopt_nonnull_ref_or_enomem(new (nothrow) TaskbarWindow()));
    TRY(window->populate_taskbar());
    TRY(window->load_assistant());
    return window;
}

TaskbarWindow::TaskbarWindow()
{
    set_window_type(GUI::WindowType::Taskbar);
    set_title("Taskbar");

    on_screen_rects_change(GUI::Desktop::the().rects(), GUI::Desktop::the().main_screen_index());
}

ErrorOr<void> TaskbarWindow::populate_taskbar()
{
    auto main_widget = TRY(TaskbarWidget::create());
    // The widget paints its own flat grey band; never fill with the theme's
    // (possibly light) Window colour underneath it.
    main_widget->set_fill_with_background_color(false);
    set_main_widget(main_widget);

    main_widget->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 2, 3, 0, 3 });

    // 布局顺序：[开始][快速启动][窗口按钮][托盘]，与之前一致。保持 Win2k 经典
    // 立体按钮（StylePainter 的 Normal 样式）以及 56x22 的尺寸、28px 任务栏高度，
    // 只把 Serenity 的小球徽标（ladyball）加回开始按钮。icon_spacing=0 让 16px
    // 徽标紧贴「开始」文字，这样不必加宽按钮也不会挤掉文字。
    m_start_button = GUI::Button::construct();
    m_start_button->set_text("开始"_string);
    m_start_button->set_button_style(Gfx::ButtonStyle::Normal);
    m_start_button->set_font(Gfx::FontDatabase::default_font().bold_variant());
    m_start_button->set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/ladyball.png"sv)));
    m_start_button->set_icon_spacing(0);
    m_start_button->set_fixed_size(56, 22);
    main_widget->add_child(*m_start_button);

    // 快速启动：用普通按钮从目录读，替代 QuickLaunchWidget（那个自绘 widget 在这套
    // LibGUI 里既画不出来、悬停还会 VERIFY 崩掉整个任务栏）。
    m_quick_launch = TRY(Taskbar::QuickLaunchWidget::create());
    m_quick_launch->set_frame_style(Gfx::FrameStyle::NoFrame);
    m_quick_launch->set_fill_with_background_color(false);
    TRY(main_widget->try_add_child(*m_quick_launch));

    m_task_button_container = main_widget->add<GUI::Widget>();
    m_task_button_container->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 3);
    m_task_button_container->set_fill_with_background_color(false);

    m_default_icon = Desktop::icon("window"sv);

    m_applet_area_container = main_widget->add<TaskbarFrame>();
    // Win2k 的托盘是凹进任务栏的面板（内凹边框），不再是 Vista 玻璃。
    m_applet_area_container->set_frame_style(Gfx::FrameStyle::SunkenBox);
    m_applet_area_container->set_fill_with_background_color(false);

    // 托盘音量按钮：VolumeApplet 作为 applet 窗口时在这套移植里点不到（它被
    // 任务栏窗口盖住），所以做成任务栏自己的按钮，点了开一个独立的音量窗口。
    m_volume_button = main_widget->add<GUI::Button>();
    m_volume_button->set_tooltip("Volume"_string);
    m_volume_button->set_button_style(Gfx::ButtonStyle::Normal);
    m_volume_button->set_fill_with_background_color(false);
    m_volume_button->set_fixed_size(22, 22);
    // Embedded icon held in a member: the old code took bitmap_for_size() from
    // a temporary Icon, leaving a dangling pointer (blank volume button).
    m_volume_icon = Desktop::icon("audio-volume-high"sv);
    m_volume_button->set_icon(m_volume_icon.ptr());
    m_volume_button->on_click = [this](unsigned) {
        // 贴着托盘按钮弹出，像 Win9x 的音量托盘飞出面板。
        constexpr int popup_width = 96;
        constexpr int popup_height = 240;
        auto button_rect = m_volume_button->screen_relative_rect();
        auto x = max(0, button_rect.center().x() - popup_width / 2);
        auto y = max(0, button_rect.top() - popup_height - 4);
        Array<ByteString, 2> arguments {
            ByteString { "--window" },
            ByteString::formatted("--position={},{}", x, y),
        };
        GUI::Process::spawn_or_show_error(this, "/bin/VolumeApplet"sv, ReadonlySpan<ByteString> { arguments.data(), arguments.size() });
    };

    m_clock_widget = main_widget->add<Taskbar::ClockWidget>();

    // 输入法指示器：任务栏托盘里的"中/EN"按钮，点开切输入法。只读 addon 发的
    // 状态文件、经控制 socket 发切换命令，不链接任何 fcitx 代码。
    m_ime_button = main_widget->add<GUI::IMEIndicator>();
    m_ime_button->set_fill_with_background_color(false);
    m_ime_button->set_foreground_role(Gfx::ColorRole::TrayText);

    m_show_desktop_button = main_widget->add<GUI::Button>();
    m_show_desktop_button->set_tooltip("Show Desktop"_string);
    m_show_desktop_icon = Desktop::icon("desktop"sv);
    m_show_desktop_button->set_icon(m_show_desktop_icon.ptr());
    m_show_desktop_button->set_button_style(Gfx::ButtonStyle::Normal);
    m_show_desktop_button->set_fill_with_background_color(false);
    m_show_desktop_button->set_fixed_size(22, 22);
    m_show_desktop_button->on_click = TaskbarWindow::show_desktop_button_clicked;

    return {};
}

ErrorOr<void> TaskbarWindow::load_assistant()
{
    auto af_path = TRY(String::formatted("{}/{}", Desktop::AppFile::APP_FILES_DIRECTORY, "Assistant.af"));
    m_assistant_app_file = Desktop::AppFile::open(af_path);

    return {};
}

void TaskbarWindow::add_system_menu(NonnullRefPtr<GUI::Menu> system_menu)
{
    m_system_menu = move(system_menu);

    if (m_start_button)
        m_start_button->set_menu(m_system_menu);
}

void TaskbarWindow::config_string_did_change(StringView domain, StringView group, StringView key, StringView value)
{
    if (domain == "Taskbar" && group == "Clock" && key == "TimeFormat") {
        m_clock_widget->update_format(value);
        update_applet_area();
    }
}

void TaskbarWindow::show_desktop_button_clicked(unsigned)
{
    toggle_show_desktop();
}

void TaskbarWindow::toggle_show_desktop()
{
    GUI::ConnectionToWindowManagerServer::the().async_toggle_show_desktop();
}

void TaskbarWindow::on_screen_rects_change(Vector<Gfx::IntRect, 4> const& rects, size_t main_screen_index)
{
    auto const& rect = rects[main_screen_index];
    Gfx::IntRect new_rect { rect.x(), rect.bottom() - s_bar_height, rect.width(), s_bar_height };
    set_rect(new_rect);
    update_applet_area();
}

void TaskbarWindow::update_applet_area()
{
    // NOTE: Widget layout is normally lazy, but here we have to force it right away so we can tell
    //       WindowServer where to place the applet area window.
    if (!main_widget())
        return;
    main_widget()->do_layout();
    auto new_rect = Gfx::IntRect({}, m_applet_area_size).centered_within(m_applet_area_container->screen_relative_rect());
    GUI::ConnectionToWindowManagerServer::the().async_set_applet_area_position(new_rect.location());
}

NonnullRefPtr<GUI::Button> TaskbarWindow::create_button(WindowIdentifier const& identifier)
{
    auto& button = m_task_button_container->add<TaskbarButton>(identifier);
    button.set_min_size(20, 22);
    button.set_max_size(140, 22);
    button.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    button.set_icon(*m_default_icon);
    return button;
}

void TaskbarWindow::add_window_button(::Window& window, WindowIdentifier const& identifier)
{
    if (window.button())
        return;
    window.set_button(create_button(identifier));
    auto* button = window.button();
    button->on_click = [window = &window, identifier](auto) {
        if (window->is_minimized() || !window->is_active())
            GUI::ConnectionToWindowManagerServer::the().async_set_active_window(identifier.client_id(), identifier.window_id());
        else if (!window->is_blocked())
            GUI::ConnectionToWindowManagerServer::the().async_set_window_minimized(identifier.client_id(), identifier.window_id(), true);
    };
}

void TaskbarWindow::remove_window_button(::Window& window, bool was_removed)
{
    auto* button = window.button();
    if (!button)
        return;
    if (!was_removed)
        static_cast<TaskbarButton*>(button)->clear_taskbar_rect();
    window.set_button(nullptr);
    button->remove_from_parent();
}

void TaskbarWindow::update_window_button(::Window& window, bool show_as_active)
{
    auto* button = window.button();
    if (!button)
        return;
    button->set_text(String::from_byte_string(window.title()).release_value_but_fixme_should_propagate_errors());
    button->set_tooltip(String::from_byte_string(window.title()).release_value_but_fixme_should_propagate_errors());
    button->set_checked(show_as_active);
    button->set_visible(is_window_on_current_workspace(window));
}

void TaskbarWindow::event(Core::Event& event)
{
    switch (event.type()) {
    case GUI::Event::MouseDown: {
        // If the cursor is at the edge/corner of the screen but technically not within the start button (or other taskbar buttons),
        // we adjust it so that the nearest button ends up being clicked anyways.

        auto& mouse_event = static_cast<GUI::MouseEvent&>(event);
        int const ADJUSTMENT = 4;
        auto adjusted_x = AK::clamp(mouse_event.x(), ADJUSTMENT, width() - ADJUSTMENT);
        auto adjusted_y = AK::min(mouse_event.y(), height() - ADJUSTMENT);
        Gfx::IntPoint adjusted_point = { adjusted_x, adjusted_y };

        if (adjusted_point != mouse_event.position()) {
            GUI::ConnectionToWindowServer::the().async_set_global_cursor_position(position() + adjusted_point);
            GUI::MouseEvent adjusted_event = { (GUI::Event::Type)mouse_event.type(), adjusted_point, mouse_event.buttons(), mouse_event.button(), mouse_event.modifiers(), mouse_event.wheel_delta_x(), mouse_event.wheel_delta_y(), mouse_event.wheel_raw_delta_x(), mouse_event.wheel_raw_delta_y() };
            Window::event(adjusted_event);
            return;
        }
        break;
    }
    case GUI::Event::FontsChange:
        set_start_button_font(Gfx::FontDatabase::default_font().bold_variant());
        break;
    default:
        break;
    }
    Window::event(event);
}

void TaskbarWindow::wm_event(GUI::WMEvent& event)
{
    WindowIdentifier identifier { event.client_id(), event.window_id() };
    switch (event.type()) {
    case GUI::Event::WM_WindowRemoved: {
        if constexpr (EVENT_DEBUG) {
            auto& removed_event = static_cast<GUI::WMWindowRemovedEvent&>(event);
            dbgln("WM_WindowRemoved: client_id={}, window_id={}",
                removed_event.client_id(),
                removed_event.window_id());
        }
        if (auto* window = WindowList::the().window(identifier))
            remove_window_button(*window, true);
        WindowList::the().remove_window(identifier);
        update();
        break;
    }
    case GUI::Event::WM_WindowRectChanged: {
        if constexpr (EVENT_DEBUG) {
            auto& changed_event = static_cast<GUI::WMWindowRectChangedEvent&>(event);
            dbgln("WM_WindowRectChanged: client_id={}, window_id={}, rect={}",
                changed_event.client_id(),
                changed_event.window_id(),
                changed_event.rect());
        }
        break;
    }

    case GUI::Event::WM_WindowIconBitmapChanged: {
        auto& changed_event = static_cast<GUI::WMWindowIconBitmapChangedEvent&>(event);
        if (auto* window = WindowList::the().window(identifier)) {
            if (window->button()) {
                auto icon = changed_event.bitmap();
                if (icon->height() != taskbar_icon_size() || icon->width() != taskbar_icon_size()) {
                    auto sw = taskbar_icon_size() / (float)icon->width();
                    auto sh = taskbar_icon_size() / (float)icon->height();
                    auto scaled_bitmap_or_error = icon->scaled(sw, sh);
                    if (scaled_bitmap_or_error.is_error())
                        window->button()->set_icon(nullptr);
                    else
                        window->button()->set_icon(scaled_bitmap_or_error.release_value());
                } else {
                    window->button()->set_icon(icon);
                }
            }
        }
        break;
    }

    case GUI::Event::WM_WindowStateChanged: {
        auto& changed_event = static_cast<GUI::WMWindowStateChangedEvent&>(event);
        if constexpr (EVENT_DEBUG) {
            dbgln("WM_WindowStateChanged: client_id={}, window_id={}, title={}, rect={}, is_active={}, is_blocked={}, is_minimized={}",
                changed_event.client_id(),
                changed_event.window_id(),
                changed_event.title(),
                changed_event.rect(),
                changed_event.is_active(),
                changed_event.is_blocked(),
                changed_event.is_minimized());
        }
        // Only the window type filters here: frameless Normal windows are real
        // app windows too on this port (native Wayland clients with client-side
        // decorations), so they must get a taskbar button as well. Unmanaged
        // surfaces (menus, tooltips, popups) arrive as WindowType::Popup.
        if (changed_event.window_type() != GUI::WindowType::Normal) {
            if (auto* window = WindowList::the().window(identifier))
                remove_window_button(*window, false);
            break;
        }
        auto& window = WindowList::the().ensure_window(identifier);
        window.set_title(changed_event.title());
        window.set_rect(changed_event.rect());
        window.set_active(changed_event.is_active());
        window.set_blocked(changed_event.is_blocked());
        window.set_minimized(changed_event.is_minimized());
        window.set_progress(changed_event.progress());
        window.set_workspace(changed_event.workspace_row(), changed_event.workspace_column());
        add_window_button(window, identifier);
        update_window_button(window, window.is_active());
        break;
    }
    case GUI::Event::WM_AppletAreaSizeChanged: {
        auto& changed_event = static_cast<GUI::WMAppletAreaSizeChangedEvent&>(event);
        m_applet_area_size = changed_event.size();
        m_applet_area_container->set_fixed_size(changed_event.size().width() + 8, 22);
        update_applet_area();
        break;
    }
    case GUI::Event::WM_SuperKeyPressed: {
        if (!m_system_menu)
            break;

        if (m_system_menu->is_visible()) {
            m_system_menu->dismiss();
        } else {
            m_system_menu->popup(m_start_button->screen_relative_rect().top_left());
        }
        break;
    }
    case GUI::Event::WM_SuperSpaceKeyPressed: {
        if (auto result = m_assistant_app_file->spawn(); result.is_error())
            warnln("Failed to spawn 'Assistant' when requested via Super+Space: {}", result.error());
        break;
    }
    case GUI::Event::WM_SuperDKeyPressed: {
        toggle_show_desktop();
        break;
    }
    case GUI::Event::WM_SuperDigitKeyPressed: {
        auto& digit_event = static_cast<GUI::WMSuperDigitKeyPressedEvent&>(event);
        auto index = digit_event.digit() != 0 ? digit_event.digit() - 1 : 9;

        for (auto& widget : m_task_button_container->child_widgets()) {
            // NOTE: The button might be invisible depending on the current workspace
            if (!widget.is_visible())
                continue;

            if (index == 0) {
                static_cast<TaskbarButton&>(widget).click();
                break;
            }

            --index;
        }
        break;
    }
    case GUI::Event::WM_WorkspaceChanged: {
        auto& changed_event = static_cast<GUI::WMWorkspaceChangedEvent&>(event);
        workspace_change_event(changed_event.current_row(), changed_event.current_column());
        break;
    }
    case GUI::Event::WM_AddToQuickLaunch: {
        auto add_event = static_cast<GUI::WMAddToQuickLaunchEvent&>(event);
        auto result = m_quick_launch->add_from_pid(add_event.pid());
        if (result.is_error()) {
            dbgln("Couldn't add pid {} to quick launch menu: {}", add_event.pid(), result.error());
            GUI::MessageBox::show_error(this, String::formatted("Failed to add to Quick Launch: {}", result.release_error()).release_value_but_fixme_should_propagate_errors());
        } else if (!result.release_value()) {
            dbgln("Couldn't add pid {} to quick launch menu due to an unexpected error", add_event.pid());
            GUI::MessageBox::show_error(this, "Failed to add to Quick Launch due to an unexpected error."sv);
        }
        break;
    }
    default:
        break;
    }
}

void TaskbarWindow::screen_rects_change_event(GUI::ScreenRectsChangeEvent& event)
{
    on_screen_rects_change(event.rects(), event.main_screen_index());
}

bool TaskbarWindow::is_window_on_current_workspace(::Window& window) const
{
    return window.workspace_row() == m_current_workspace_row && window.workspace_column() == m_current_workspace_column;
}

void TaskbarWindow::workspace_change_event(unsigned current_row, unsigned current_column)
{
    m_current_workspace_row = current_row;
    m_current_workspace_column = current_column;

    WindowList::the().for_each_window([&](auto& window) {
        if (auto* button = window.button())
            button->set_visible(is_window_on_current_workspace(window));
    });
}

void TaskbarWindow::set_start_button_font(Gfx::Font const& font)
{
    m_start_button->set_font(font);
}
