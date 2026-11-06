/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <AK/QuickSort.h>
#include <AK/TypedTransfer.h>
#include <Applications/TerminalSettings/Defaults.h>
#include <LibConfig/Client.h>
#include <LibConfig/Listener.h>
#include <LibCore/Account.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <cstdio>
#include <LibDesktop/Launcher.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Event.h>
#include <LibGUI/Icon.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Process.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>
#include <LibVT/TerminalWidget.h>
#include <pty.h>

class TerminalChangeListener : public Config::Listener {
public:
    // 一个窗口有多个标签页，配置变更只作用在当前活动的那个上。
    TerminalChangeListener(Function<VT::TerminalWidget&()> terminal)
        : m_terminal(move(terminal))
    {
    }

    virtual void config_bool_did_change(StringView domain, StringView group, StringView key, bool value) override
    {
        VERIFY(domain == "Terminal");

        if (group == "Terminal") {
            if (key == "ShowScrollBar")
                terminal().set_show_scrollbar(value);
            else if (key == "ConfirmClose" && on_confirm_close_changed)
                on_confirm_close_changed(value);
        } else if (group == "Cursor" && key == "Blinking") {
            terminal().set_cursor_blinking(value);
        }
    }

    virtual void config_string_did_change(StringView domain, StringView group, StringView key, StringView value) override
    {
        VERIFY(domain == "Terminal");

        if (group == "Window" && key == "Bell") {
            auto bell_mode = VT::TerminalWidget::parse_bell(value).value_or(VT::TerminalWidget::BellMode::Visible);
            terminal().set_bell_mode(bell_mode);
        } else if (group == "Text" && key == "Font") {
            auto font = Gfx::FontDatabase::the().get_by_name(value);
            if (font.is_null())
                font = Gfx::FontDatabase::default_fixed_width_font();
            terminal().set_font_and_resize_to_fit(*font);
            terminal().apply_size_increments_to_window(*terminal().window());
            terminal().window()->resize(terminal().size());
        } else if (group == "Cursor" && key == "Shape") {
            auto cursor_shape = VT::TerminalWidget::parse_cursor_shape(value).value_or(VT::CursorShape::Block);
            terminal().set_cursor_shape(cursor_shape);
        } else if (group == "Terminal" && key == "AutoMark") {
            auto automark_mode = VT::TerminalWidget::parse_automark_mode(value).value_or(VT::TerminalWidget::AutoMarkMode::MarkInteractiveShellPrompt);
            terminal().set_auto_mark_mode(automark_mode);
        }
    }

    virtual void config_i32_did_change(StringView domain, StringView group, StringView key, i32 value) override
    {
        VERIFY(domain == "Terminal");

        if (group == "Terminal" && key == "MaxHistorySize") {
            terminal().set_max_history_size(value);
        } else if (group == "Window" && key == "Opacity") {
            terminal().set_opacity(value);
        }
    }

    Function<void(bool)> on_confirm_close_changed;

private:
    VT::TerminalWidget& terminal() { return m_terminal(); }

    Function<VT::TerminalWidget&()> m_terminal;
};

static ErrorOr<void> utmp_update(StringView tty, pid_t pid, bool create)
{
    // /bin/utmpupdate is a SerenityOS helper; there is no utmp bookkeeping to do on Linux.
    (void)tty;
    (void)pid;
    (void)create;
#ifdef AK_OS_SERENITY
    auto pid_string = String::number(pid);
    Array utmp_update_command {
        "-f"sv,
        "Terminal"sv,
        "-p"sv,
        pid_string.bytes_as_string_view(),
        (create ? "-c"sv : "-d"sv),
        tty,
    };

    auto utmpupdate_pid = TRY(Core::Process::spawn("/bin/utmpupdate"sv, utmp_update_command, {}, Core::Process::KeepAsChild::Yes));

    Core::System::WaitPidResult status;
    auto wait_successful = false;
    while (!wait_successful) {
        auto result = Core::System::waitpid(utmpupdate_pid, 0);
        if (result.is_error() && result.error().code() != EINTR) {
            return result.release_error();
        } else if (!result.is_error()) {
            status = result.release_value();
            wait_successful = true;
        }
    }

    if (WIFEXITED(status.status) && WEXITSTATUS(status.status) != 0)
        dbgln("Terminal: utmpupdate exited with status {}", WEXITSTATUS(status.status));
    else if (WIFSIGNALED(status.status))
        dbgln("Terminal: utmpupdate exited due to unhandled signal {}", WTERMSIG(status.status));
#endif // AK_OS_SERENITY

    return {};
}

static ErrorOr<void> run_command(StringView command, bool keep_open)
{
    auto shell = TRY(String::from_byte_string(TRY(Core::Account::self(Core::Account::Read::PasswdOnly)).shell()));
    if (shell.is_empty()) {
#ifdef AK_OS_SERENITY
        shell = "/bin/Shell"_string;
#else
        shell = "/bin/bash"_string;
#endif
    }

    Vector<StringView> arguments;
    arguments.append(shell);
    if (!command.is_empty()) {
        if (keep_open)
            arguments.append("--keep-open"sv);
        arguments.append("-c"sv);
        arguments.append(command);
    }
    auto env = TRY(FixedArray<StringView>::create({ "TERM=xterm"sv, "COLORTERM=truecolor"sv, "PAGER=more"sv, "PATH="sv DEFAULT_PATH_SV }));
    TRY(Core::System::exec(shell, arguments, Core::System::SearchInPath::No, env.span()));
    VERIFY_NOT_REACHED();
}

static ErrorOr<NonnullRefPtr<GUI::Window>> create_find_window(VT::TerminalWidget& terminal)
{
    auto window = GUI::Window::construct(&terminal);
    window->set_window_mode(GUI::WindowMode::RenderAbove);
    window->set_title("Find in Terminal");
    window->set_resizable(false);
    window->resize(300, 90);

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_background_role(ColorRole::Button);
    main_widget->set_layout<GUI::VerticalBoxLayout>(4);

    auto& find = main_widget->add<GUI::Widget>();
    find.set_layout<GUI::HorizontalBoxLayout>(4);
    find.set_fixed_height(30);

    auto& find_textbox = find.add<GUI::TextBox>();
    find_textbox.set_fixed_width(230);
    find_textbox.set_focus(true);
    if (terminal.has_selection())
        find_textbox.set_text(terminal.selected_text().replace("\n"sv, " "sv, ReplaceMode::All));
    auto& find_backwards = find.add<GUI::Button>();
    find_backwards.set_fixed_width(25);
    find_backwards.set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/upward-triangle.png"sv)));
    auto& find_forwards = find.add<GUI::Button>();
    find_forwards.set_fixed_width(25);
    find_forwards.set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/downward-triangle.png"sv)));

    find_textbox.on_return_pressed = [&find_backwards] {
        find_backwards.click();
    };

    find_textbox.on_shift_return_pressed = [&find_forwards] {
        find_forwards.click();
    };

    auto& match_case = main_widget->add<GUI::CheckBox>("Case sensitive"_string);
    auto& wrap_around = main_widget->add<GUI::CheckBox>("Wrap around"_string);

    find_backwards.on_click = [&terminal, &find_textbox, &match_case, &wrap_around](auto) {
        auto needle = find_textbox.text();
        if (needle.is_empty()) {
            return;
        }

        auto found_range = terminal.find_previous(needle, terminal.normalized_selection().start(), match_case.is_checked(), wrap_around.is_checked());

        if (found_range.is_valid()) {
            terminal.scroll_to_row(found_range.start().row());
            terminal.set_selection(found_range);
        }
    };
    find_forwards.on_click = [&terminal, &find_textbox, &match_case, &wrap_around](auto) {
        auto needle = find_textbox.text();
        if (needle.is_empty()) {
            return;
        }

        auto found_range = terminal.find_next(needle, terminal.normalized_selection().end(), match_case.is_checked(), wrap_around.is_checked());

        if (found_range.is_valid()) {
            terminal.scroll_to_row(found_range.start().row());
            terminal.set_selection(found_range);
        }
    };

    return window;
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio tty rpath cpath wpath recvfd sendfd proc exec unix sigaction"));

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    // Do not trust that both function pointers overlap.
    act.sa_sigaction = nullptr;

    act.sa_flags = SA_NOCLDWAIT;
    act.sa_handler = SIG_IGN;

    TRY(Core::System::sigaction(SIGCHLD, &act, nullptr));

    auto app = TRY(GUI::Application::create(arguments));

    TRY(Core::System::pledge("stdio tty rpath cpath wpath recvfd sendfd proc exec unix"));

    Config::pledge_domain("Terminal");

    StringView command_to_execute;
    bool keep_open = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(command_to_execute, "Execute this command inside the terminal", nullptr, 'e', "command");
    args_parser.add_option(keep_open, "Keep the terminal open after the command has finished executing", nullptr, 'k');

    args_parser.parse(arguments);

    if (keep_open && command_to_execute.is_empty()) {
        warnln("Option -k can only be used in combination with -e.");
        return 1;
    }

    auto app_icon = GUI::Icon::default_icon("app-terminal"sv);

    auto window = GUI::Window::construct();
    window->set_title("Terminal");
    window->set_obey_widget_min_size(false);
    window->set_icon(app_icon.bitmap_for_size(16));
    // 记住上次窗口大小/位置（domain "Terminal" / group "WindowGeometry"），关闭时自动落盘。
    window->save_size_and_position_on_close("Terminal"sv, "WindowGeometry"sv);

    // 关键：先给窗口一个合理的默认矩形，再 add_terminal_tab()。
    // 否则窗口此时还是构造默认值（0/未布局），TerminalWidget 的 relayout() 会拿到垃圾尺寸，
    // 用 set_size(负数) 算出 u16 回绕的 65535 列——preferred_content_size() 跟着爆炸（720887×34），
    // restore 又把它当兜底设回窗口，形成“越存越大”的死循环。给定默认矩形后，首次 relayout
    // 就能算出正常的 80×25 量级，restore 兜底也是正常的。
    window->set_rect({ 100, 100, 720, 480 });

    // 几何记忆的兜底保存：
    // 1) 正常关闭走 Window::close() → save_size_and_position_on_close 落盘；
    // 2) 但 Terminal 在“最后一个标签页关闭 / shell exit”时走的是 Application::quit(0)，
    //    根本不会触发 Window::close()，所以这里显式在 quit 前先保存一次；
    // 3) 再补一个随 resize 持续 debounce 的保存，应对 WM 异常退出（见 STATUS_XWM_SEGV.md）。
    // 注意：定时器一定要传 parent（窗口的 EventReceiver），否则无父定时器不会被事件循环派发。
    auto geometry_save_timer = Core::Timer::create_single_shot(400, [&window] {
        window->save_size_and_position("Terminal"sv, "WindowGeometry"sv);
    }, window.ptr());
    auto quit_app = [&window] {
        window->save_size_and_position("Terminal"sv, "WindowGeometry"sv);
        GUI::Application::the()->quit(0);
    };

    Config::monitor_domain("Terminal");

    auto should_confirm_close = Config::read_bool("Terminal"sv, "Terminal"sv, "ConfirmClose"sv, Terminal::Defaults::CONFIRM_CLOSE);
    auto new_opacity = Config::read_i32("Terminal"sv, "Window"sv, "Opacity"sv, Terminal::Defaults::OPACITY);
    window->set_has_alpha_channel(new_opacity < 255);

    // 一个窗口多个会话：主控件是 TabWidget，每个标签页有自己的 pty + shell，
    // Ctrl+Shift+T 新建、Ctrl+Shift+W 关闭。
    auto tabs_ref = window->set_main_widget<GUI::TabWidget>();
    auto& tabs = *tabs_ref;
    tabs.set_tab_position(TabPosition::Top);

    // 标签页 → 会话信息。TerminalWidget 自己不暴露 shell pid / ptm fd，
    // 关闭确认和 tcgetpgrp 需要它们。
    HashMap<VT::TerminalWidget*, pid_t> tab_shell_pids;
    HashMap<VT::TerminalWidget*, int> tab_ptm_fds;
    HashMap<VT::TerminalWidget*, NonnullRefPtr<GUI::Window>> tab_find_windows;
    int open_tabs = 0;
    int tab_number = 0;

    auto current_terminal = [&]() -> VT::TerminalWidget& {
        return *static_cast<VT::TerminalWidget*>(tabs.active_widget());
    };

    // 新标签页启动时套用一遍当前配置（配置改动由 listener 负责）。
    auto apply_terminal_config = [](VT::TerminalWidget& widget) {
        widget.set_bell_mode(VT::TerminalWidget::parse_bell(Config::read_string("Terminal"sv, "Window"sv, "Bell"sv)).value_or(Terminal::Defaults::BELL_MODE));
        widget.set_auto_mark_mode(VT::TerminalWidget::parse_automark_mode(Config::read_string("Terminal"sv, "Terminal"sv, "AutoMark"sv)).value_or(Terminal::Defaults::AUTOMARK_MODE));
        widget.set_cursor_shape(VT::TerminalWidget::parse_cursor_shape(Config::read_string("Terminal"sv, "Cursor"sv, "Shape"sv)).value_or(Terminal::Defaults::CURSOR_SHAPE));
        widget.set_cursor_blinking(Config::read_bool("Terminal"sv, "Cursor"sv, "Blinking"sv, Terminal::Defaults::CURSOR_BLINKING));
        widget.set_opacity(Config::read_i32("Terminal"sv, "Window"sv, "Opacity"sv, Terminal::Defaults::OPACITY));
        widget.set_max_history_size(Config::read_i32("Terminal"sv, "Terminal"sv, "MaxHistorySize"sv, Terminal::Defaults::MAX_HISTORY_SIZE));
        widget.set_show_scrollbar(Config::read_bool("Terminal"sv, "Terminal"sv, "ShowScrollBar"sv, Terminal::Defaults::SHOW_SCROLLBAR));
    };

    // 新建标签页要能被右键菜单调用，而菜单项在 lambda 里创建 —— 用一层
    // Function 打断这个先有鸡还是先有蛋。
    Function<void()> request_new_tab;
    Function<void()> request_close_tab;

    auto add_terminal_tab = [&]() -> ErrorOr<void> {
        int ptm_fd = -1;
        pid_t shell_pid = forkpty(&ptm_fd, nullptr, nullptr, nullptr);
        if (shell_pid < 0)
            return Error::from_errno(errno);

        // 子进程：跑启动命令（exec 之后不再返回）。
        if (shell_pid == 0) {
            // 终端进程为了免收尸把 SIGCHLD 设成 SIG_IGN(+SA_NOCLDWAIT)，forkpty
            // 子进程会原样继承，而 bash 又会把它当"原始信号"传给所有前台命令。
            // 实测后果：apt 的 Pre-Install-Pkgs 钩子（apt-listchanges）被内核
            // 自动收尸，apt 的 waitpid() 返回 ECHILD，报
            // "Waited for /usr/bin/apt-listchanges ... but it wasn't there"，
            // 安装直接中断。SIGPIPE 同理被 LibGUI 栈忽略，一并还原为默认处置。
            struct sigaction dfl;
            sigemptyset(&dfl.sa_mask);
            dfl.sa_flags = 0;
            dfl.sa_handler = SIG_DFL;
            (void)sigaction(SIGCHLD, &dfl, nullptr);
            (void)sigaction(SIGPIPE, &dfl, nullptr);
            if (!command_to_execute.is_empty())
                TRY(run_command(command_to_execute, keep_open));
            else
                TRY(run_command(Config::read_string("Terminal"sv, "Startup"sv, "Command"sv, ""sv), false));
            VERIFY_NOT_REACHED();
        }

        auto ptsname = TRY(Core::System::ptsname(ptm_fd));
        TRY(utmp_update(ptsname, shell_pid, true));

        ++tab_number;
        auto title = MUST(String::formatted("Terminal {}", tab_number));
        auto& terminal = tabs.add_tab<VT::TerminalWidget>(title, ptm_fd, true);
        tabs.activate_last_tab();
        terminal.set_focus(true);
        ++open_tabs;

        terminal.set_startup_process_id(shell_pid);
        apply_terminal_config(terminal);
        tab_shell_pids.set(&terminal, shell_pid);
        tab_ptm_fds.set(&terminal, ptm_fd);

        auto find_window = TRY(create_find_window(terminal));
        tab_find_windows.set(&terminal, find_window);

        // 在自己的回调里删掉标签页会把 widget 析构掉，延后一拍再删。
        terminal.on_command_exit = [&tabs, &tab_shell_pids, &tab_ptm_fds, &tab_find_windows, &open_tabs, &request_close_tab, &quit_app, widget = &terminal] {
            Core::deferred_invoke([&tabs, &tab_shell_pids, &tab_ptm_fds, &tab_find_windows, &open_tabs, &request_close_tab, &quit_app, widget] {
                tab_shell_pids.remove(widget);
                tab_ptm_fds.remove(widget);
                tab_find_windows.remove(widget);
                if (widget->parent())
                    tabs.remove_tab(*widget);
                --open_tabs;
                if (open_tabs <= 0)
                    quit_app();
                (void)request_close_tab;
            });
        };
        terminal.on_title_change = [&tabs, &window, widget = &terminal](StringView title) {
            auto title_string = MUST(String::from_utf8(title));
            tabs.set_tab_title(*widget, title_string);
            if (tabs.active_widget() == widget)
                window->set_title(title_string.to_byte_string());
        };
        terminal.on_terminal_size_change = [&window, &tabs, geometry_save_timer](auto size) {
            // 多标签时别按某个会话的字符网格改窗口大小，会来回跳。
            if (tabs.tab_count() <= 1)
                window->resize(size);
            // 窗口尺寸变化（拖动 / 字体缩放）后 debounce 落盘，避免只在关闭时保存（WM 崩溃会丢）。
            geometry_save_timer->restart();
        };

        // 右键菜单：LibVT 自带复制/粘贴/清屏，这里补标签页操作。
        auto& menu = terminal.context_menu();
        menu.add_separator();
        menu.add_action(GUI::Action::create("New Tab", [&request_new_tab](auto&) {
            if (request_new_tab)
                request_new_tab();
        }));
        menu.add_action(GUI::Action::create("Close Tab", [&request_close_tab](auto&) {
            if (request_close_tab)
                request_close_tab();
        }));

        return {};
    };

    request_new_tab = [&] {
        (void)add_terminal_tab();
    };
    request_close_tab = [&] {
        auto& terminal = current_terminal();
        if (auto find_it = tab_find_windows.find(&terminal); find_it != tab_find_windows.end())
            find_it->value->close();
        tab_shell_pids.remove(&terminal);
        tab_ptm_fds.remove(&terminal);
        tab_find_windows.remove(&terminal);
        tabs.remove_tab(terminal);
        --open_tabs;
        if (open_tabs <= 0)
            quit_app();
    };

    TRY(add_terminal_tab());
    current_terminal().apply_size_increments_to_window(*window);

    // 清理遗留：旧实现把窗口几何（像素）写进了 [Terminal] Window 的 Width/Height/X/Y/Maximized，
    // 与 TerminalWidget 的字符网格语义冲突（曾把窗口撑到 720898×34）。现在几何改存 WindowGeometry
    // 组，[Window] 里的这几把几何键已无人读取，清掉以免混淆。注意保留 [Window] 的 Bell/Opacity。
    Config::remove_key("Terminal"sv, "Window"sv, "Width"sv);
    Config::remove_key("Terminal"sv, "Window"sv, "Height"sv);
    Config::remove_key("Terminal"sv, "Window"sv, "X"sv);
    Config::remove_key("Terminal"sv, "Window"sv, "Y"sv);
    Config::remove_key("Terminal"sv, "Window"sv, "Maximized"sv);

    // 防御：若上一轮把损坏的几何（如 720898×34）写进了 WindowGeometry，清掉，
    // 让 restore 回落到 preferred_content_size（80×25 字体尺寸）。正常范围的窗口尺寸保留。
    {
        auto wg_w = Config::read_i32("Terminal"sv, "WindowGeometry"sv, "Width"sv, 0);
        auto wg_h = Config::read_i32("Terminal"sv, "WindowGeometry"sv, "Height"sv, 0);
        if (wg_w <= 0 || wg_h <= 0 || wg_w > 100000 || wg_h > 100000) {
            Config::remove_key("Terminal"sv, "WindowGeometry"sv, "Width"sv);
            Config::remove_key("Terminal"sv, "WindowGeometry"sv, "Height"sv);
            Config::remove_key("Terminal"sv, "WindowGeometry"sv, "X"sv);
            Config::remove_key("Terminal"sv, "WindowGeometry"sv, "Y"sv);
            Config::remove_key("Terminal"sv, "WindowGeometry"sv, "Maximized"sv);
        }
    }

    // 优先恢复上次记忆的窗口大小/位置；首次运行（无存档）回落到字体+网格算出的最佳尺寸，
    // 否则窗口会以未布局前的 0x0 / 极小块打开。
    window->restore_size_and_position("Terminal"sv, "WindowGeometry"sv, current_terminal().preferred_content_size());

    TerminalChangeListener listener { [&current_terminal]() -> VT::TerminalWidget& { return current_terminal(); } };

    auto open_settings_action = GUI::Action::create("Terminal &Settings", TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/settings.png"sv)),
        [&](auto&) {
            GUI::Process::spawn_or_show_error(window, "/bin/TerminalSettings"sv);
        });

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::Action::create("New &Tab", { Mod_Ctrl | Mod_Shift, Key_T }, TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/app-terminal.png"sv)), [&](auto&) {
        request_new_tab();
    }));
    file_menu->add_action(GUI::Action::create("&Close Tab", { Mod_Ctrl | Mod_Shift, Key_W }, [&](auto&) {
        request_close_tab();
    }));
    file_menu->add_separator();
    file_menu->add_action(GUI::Action::create("Open New &Terminal", { Mod_Ctrl | Mod_Shift, Key_N }, TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/app-terminal.png"sv)), [&](auto&) {
        GUI::Process::spawn_or_show_error(window, "/bin/Terminal"sv);
    }));

    file_menu->add_action(open_settings_action);
    file_menu->add_separator();

    // 当前标签页的 shell 有没有前台子进程在跑（关闭前确认用）。
    auto tty_has_foreground_process = [&] {
        auto& terminal = current_terminal();
        auto fd = tab_ptm_fds.get(&terminal).value_or(-1);
        auto pid = tab_shell_pids.get(&terminal).value_or(-1);
        if (fd < 0 || pid < 0)
            return false;
        pid_t fg_pid = tcgetpgrp(fd);
        return fg_pid != -1 && fg_pid != pid;
    };

    auto shell_child_process_count = [&] {
        int background_process_count = 0;
        // /proc/<pid>/children is Serenity-specific; on Linux just report no children.
        auto listing = Core::Directory::for_each_entry(String::formatted("/proc/{}/children", tab_shell_pids.get(&current_terminal()).value_or(-1)).release_value_but_fixme_should_propagate_errors(), Core::DirIterator::Flags::SkipParentAndBaseDir, [&](auto&, auto&) {
            ++background_process_count;
            return IterationDecision::Continue;
        });
        if (listing.is_error())
            return 0;
        (void)listing.release_value();
        return background_process_count;
    };

    auto check_terminal_quit = [&]() -> GUI::Dialog::ExecResult {
        if (!should_confirm_close)
            return GUI::MessageBox::ExecResult::OK;
        Optional<String> close_message;
        auto title = "Running Process"sv;
        if (tty_has_foreground_process()) {
            close_message = "Close Terminal and kill its foreground process?"_string;
        } else {
            auto child_process_count = shell_child_process_count();
            if (child_process_count > 1) {
                title = "Running Processes"sv;
                close_message = String::formatted("Close Terminal and kill its {} background processes?", child_process_count).release_value_but_fixme_should_propagate_errors();
            } else if (child_process_count == 1) {
                close_message = "Close Terminal and kill its background process?"_string;
            }
        }
        if (close_message.has_value())
            return GUI::MessageBox::show(window, *close_message, title, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::OKCancel);
        return GUI::MessageBox::ExecResult::OK;
    };

    file_menu->add_action(GUI::CommonActions::make_quit_action(
        [&](auto&) {
            dbgln("Terminal: Quit menu activated!");
            if (check_terminal_quit() == GUI::MessageBox::ExecResult::OK)
                GUI::Application::the()->quit();
        },
        GUI::CommonActions::QuitAltShortcut::None));

    auto edit_menu = window->add_menu("&Edit"_string);
    edit_menu->add_action(current_terminal().copy_action());
    edit_menu->add_action(current_terminal().paste_action());
    edit_menu->add_separator();
    edit_menu->add_action(GUI::Action::create("&Find...", { Mod_Ctrl | Mod_Shift, Key_F }, TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/find.png"sv)),
        [&](auto&) {
            auto& terminal = current_terminal();
            if (auto it = tab_find_windows.find(&terminal); it != tab_find_windows.end()) {
                it->value->show();
                it->value->move_to_front();
            }
        }));

    auto view_menu = window->add_menu("&View"_string);
    view_menu->add_action(GUI::CommonActions::make_fullscreen_action([&](auto&) {
        window->set_fullscreen(!window->is_fullscreen());
    }));
    view_menu->add_action(current_terminal().clear_including_history_action());
    view_menu->add_action(current_terminal().clear_to_previous_mark_action());

    // 键盘切页：终端内容聚焦时，TabWidget 原生的 Ctrl+Tab 收不到事件，这里用窗口级动作转发。
    // 用 Ctrl+PageUp/Down（与 TabWidget 原生 Ctrl+Tab 不冲突，且是 GNOME Terminal 的习惯）。
    view_menu->add_separator();
    view_menu->add_action(GUI::Action::create("Next &Tab", { Mod_Ctrl, Key_PageDown }, [&](auto&) {
        tabs.activate_next_tab();
    }));
    view_menu->add_action(GUI::Action::create("Previous Tab", { Mod_Ctrl, Key_PageUp }, [&](auto&) {
        tabs.activate_previous_tab();
    }));

    auto adjust_font_size = [&](float adjustment, Gfx::Font::AllowInexactSizeMatch preference) {
        auto& font = current_terminal().font();
        auto new_size = max(5, font.presentation_size() + adjustment);
        if (auto new_font = Gfx::FontDatabase::the().get(font.family(), new_size, font.weight(), font.width(), font.slope(), preference)) {
            current_terminal().set_font_and_resize_to_fit(*new_font);
            current_terminal().apply_size_increments_to_window(*window);
            window->resize(current_terminal().size());
        }
    };

    view_menu->add_separator();
    view_menu->add_action(GUI::CommonActions::make_zoom_in_action([&](auto&) {
        adjust_font_size(1, Gfx::Font::AllowInexactSizeMatch::Larger);
    }));
    view_menu->add_action(GUI::CommonActions::make_zoom_out_action([&](auto&) {
        adjust_font_size(-1, Gfx::Font::AllowInexactSizeMatch::Smaller);
    }));

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::CommonActions::make_command_palette_action(window));
    help_menu->add_action(GUI::CommonActions::make_help_action([](auto&) {
        Desktop::Launcher::open(URL::create_with_file_scheme("/usr/share/man/man1/Applications/Terminal.md"), "/bin/Help");
    }));
    help_menu->add_action(GUI::CommonActions::make_about_action("Terminal"_string, app_icon, window));

    window->on_close_request = [&]() -> GUI::Window::CloseRequestDecision {
        if (check_terminal_quit() == GUI::MessageBox::ExecResult::OK)
            return GUI::Window::CloseRequestDecision::Close;
        return GUI::Window::CloseRequestDecision::StayOpen;
    };

    window->on_input_preemption_change = [&](bool is_preempted) {
        current_terminal().set_logical_focus(!is_preempted);
    };

    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil("/bin", "r"));
    TRY(Core::System::unveil("/proc", "r"));
    TRY(Core::System::unveil("/bin/Terminal", "x"));
    TRY(Core::System::unveil("/bin/TerminalSettings", "x"));
    TRY(Core::System::unveil("/bin/utmpupdate", "x"));
    TRY(Core::System::unveil("/etc/FileIconProvider.ini", "r"));
    TRY(Core::System::unveil("/tmp/session/%sid/portal/launch", "rw"));
    TRY(Core::System::unveil("/dev/beep", "rw"));
    TRY(Core::System::unveil(nullptr, nullptr));

    auto modified_state_check_timer = Core::Timer::create_repeating(500, [&] {
        window->set_modified(tty_has_foreground_process() || shell_child_process_count() > 0);
    });

    listener.on_confirm_close_changed = [&](bool confirm_close) {
        if (confirm_close) {
            modified_state_check_timer->start();
        } else {
            modified_state_check_timer->stop();
            window->set_modified(false);
        }
        should_confirm_close = confirm_close;
    };

    window->show();
    // Ensure the terminal (not the TabWidget, which has TabFocus) owns keyboard focus on open,
    // so typing/arrows work immediately without first clicking into the window.
    current_terminal().set_focus(true);
    // 首启主动触发一次几何落盘（此时布局已完成，floating_rect 有效）；
    // 否则 80x25 默认尺寸下没有“尺寸变化”事件，debounce 定时器不会被启动，几何就记不住。
    geometry_save_timer->restart();
    if (should_confirm_close)
        modified_state_check_timer->start();
    int result = app->exec();
    dbgln("Exiting terminal, updating utmp");
    // 多标签下没有单一的 ptsname/shell_pid，utmp 记录在 Linux 上是 no-op。
    (void)utmp_update(StringView {}, 0, false);
    return result;
}
