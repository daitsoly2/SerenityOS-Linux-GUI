/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Mustafa Quraish <mustafa@cs.toronto.edu>
 * Copyright (c) 2022-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DesktopWidget.h"
#include "ExplorerChrome.h"
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Margins.h>
#include <Ports/LinuxGUI/Common/ExplorerEntries.h>
#include <Ports/LinuxGUI/Common/ExplorerMenus.h>
#include "DirectoryView.h"
#include "FileUtils.h"
#include <LibDesktop/DesktopIcons.h>
#include "PropertiesWindow.h"
#include "SearchScope.h"
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Try.h>
#include <Applications/FileManager/FileManagerWindowGML.h>
#include <LibConfig/Client.h>
#include <LibConfig/Listener.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibDesktop/Launcher.h>
#include <LibFileSystem/FileSystem.h>
#include <LibFileSystem/TempFile.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/FileIconProvider.h>
#include <LibGUI/FileSystemModel.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGUI/PathBreadcrumbbar.h>
#include <LibGUI/Progressbar.h>
#include <LibGUI/Splitter.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/Toolbar.h>
#include <LibGUI/ToolbarContainer.h>
#include <LibGUI/TreeView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Palette.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>
#include <Ports/LinuxGUI/Common/ArchiveOps.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace FileManager;

static ErrorOr<int> run_in_desktop_mode();
static ErrorOr<int> run_in_windowed_mode(ByteString const& initial_location, ByteString const& entry_focused_on_init);
static void do_copy(Vector<ByteString> const& selected_file_paths, FileOperation file_operation);
static void do_paste(ByteString const& target_directory, GUI::Window* window);
static void do_create_link(Vector<ByteString> const& selected_file_paths, GUI::Window* window);
static void do_create_archive(Vector<ByteString> const& selected_file_paths, GUI::Window* window);
static void do_set_wallpaper(ByteString const& file_path, GUI::Window* window);
static void do_unzip_archive(Vector<ByteString> const& selected_file_paths, GUI::Window* window);

void show_properties(ByteString const& container_dir_path, ByteString const& path, Vector<ByteString> const& selected, GUI::Window* window);
static bool add_launch_handler_actions_to_menu(RefPtr<GUI::Menu>& menu, DirectoryView const& directory_view, ByteString const& full_path, RefPtr<GUI::Action>& default_action);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio thread recvfd sendfd unix cpath rpath wpath fattr proc exec sigaction"));

    struct sigaction act = {};
    act.sa_flags = SA_NOCLDWAIT;
    act.sa_handler = SIG_IGN;
    TRY(Core::System::sigaction(SIGCHLD, &act, nullptr));

    Core::ArgsParser args_parser;
    bool is_desktop_mode { false };
    bool is_selection_mode { false };
    bool ignore_path_resolution { false };
    ByteString initial_location;
    args_parser.add_option(is_desktop_mode, "Run in desktop mode", "desktop", 'd');
    args_parser.add_option(is_selection_mode, "Show entry in parent folder", "select", 's');
    args_parser.add_option(ignore_path_resolution, "Use raw path, do not resolve real path", "raw", 'r');
    args_parser.add_positional_argument(initial_location, "Path to open", "path", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto app = TRY(GUI::Application::create(arguments));

    TRY(Core::System::pledge("stdio thread recvfd sendfd cpath rpath wpath fattr proc exec unix"));

    Config::pledge_domains({ "FileManager", "WindowManager", "Maps" });
    Config::monitor_domain("FileManager");
    Config::monitor_domain("WindowManager");

    if (is_desktop_mode)
        return run_in_desktop_mode();

    // our initial location is defined as, in order of precedence:
    // 1. the command-line path argument (e.g. FileManager /bin)
    // 2. the current directory
    // 3. the user's home directory
    // 4. the root directory

    LexicalPath path(initial_location);
    if (!initial_location.is_empty()) {
        if (auto error_or_path = FileSystem::real_path(initial_location); !ignore_path_resolution && !error_or_path.is_error())
            initial_location = error_or_path.release_value();

        if (!FileSystem::is_directory(initial_location)) {
            // We want to extract zips to a temporary directory when FileManager is launched with a .zip file as its first argument
            if (path.has_extension(".zip"sv)) {
                auto temp_directory = FileSystem::TempFile::create_temp_directory();
                if (temp_directory.is_error()) {
                    dbgln("Failed to create temporary directory during zip extraction: {}", temp_directory.error());

                    GUI::MessageBox::show_error(app->active_window(), "Failed to create temporary directory!"sv);
                    return -1;
                }

                auto temp_directory_path = temp_directory.value()->path();
                auto result = Core::Process::spawn("/bin/unzip"sv, Array { "-d"sv, temp_directory_path, initial_location });

                if (result.is_error()) {
                    dbgln("Failed to extract {} to {}: {}", initial_location, temp_directory_path, result.error());

                    auto message = TRY(String::formatted("Failed to extract {} to {}", initial_location, temp_directory_path));
                    GUI::MessageBox::show_error(app->active_window(), message);

                    return -1;
                }

                return run_in_windowed_mode(temp_directory_path, path.basename());
            }

            is_selection_mode = true;
        }
    }

    if (auto error_or_cwd = FileSystem::current_working_directory(); initial_location.is_empty() && !error_or_cwd.is_error())
        initial_location = error_or_cwd.release_value();

    if (initial_location.is_empty())
        initial_location = Core::StandardPaths::home_directory();

    if (initial_location.is_empty())
        initial_location = "/";

    ByteString focused_entry;
    if (is_selection_mode) {
        initial_location = path.dirname();
        focused_entry = path.basename();
    }

    return run_in_windowed_mode(initial_location, focused_entry);
}

void do_copy(Vector<ByteString> const& selected_file_paths, FileOperation file_operation)
{
    VERIFY(!selected_file_paths.is_empty());

    StringBuilder copy_text;
    if (file_operation == FileOperation::Move) {
        copy_text.append("#cut\n"sv); // This exploits the comment lines in the text/uri-list specification, which might be a bit hackish
    }
    for (auto& path : selected_file_paths) {
        auto url = URL::create_with_file_scheme(path);
        copy_text.appendff("{}\n", url);
    }
    GUI::Clipboard::the().set_data(copy_text.string_view().bytes(), "text/uri-list");
}

void do_paste(ByteString const& target_directory, GUI::Window* window)
{
    auto data_and_type = GUI::Clipboard::the().fetch_data_and_type();
    if (data_and_type.mime_type != "text/uri-list") {
        dbgln("Cannot paste clipboard type {}", data_and_type.mime_type);
        return;
    }
    auto copied_lines = ByteString::copy(data_and_type.data).split('\n');
    if (copied_lines.is_empty()) {
        dbgln("No files to paste");
        return;
    }

    FileOperation file_operation = FileOperation::Copy;
    if (copied_lines[0] == "#cut") { // cut operation encoded as a text/uri-list comment
        file_operation = FileOperation::Move;
        copied_lines.remove(0);
    }

    Vector<ByteString> source_paths;
    for (auto& uri_as_string : copied_lines) {
        if (uri_as_string.is_empty())
            continue;
        URL::URL url = uri_as_string;
        if (!url.is_valid() || url.scheme() != "file") {
            dbgln("Cannot paste URI {}", uri_as_string);
            continue;
        }
        source_paths.append(URL::percent_decode(url.serialize_path()));
    }

    if (!source_paths.is_empty()) {
        if (auto result = run_file_operation(file_operation, source_paths, target_directory, window); result.is_error())
            dbgln("Failed to paste files: {}", result.error());
    }
}

void do_create_link(Vector<ByteString> const& selected_file_paths, GUI::Window* window)
{
    auto path = selected_file_paths.first();
    auto destination = ByteString::formatted("{}/{}", Core::StandardPaths::desktop_directory(), LexicalPath::basename(path));
    if (auto result = FileSystem::link_file(destination, path); result.is_error()) {
        GUI::MessageBox::show(window, ByteString::formatted("Could not create desktop shortcut:\n{}", result.error()), "File Manager"sv,
            GUI::MessageBox::Type::Error);
    }
}

void do_create_archive(Vector<ByteString> const& selected_file_paths, GUI::Window* window)
{
    // Same tooling as the desktop's Compress menu; the typed name decides the
    // format (.tar.gz -> tar, otherwise zip), so both entry points behave alike.
    String archive_name;
    if (GUI::InputBox::show(window, archive_name, "Enter name:"sv, "Create Archive"sv) != GUI::InputBox::ExecResult::OK)
        return;

    auto name = archive_name.to_byte_string().trim_whitespace();
    bool use_zip = !name.ends_with(".tar.gz"sv, CaseSensitivity::CaseInsensitive);
    Optional<ByteString> stem;
    if (!name.is_empty()) {
        if (use_zip && name.ends_with(".zip"sv, CaseSensitivity::CaseInsensitive))
            name = name.substring(0, name.length() - 4);
        else if (!use_zip && name.ends_with(".tar.gz"sv, CaseSensitivity::CaseInsensitive))
            name = name.substring(0, name.length() - 7);
        stem = name;
    }

    for (auto const& path : selected_file_paths) {
        if (auto error = PortArchiveOps::compress(path, use_zip, stem); !error.is_empty())
            GUI::MessageBox::show_error(window, error);
    }
}

void do_set_wallpaper(ByteString const& file_path, GUI::Window* window)
{
    auto show_error = [&] {
        GUI::MessageBox::show(window, ByteString::formatted("Failed to set {} as wallpaper.", file_path), "Failed to set wallpaper"sv, GUI::MessageBox::Type::Error);
    };

    constexpr auto scale_factor = 1;
    auto bitmap_or_error = Gfx::Bitmap::load_from_file(file_path, scale_factor, GUI::Desktop::the().rect().size());
    if (bitmap_or_error.is_error()) {
        show_error();
        return;
    }

    if (!GUI::Desktop::the().set_wallpaper(bitmap_or_error.release_value(), file_path))
        show_error();
}

static void do_unzip_archive(Vector<ByteString> const& selected_file_paths, GUI::Window* window)
{
    for (auto const& path : selected_file_paths) {
        if (auto error = PortArchiveOps::extract(path); !error.is_empty())
            GUI::MessageBox::show_error(window, error);
    }
}

void show_properties(ByteString const& container_dir_path, ByteString const& path, Vector<ByteString> const& selected, GUI::Window* window)
{
    ErrorOr<RefPtr<PropertiesWindow>> properties_or_error = nullptr;
    if (selected.is_empty()) {
        properties_or_error = window->try_add<PropertiesWindow>(path, true);
    } else {
        properties_or_error = window->try_add<PropertiesWindow>(selected.first(), access(container_dir_path.characters(), W_OK) != 0);
    }

    if (properties_or_error.is_error()) {
        GUI::MessageBox::show(window, "Could not show properties"sv, "Properties Error"sv, GUI::MessageBox::Type::Error);
        return;
    }

    auto properties = properties_or_error.release_value();
    properties->on_close = [properties = properties.ptr()] {
        properties->remove_from_parent();
    };
    properties->center_on_screen();
    properties->show();
}

bool add_launch_handler_actions_to_menu(RefPtr<GUI::Menu>& menu, DirectoryView const& directory_view, ByteString const& full_path, RefPtr<GUI::Action>& default_action)
{
    // Windows executables have no MIME handler in this port, so the handler
    // lookup below yields nothing and "Open" would be missing. Offer the Wine
    // launcher instead, so right-click -> run works like the desktop icon.
    auto lower = full_path.to_lowercase();
    bool windows_binary = false;
    for (auto suffix : { ".exe"sv, ".msi"sv, ".bat"sv, ".cmd"sv }) {
        if (lower.ends_with(suffix)) {
            windows_binary = true;
            break;
        }
    }
    if (windows_binary) {
        auto run_action = GUI::Action::create("用 Wine 运行"sv, [path = full_path](auto&) {
            auto prefix = ::getenv("SERENITY_PREFIX");
            ByteString winapp = ByteString::formatted("{}/bin/serenity-winapp", prefix && *prefix ? prefix : "/serenity");
            Vector<ByteString> arguments;
            arguments.append(path);
            if (auto result = Core::Process::spawn(winapp, ReadonlySpan<ByteString> { arguments.data(), arguments.size() }, LexicalPath::dirname(path)); result.is_error())
                warnln("filemanager: run with wine failed: {}", result.error());
        });
        default_action = run_action;
        menu->add_action(move(run_action));
        return true;
    }

    auto current_file_launch_handlers = directory_view.get_launch_handlers(full_path);

    bool added_open_menu_items = false;
    auto default_file_handler = directory_view.get_default_launch_handler(current_file_launch_handlers);
    if (default_file_handler) {
        auto file_open_action = default_file_handler->create_launch_action([&, full_path = move(full_path)](auto& launcher_handler) {
            directory_view.launch(URL::create_with_file_scheme(full_path), launcher_handler);
        });
        if (default_file_handler->details().launcher_type == Desktop::Launcher::LauncherType::Application)
            file_open_action->set_text(ByteString::formatted("Run {}", file_open_action->text()));
        else
            file_open_action->set_text(ByteString::formatted("Open in {}", file_open_action->text()));

        default_action = file_open_action;

        menu->add_action(move(file_open_action));
        added_open_menu_items = true;
    } else {
        default_action.clear();
    }

    if (current_file_launch_handlers.size() > 1) {
        added_open_menu_items = true;
        auto file_open_with_menu = menu->add_submenu("Open with"_string);
        for (auto& handler : current_file_launch_handlers) {
            if (handler == default_file_handler)
                continue;
            file_open_with_menu->add_action(handler->create_launch_action([&, full_path = move(full_path)](auto& launcher_handler) {
                directory_view.launch(URL::create_with_file_scheme(full_path), launcher_handler);
            }));
        }
    }

    return added_open_menu_items;
}

ErrorOr<int> run_in_desktop_mode()
{
    (void)Core::Process::set_name("FileManager (Desktop)"sv, Core::Process::SetThreadName::Yes);

    auto window = GUI::Window::construct();
    window->set_title("Desktop Manager");
    window->set_window_type(GUI::WindowType::Desktop);
    window->set_has_alpha_channel(true);

    auto desktop_icon = Desktop::icon("desktop"sv);
    window->set_icon(desktop_icon);

    auto desktop_widget = window->set_main_widget<FileManager::DesktopWidget>();
    desktop_widget->set_layout<GUI::VerticalBoxLayout>();

    auto directory_view = TRY(desktop_widget->try_add<DirectoryView>(DirectoryView::Mode::Desktop));
    directory_view->set_name("directory_view");

    auto cut_action = GUI::CommonActions::make_cut_action(
        [&](auto&) {
            auto paths = directory_view->selected_file_paths();
            VERIFY(!paths.is_empty());

            do_copy(paths, FileOperation::Move);
        },
        window);
    cut_action->set_enabled(false);

    auto copy_action = GUI::CommonActions::make_copy_action(
        [&](auto&) {
            auto paths = directory_view->selected_file_paths();
            VERIFY(!paths.is_empty());

            do_copy(paths, FileOperation::Copy);
        },
        window);
    copy_action->set_enabled(false);

    auto create_archive_action
        = GUI::Action::create(
            "Create &Archive",
            Desktop::icon("filetype-archive"sv),
            [&](GUI::Action const&) {
                auto paths = directory_view->selected_file_paths();
                if (paths.is_empty())
                    return;

                do_create_archive(paths, directory_view->window());
            },
            window);

    auto unzip_archive_action
        = GUI::Action::create(
            "E&xtract Here",
            [&](GUI::Action const&) {
                auto paths = directory_view->selected_file_paths();
                if (paths.is_empty())
                    return;

                do_unzip_archive(paths, directory_view->window());
            },
            window);

    auto set_wallpaper_action
        = GUI::Action::create(
            "Set as Desktop &Wallpaper",
            Desktop::icon("app-display-settings"sv),
            [&](GUI::Action const&) {
                auto paths = directory_view->selected_file_paths();
                if (paths.is_empty())
                    return;

                do_set_wallpaper(paths.first(), directory_view->window());
            },
            window);

    directory_view->on_selection_change = [&](GUI::AbstractView const& view) {
        cut_action->set_enabled(!view.selection().is_empty());
        copy_action->set_enabled(!view.selection().is_empty());
    };

    auto properties_action = GUI::CommonActions::make_properties_action(
        [&](auto&) {
            ByteString path = directory_view->path();
            Vector<ByteString> selected = directory_view->selected_file_paths();

            show_properties(path, path, selected, directory_view->window());
        },
        window);

    auto paste_action = GUI::CommonActions::make_paste_action(
        [&](GUI::Action const&) {
            do_paste(directory_view->path(), directory_view->window());
        },
        window);
    paste_action->set_enabled(GUI::Clipboard::the().fetch_mime_type() == "text/uri-list" && access(directory_view->path().characters(), W_OK) == 0);

    GUI::Clipboard::the().on_change = [&](ByteString const& data_type) {
        paste_action->set_enabled(data_type == "text/uri-list" && access(directory_view->path().characters(), W_OK) == 0);
    };

    auto desktop_view_context_menu = GUI::Menu::construct("Directory View"_string);

    auto file_manager_action = GUI::Action::create("Open in File &Manager", {}, Desktop::icon("app-file-manager"sv), [&](auto&) {
        auto paths = directory_view->selected_file_paths();
        if (paths.is_empty()) {
            Desktop::Launcher::open(URL::create_with_file_scheme(directory_view->path()));
            return;
        }

        for (auto& path : paths) {
            if (FileSystem::is_directory(path))
                Desktop::Launcher::open(URL::create_with_file_scheme(path));
        }
    });

    auto open_terminal_action = GUI::Action::create("Open in &Terminal", {}, Desktop::icon("app-terminal"sv), [&](auto&) {
        auto paths = directory_view->selected_file_paths();
        if (paths.is_empty()) {
            spawn_terminal(window, directory_view->path());
            return;
        }

        for (auto& path : paths) {
            if (FileSystem::is_directory(path)) {
                spawn_terminal(window, path);
            }
        }
    });

    auto display_properties_action = GUI::Action::create("&Display Settings", {}, Desktop::icon("app-display-settings"sv), [&](GUI::Action const&) {
        Desktop::Launcher::open(URL::create_with_file_scheme("/bin/DisplaySettings"));
    });

    desktop_view_context_menu->add_action(directory_view->mkdir_action());
    desktop_view_context_menu->add_action(directory_view->touch_action());
    desktop_view_context_menu->add_action(paste_action);
    desktop_view_context_menu->add_separator();
    desktop_view_context_menu->add_action(file_manager_action);
    desktop_view_context_menu->add_action(open_terminal_action);
    desktop_view_context_menu->add_separator();
    desktop_view_context_menu->add_action(display_properties_action);

    auto desktop_context_menu = GUI::Menu::construct("Directory View Directory"_string);

    desktop_context_menu->add_action(file_manager_action);
    desktop_context_menu->add_action(open_terminal_action);
    desktop_context_menu->add_separator();
    desktop_context_menu->add_action(cut_action);
    desktop_context_menu->add_action(copy_action);
    desktop_context_menu->add_action(paste_action);
    desktop_context_menu->add_action(directory_view->delete_action());
    desktop_context_menu->add_action(directory_view->rename_action());
    desktop_context_menu->add_separator();
    desktop_context_menu->add_action(properties_action);

    RefPtr<GUI::Menu> file_context_menu;
    RefPtr<GUI::Action> file_context_menu_action_default_action;

    directory_view->on_context_menu_request = [&](GUI::ModelIndex const& index, GUI::ContextMenuEvent const& event) {
        if (index.is_valid()) {
            auto& node = directory_view->node(index);
            if (node.is_directory()) {
                desktop_context_menu->popup(event.screen_position(), file_manager_action);
            } else {
                file_context_menu = GUI::Menu::construct("Directory View File"_string);

                bool added_open_menu_items = add_launch_handler_actions_to_menu(file_context_menu, directory_view, node.full_path(), file_context_menu_action_default_action);
                if (added_open_menu_items)
                    file_context_menu->add_separator();

                file_context_menu->add_action(cut_action);
                file_context_menu->add_action(copy_action);
                file_context_menu->add_action(paste_action);
                file_context_menu->add_action(directory_view->delete_action());
                file_context_menu->add_action(directory_view->rename_action());
                file_context_menu->add_action(create_archive_action);
                file_context_menu->add_separator();

                if (Gfx::Bitmap::is_path_a_supported_image_format(node.name)) {
                    file_context_menu->add_action(set_wallpaper_action);
                    file_context_menu->add_separator();
                }

                if (PortArchiveOps::is_archive(node.full_path())) {
                    file_context_menu->add_action(unzip_archive_action);
                    file_context_menu->add_separator();
                }

                file_context_menu->add_action(properties_action);
                file_context_menu->popup(event.screen_position(), file_context_menu_action_default_action);
            }
        } else {
            desktop_view_context_menu->popup(event.screen_position());
        }
    };
    struct BackgroundWallpaperListener : Config::Listener {
        virtual void config_string_did_change(StringView domain, StringView group, StringView key, StringView value) override
        {
            if (domain == "WindowManager" && group == "Background" && key == "Wallpaper")
                GUI::Desktop::the().apply_wallpaper(nullptr, value);
        }
    } wallpaper_listener;

    // This sets the wallpaper at startup, even if there is no wallpaper, the
    // desktop should still show the background color. It's fine to pass a
    // nullptr to Desktop::set_wallpaper.
    GUI::Desktop::the().load_current_wallpaper();

    // Update wallpaper if desktop resolution changes.
    GUI::Desktop::the().on_receive_screen_rects([&](auto&) {
        GUI::Desktop::the().load_current_wallpaper();
    });

    window->show();
    return GUI::Application::the()->exec();
}

ErrorOr<int> run_in_windowed_mode(ByteString const& initial_location, ByteString const& entry_focused_on_init)
{
    auto window = GUI::Window::construct();
    window->set_title("文件管理器");

    auto widget = window->set_main_widget<GUI::Widget>();
    TRY(widget->load_from_gml(file_manager_window_gml));

    auto& command_bar_host = *widget->find_descendant_of_type_named<GUI::Widget>("command_bar_host");
    auto& address_bar_host = *widget->find_descendant_of_type_named<GUI::Widget>("address_bar_host");
    auto& content_host = *widget->find_descendant_of_type_named<GUI::Widget>("content_host");
    auto& statusbar_host = *widget->find_descendant_of_type_named<GUI::Widget>("statusbar_host");

    command_bar_host.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
    auto command_bar = TRY(command_bar_host.try_add<ExplorerCommandBar>());

    address_bar_host.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
    auto address_bar = TRY(address_bar_host.try_add<ExplorerAddressBar>());
    // Take the full host width; otherwise the bar only claims its preferred
    // width and leaves a gap to the right of the search box.
    address_bar->set_preferred_width(GUI::SpecialDimension::Grow);
    auto breadcrumbbar = TRY(address_bar->breadcrumb_host().try_add<GUI::PathBreadcrumbbar>());

    content_host.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
    auto navigation_pane = TRY(content_host.try_add<ExplorerNavigationPane>());
    navigation_pane->set_fill_with_background_color(true);
    navigation_pane->set_fixed_width(178);

    auto directory_view = TRY(content_host.try_add<DirectoryView>(DirectoryView::Mode::Normal));
    directory_view->set_name("directory_view");
    directory_view->set_preferred_width(GUI::SpecialDimension::Grow);

    auto preview_pane = TRY(content_host.try_add<ExplorerPreviewPane>());
    preview_pane->set_fill_with_background_color(true);
    preview_pane->set_fixed_width(190);
    preview_pane->set_visible(false);

    statusbar_host.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
    auto statusbar = TRY(statusbar_host.try_add<ExplorerStatusbar>());

    GUI::Application::the()->on_action_enter = [statusbar](GUI::Action& action) {
        statusbar->set_override_text(action.status_tip());
    };
    GUI::Application::the()->on_action_leave = [statusbar](GUI::Action&) {
        statusbar->set_override_text({});
    };

    auto& progressbar = statusbar->progressbar();
    progressbar.set_format(GUI::Progressbar::Format::ValueSlashMax);
    progressbar.set_frame_style(Gfx::FrameStyle::SunkenPanel);

    // --- Layout state -----------------------------------------------------
    bool show_hidden_files = Config::read_bool("FileManager"sv, "DirectoryView"sv, "ShowDotFiles"sv, false);
    show_hidden_files |= initial_location.contains("/."sv);
    bool show_navigation_pane = Config::read_bool("FileManager"sv, "Layout"sv, "ShowNavigationPane"sv, true);
    bool show_preview_pane = Config::read_bool("FileManager"sv, "Layout"sv, "ShowPreviewPane"sv, false);
    bool show_statusbar = Config::read_bool("FileManager"sv, "Layout"sv, "ShowStatusbar"sv, true);
    bool show_command_bar = Config::read_bool("FileManager"sv, "Layout"sv, "ShowCommandBar"sv, true);

    auto apply_hidden_files = [&](bool show) {
        show_hidden_files = show;
        directory_view->set_should_show_dotfiles(show);
        directory_view->refresh();
        Config::write_bool("FileManager"sv, "DirectoryView"sv, "ShowDotFiles"sv, show);
    };
    auto apply_navigation_pane = [&](bool show) {
        show_navigation_pane = show;
        navigation_pane->set_visible(show);
        Config::write_bool("FileManager"sv, "Layout"sv, "ShowNavigationPane"sv, show);
    };
    auto apply_preview_pane = [&](bool show) {
        show_preview_pane = show;
        preview_pane->set_visible(show);
        statusbar->view_switcher().set_preview_visible(show);
        Config::write_bool("FileManager"sv, "Layout"sv, "ShowPreviewPane"sv, show);
    };
    auto apply_statusbar = [&](bool show) {
        show_statusbar = show;
        statusbar_host.set_visible(show);
        Config::write_bool("FileManager"sv, "Layout"sv, "ShowStatusbar"sv, show);
    };
    auto apply_command_bar = [&](bool show) {
        show_command_bar = show;
        command_bar_host.set_visible(show);
        Config::write_bool("FileManager"sv, "Layout"sv, "ShowCommandBar"sv, show);
    };

    apply_hidden_files(show_hidden_files);
    apply_navigation_pane(show_navigation_pane);
    apply_preview_pane(show_preview_pane);
    apply_statusbar(show_statusbar);
    apply_command_bar(show_command_bar);

    // --- Shared helpers ---------------------------------------------------
    auto selected_paths = [directory_view]() {
        return directory_view->selected_file_paths();
    };
    auto can_paste_here = [&]() {
        return GUI::Clipboard::the().fetch_mime_type() == "text/uri-list"
            && access(directory_view->path().characters(), W_OK) == 0;
    };
    auto compress_paths = [window](Vector<ByteString> const& paths, bool use_zip) {
        for (auto const& path : paths) {
            if (auto error = PortArchiveOps::compress(path, use_zip); !error.is_empty())
                GUI::MessageBox::show_error(window, error);
        }
    };

    // --- Search scope -----------------------------------------------------
    // Vista's search box is opt-in (the filter proxy is built lazily) and
    // bounded: only the user's home directory is searchable, plus locations
    // the user explicitly added. Nothing outside those roots is ever walked.
    auto nav_icon = [](StringView name) -> RefPtr<Gfx::Bitmap const> {
        auto bitmap = Gfx::Bitmap::load_from_uri(ByteString::formatted("resource://icons/16x16/{}", name));
        if (bitmap.is_error())
            return nullptr;
        return bitmap.release_value();
    };

    auto rebuild_navigation_pane = [&]() {
        navigation_pane->clear();

        auto favorites = navigation_pane->add_group("收藏夹"sv);
        navigation_pane->add_item(favorites, "桌面"sv, Core::StandardPaths::desktop_directory(), nav_icon("desktop.png"sv));
        navigation_pane->add_item(favorites, "下载"sv, Core::StandardPaths::downloads_directory(), nav_icon("filetype-folder.png"sv));

        auto libraries = navigation_pane->add_group("库"sv);
        navigation_pane->add_item(libraries, "文档"sv, Core::StandardPaths::documents_directory(), nav_icon("filetype-folder.png"sv));
        navigation_pane->add_item(libraries, "图片"sv, Core::StandardPaths::pictures_directory(), nav_icon("filetype-folder.png"sv));
        navigation_pane->add_item(libraries, "音乐"sv, Core::StandardPaths::music_directory(), nav_icon("filetype-folder.png"sv));
        navigation_pane->add_item(libraries, "视频"sv, Core::StandardPaths::videos_directory(), nav_icon("filetype-folder.png"sv));

        auto computer = navigation_pane->add_group("计算机"sv);
        navigation_pane->add_item(computer, "本地磁盘 (C:)"sv, "/"sv, nav_icon("hard-disk.png"sv));
        navigation_pane->add_item(computer, "用户"sv, SearchScope::the().home_directory(), nav_icon("home-directory.png"sv));

        // Win7 "Libraries" equivalent: locations the user opted into search.
        auto scope_group = navigation_pane->add_group("搜索位置"sv);
        auto const& extra = SearchScope::the().additional_directories();
        if (extra.is_empty()) {
            navigation_pane->add_item(scope_group, "仅用户目录"sv,
                SearchScope::the().home_directory(), nav_icon("home-directory.png"sv));
        } else {
            for (auto const& path : extra) {
                auto title = LexicalPath::basename(path);
                if (title.is_empty())
                    title = path; // e.g. "/" has no basename
                navigation_pane->add_item(scope_group, title, path, nav_icon("filetype-folder.png"sv));
            }
        }

        navigation_pane->set_current_path(directory_view->path());
    };

    auto refresh_search_scope = [&]() {
        bool searchable = SearchScope::the().contains(directory_view->path());
        if (searchable) {
            address_bar->set_search_placeholder(LexicalPath::basename(directory_view->path()));
            statusbar->set_override_text({});
        } else {
            // AllowCallback::No: clearing the box must not re-enter the filter.
            address_bar->search_box().set_text(""sv, GUI::AllowCallback::No);
            directory_view->set_filter_term({});
            address_bar->search_box().set_placeholder("仅可搜索用户目录与已添加的目录"sv);
            statusbar->set_override_text(MUST(String::from_utf8("当前目录不在搜索范围内，请用「搜索范围」添加"sv)));
        }
        address_bar->search_box().set_enabled(searchable);
    };

    auto apply_search_term = [&]() {
        auto term = address_bar->search_box().text();
        if (!term.is_empty() && !SearchScope::the().contains(directory_view->path())) {
            directory_view->set_filter_term({});
            statusbar->set_override_text(MUST(String::from_utf8("仅可搜索用户目录与已添加的目录"sv)));
            return;
        }
        statusbar->set_override_text({});
        directory_view->set_filter_term(term);
    };

    // --- Actions ----------------------------------------------------------
    auto open_parent_directory_action = GUI::Action::create("上一级(&U)"sv, { Mod_Alt, Key_Up },
        Desktop::icon("open-parent-directory"sv),
        [directory_view](auto&) { directory_view->open_parent_directory(); }, window);

    auto open_child_directory_action = GUI::Action::create("下一级(&D)"sv, { Mod_Alt, Key_Down },
        [breadcrumbbar](auto&) { breadcrumbbar->select_child_segment(); }, window);

    auto new_window_action = GUI::Action::create("新建窗口(&N)"sv, { Mod_Ctrl, Key_N },
        Desktop::icon("new-window"sv),
        [directory_view](auto&) { Desktop::Launcher::open(URL::create_with_file_scheme(directory_view->path())); }, window);

    auto select_all_action = GUI::CommonActions::make_select_all_action([directory_view](auto&) {
        directory_view->current_view().select_all();
    });

    auto go_back_action = GUI::CommonActions::make_go_back_action([directory_view](auto&) {
        directory_view->open_previous_directory();
    }, window);
    auto go_forward_action = GUI::CommonActions::make_go_forward_action([directory_view](auto&) {
        directory_view->open_next_directory();
    }, window);
    auto go_home_action = GUI::CommonActions::make_go_home_action([directory_view](auto&) {
        directory_view->open(Core::StandardPaths::home_directory());
    }, window);
    auto go_to_location_action = GUI::Action::create("转到位置(&L)..."sv, { Mod_Ctrl, Key_L }, Key_F6,
        Desktop::icon("go-to"sv),
        [breadcrumbbar](auto&) { breadcrumbbar->show_location_text_box(); }, window);
    auto search_action = GUI::Action::create("搜索(&F)"sv, { Mod_Ctrl, Key_F },
        [address_bar, directory_view, statusbar](auto&) {
            if (!SearchScope::the().contains(directory_view->path())) {
                statusbar->set_override_text(MUST(String::from_utf8("当前目录不在搜索范围内，请用「搜索范围」添加"sv)));
                return;
            }
            address_bar->search_box().set_focus(true);
        }, window);

    auto cut_action = GUI::CommonActions::make_cut_action([&](auto&) {
        auto paths = selected_paths();
        if (!paths.is_empty())
            do_copy(paths, FileOperation::Move);
    }, window);
    cut_action->set_enabled(false);

    auto copy_action = GUI::CommonActions::make_copy_action([&](auto&) {
        auto paths = selected_paths();
        if (!paths.is_empty())
            do_copy(paths, FileOperation::Copy);
    }, window);
    copy_action->set_enabled(false);

    auto copy_path_action = GUI::Action::create("复制路径(&P)"sv, [&](auto&) {
        auto paths = selected_paths();
        if (paths.is_empty())
            return;
        StringBuilder builder;
        builder.join('\n', paths.span());
        GUI::Clipboard::the().set_plain_text(builder.string_view());
    }, window);

    auto paste_action = GUI::CommonActions::make_paste_action([&](auto&) {
        do_paste(directory_view->path(), window);
    }, window);
    paste_action->set_enabled(false);

    auto delete_action = GUI::CommonActions::make_delete_action([&](auto&) {
        auto paths = selected_paths();
        if (!paths.is_empty())
            delete_paths(paths, true, window);
    }, window);
    delete_action->set_enabled(false);

    auto properties_action = GUI::CommonActions::make_properties_action([&](auto&) {
        show_properties(directory_view->path(), directory_view->path(), selected_paths(), window);
    }, window);

    auto show_hidden_files_action = GUI::Action::create_checkable("显示隐藏文件(&H)"sv, { Mod_Ctrl, Key_H },
        [&](auto& action) { apply_hidden_files(action.is_checked()); }, window);
    show_hidden_files_action->set_checked(show_hidden_files);

    auto navigation_pane_action = GUI::Action::create_checkable("导航窗格(&N)"sv,
        [&](auto& action) { apply_navigation_pane(action.is_checked()); }, window);
    navigation_pane_action->set_checked(show_navigation_pane);

    auto preview_pane_action = GUI::Action::create_checkable("预览窗格(&P)"sv,
        [&](auto& action) { apply_preview_pane(action.is_checked()); }, window);
    preview_pane_action->set_checked(show_preview_pane);

    auto statusbar_action = GUI::Action::create_checkable("状态栏(&S)"sv,
        [&](auto& action) { apply_statusbar(action.is_checked()); }, window);
    statusbar_action->set_checked(show_statusbar);

    auto command_bar_action = GUI::Action::create_checkable("命令栏(&C)"sv,
        [&](auto& action) { apply_command_bar(action.is_checked()); }, window);
    command_bar_action->set_checked(show_command_bar);

    auto view_type_action_group = make<GUI::ActionGroup>();
    view_type_action_group->set_exclusive(true);
    view_type_action_group->add_action(directory_view->view_as_icons_action());
    view_type_action_group->add_action(directory_view->view_as_table_action());
    view_type_action_group->add_action(directory_view->view_as_columns_action());

    GUI::Clipboard::the().on_change = [&](ByteString const& data_type) {
        paste_action->set_enabled(data_type == "text/uri-list"
            && access(directory_view->path().characters(), W_OK) == 0);
    };

    // --- Folder options (Vista-style small dialog) ------------------------
    auto open_folder_options = [&]() {
        auto dialog = GUI::Window::construct();
        GUI::Window* dialog_ptr = dialog.ptr();
        dialog->set_title("文件夹选项");
        dialog->resize(Gfx::IntSize { 320, 210 });
        dialog->center_on_screen();

        auto root = dialog->set_main_widget<GUI::Widget>();
        root->set_fill_with_background_color(true);
        root->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12 }, 6);

        auto make_check = [&](StringView label, bool checked) -> GUI::CheckBox& {
            auto& box = root->add<GUI::CheckBox>(MUST(String::from_utf8(label)));
            box.set_fixed_height(22);
            box.set_checked(checked);
            return box;
        };
        auto& hidden_box = make_check("显示隐藏的文件和文件夹"sv, show_hidden_files);
        auto& nav_box = make_check("显示导航窗格"sv, show_navigation_pane);
        auto& preview_box = make_check("显示预览窗格"sv, show_preview_pane);
        auto& status_box = make_check("显示状态栏"sv, show_statusbar);

        auto& spacer = root->add<GUI::Widget>();
        spacer.set_preferred_height(GUI::SpecialDimension::Grow);

        auto& button_row = root->add<GUI::Widget>();
        button_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
        button_row.set_fixed_height(28);
        auto& button_spacer = button_row.add<GUI::Widget>();
        button_spacer.set_preferred_width(GUI::SpecialDimension::Grow);
        auto& ok_button = button_row.add<GUI::Button>(MUST(String::from_utf8("确定"sv)));
        ok_button.set_fixed_width(80);
        ok_button.on_click = [&](unsigned) {
            show_hidden_files_action->set_checked(hidden_box.is_checked());
            apply_hidden_files(hidden_box.is_checked());
            navigation_pane_action->set_checked(nav_box.is_checked());
            apply_navigation_pane(nav_box.is_checked());
            preview_pane_action->set_checked(preview_box.is_checked());
            apply_preview_pane(preview_box.is_checked());
            statusbar_action->set_checked(status_box.is_checked());
            apply_statusbar(status_box.is_checked());
            dialog_ptr->close();
        };
        auto& cancel_button = button_row.add<GUI::Button>(MUST(String::from_utf8("取消"sv)));
        cancel_button.set_fixed_width(80);
        cancel_button.on_click = [dialog_ptr](unsigned) { dialog_ptr->close(); };

        dialog->show();
    };

    // --- Menus ------------------------------------------------------------
    auto file_menu = window->add_menu("文件(&F)"_string);
    file_menu->add_action(new_window_action);
    file_menu->add_action(directory_view->mkdir_action());
    file_menu->add_action(directory_view->touch_action());
    file_menu->add_separator();
    file_menu->add_action(delete_action);
    file_menu->add_action(directory_view->rename_action());
    file_menu->add_separator();
    file_menu->add_action(properties_action);
    file_menu->add_separator();
    file_menu->add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit();
    }));

    auto edit_menu = window->add_menu("编辑(&E)"_string);
    edit_menu->add_action(cut_action);
    edit_menu->add_action(copy_action);
    edit_menu->add_action(paste_action);
    edit_menu->add_action(copy_path_action);
    edit_menu->add_separator();
    edit_menu->add_action(select_all_action);

    auto view_menu = window->add_menu("查看(&V)"_string);
    auto layout_menu = view_menu->add_submenu("布局(&L)"_string);
    layout_menu->add_action(command_bar_action);
    layout_menu->add_action(navigation_pane_action);
    layout_menu->add_action(preview_pane_action);
    layout_menu->add_action(statusbar_action);
    view_menu->add_separator();
    view_menu->add_action(directory_view->view_as_icons_action());
    view_menu->add_action(directory_view->view_as_table_action());
    view_menu->add_action(directory_view->view_as_columns_action());
    view_menu->add_separator();
    view_menu->add_action(show_hidden_files_action);
    view_menu->add_action(GUI::Action::create("文件夹选项(&O)..."sv, [open_folder_options](auto&) {
        open_folder_options();
    }, window));
    view_menu->add_separator();
    view_menu->add_action(GUI::CommonActions::make_fullscreen_action([window](auto&) {
        window->set_fullscreen(!window->is_fullscreen());
    }));

    auto go_menu = window->add_menu("转到(&G)"_string);
    go_menu->add_action(go_back_action);
    go_menu->add_action(go_forward_action);
    go_menu->add_action(open_parent_directory_action);
    go_menu->add_action(open_child_directory_action);
    go_menu->add_action(go_home_action);
    go_menu->add_action(go_to_location_action);
    go_menu->add_action(search_action);
    go_menu->add_separator();
    go_menu->add_action(directory_view->open_terminal_action());

    auto help_menu = window->add_menu("帮助(&H)"_string);
    help_menu->add_action(GUI::CommonActions::make_command_palette_action(window));
    help_menu->add_action(GUI::CommonActions::make_about_action("文件管理器"_string,
        GUI::Icon(Desktop::app_logo("app-file-manager"sv)), window));

    // --- Command bar ------------------------------------------------------
    auto organize_button = &command_bar->add_command("组织"sv,
        Desktop::icon("properties"sv), true);
    auto organize_menu = GUI::Menu::construct();
    organize_menu->add_action(cut_action);
    organize_menu->add_action(copy_action);
    organize_menu->add_action(paste_action);
    organize_menu->add_action(delete_action);
    organize_menu->add_action(directory_view->rename_action());
    organize_menu->add_separator();
    organize_menu->add_action(select_all_action);
    organize_menu->add_separator();
    organize_menu->add_action(GUI::Action::create("文件夹选项"sv, [open_folder_options](auto&) {
        open_folder_options();
    }, window));
    organize_button->on_dropdown_click = [organize_menu](Gfx::IntPoint position) {
        organize_menu->popup(position);
    };
    organize_button->on_click = [organize_menu, organize_button]() {
        auto where = organize_button->screen_relative_rect().location();
        organize_menu->popup(where.translated(0, organize_button->height()));
    };

    auto share_button = &command_bar->add_command("共享"sv,
        Desktop::icon("filetype-archive"sv), true);
    auto share_menu = GUI::Menu::construct();
    share_menu->add_action(GUI::Action::create("压缩为 ZIP 压缩包"sv, [&](auto&) {
        auto paths = selected_paths();
        if (!paths.is_empty())
            compress_paths(paths, true);
    }, window));
    share_menu->add_action(GUI::Action::create("压缩为 Tar.gz 压缩包"sv, [&](auto&) {
        auto paths = selected_paths();
        if (!paths.is_empty())
            compress_paths(paths, false);
    }, window));
    share_menu->add_action(directory_view->open_terminal_action());
    share_button->on_dropdown_click = [share_menu](Gfx::IntPoint position) {
        share_menu->popup(position);
    };

    command_bar->add_separator();
    command_bar->add_command("新建文件夹"sv,
        Desktop::icon("mkdir"sv)).on_click = [directory_view]() {
        directory_view->mkdir_action().activate();
    };
    command_bar->add_command("新建文本文档"sv,
        Desktop::icon("new"sv)).on_click = [directory_view]() {
        directory_view->touch_action().activate();
    };
    auto preview_button = &command_bar->add_command("预览窗格"sv,
        Desktop::icon("app-image-viewer"sv));
    preview_button->set_checked(show_preview_pane);
    preview_button->on_click = [&]() {
        bool next = !show_preview_pane;
        preview_pane_action->set_checked(next);
        apply_preview_pane(next);
        preview_button->set_checked(next);
    };
    command_bar->add_separator();
    command_bar->add_command("在终端中打开"sv,
        Desktop::icon("app-terminal"sv)).on_click = [&]() {
        spawn_terminal(window, directory_view->path());
    };

    // Vista "Libraries / indexed locations" equivalent: the user opts extra
    // directories into the search scope. Rebuilt on every popup so removals
    // show up immediately.
    auto search_scope_button = &command_bar->add_command("搜索范围"sv,
        Desktop::icon("find"sv), true);
    // Keep the last search-scope menu alive: GUI::Menu::popup() is async, so the
    // menu must outlive the dropdown handler or it never appears on screen.
    RefPtr<GUI::Menu> search_scope_menu;
    auto make_search_scope_menu = [&]() {
        auto menu = GUI::Menu::construct();
        auto add_action = GUI::Action::create(
            ByteString::formatted("将当前文件夹加入搜索范围：{}", directory_view->path()),
            [&](auto&) {
                if (SearchScope::the().add(directory_view->path())) {
                    rebuild_navigation_pane();
                    refresh_search_scope();
                }
            }, window);
        add_action->set_enabled(!SearchScope::the().contains(directory_view->path()));
        menu->add_action(add_action);
        menu->add_separator();
        auto const& extra = SearchScope::the().additional_directories();
        if (extra.is_empty()) {
            auto placeholder_action = GUI::Action::create("（仅用户目录）"sv, [](auto&) {}, window);
            placeholder_action->set_enabled(false);
            menu->add_action(placeholder_action);
        } else {
            for (auto const& path : extra) {
                auto root = path;
                menu->add_action(GUI::Action::create(
                    ByteString::formatted("移除  {}", root),
                    [&, root](auto&) {
                        SearchScope::the().remove(root);
                        rebuild_navigation_pane();
                        refresh_search_scope();
                    }, window));
            }
        }
        search_scope_menu = menu;
        return menu;
    };
    search_scope_button->on_dropdown_click = [make_search_scope_menu](Gfx::IntPoint position) {
        auto menu = make_search_scope_menu();
        menu->popup(position);
    };
    search_scope_button->on_click = [make_search_scope_menu, search_scope_button]() {
        auto where = search_scope_button->screen_relative_rect().location();
        auto menu = make_search_scope_menu();
        menu->popup(where.translated(0, search_scope_button->height()));
    };

    command_bar->add_stretch();
    command_bar->add_command("帮助"sv,
        Desktop::icon("app-help"sv)).on_click = [window]() {
        GUI::MessageBox::show(window,
            "后退/前进/向上、面包屑；搜索框仅在用户目录与「搜索范围」中添加的目录内可用，输入时才启用过滤。右键菜单与桌面完全一致。"sv,
            "文件管理器"sv, GUI::MessageBox::Type::Information);
    };

    // --- Address bar ------------------------------------------------------
    address_bar->back_button().on_click = [go_back_action]() { go_back_action->activate(); };
    address_bar->forward_button().on_click = [go_forward_action]() { go_forward_action->activate(); };
    address_bar->up_button().on_click = [open_parent_directory_action]() { open_parent_directory_action->activate(); };
    address_bar->refresh_button().on_click = [directory_view]() { directory_view->refresh(); };

    address_bar->search_box().on_change = [apply_search_term]() { apply_search_term(); };

    breadcrumbbar->on_path_change = [&](auto selected_path) {
        if (FileSystem::is_directory(selected_path))
            directory_view->open(selected_path);
        else
            breadcrumbbar->set_current_path(directory_view->path());
    };
    breadcrumbbar->on_paths_drop = [&](auto path, GUI::DropEvent const& event) {
        handle_drop(event, path, window).release_value_but_fixme_should_propagate_errors();
    };

    // --- Navigation pane --------------------------------------------------
    rebuild_navigation_pane();
    navigation_pane->on_navigate = [directory_view](ByteString const& path) {
        if (FileSystem::is_directory(path))
            directory_view->open(path);
    };

    // --- View switcher / statusbar ---------------------------------------
    statusbar->view_switcher().on_view_change = [&](ExplorerViewSwitcher::Mode mode) {
        switch (mode) {
        case ExplorerViewSwitcher::Details:
            directory_view->view_as_table_action().activate();
            break;
        case ExplorerViewSwitcher::Icons:
            directory_view->view_as_icons_action().activate();
            break;
        case ExplorerViewSwitcher::Tiles:
            directory_view->view_as_columns_action().activate();
            break;
        }
    };
    statusbar->view_switcher().on_preview_toggle = [&](bool visible) {
        preview_pane_action->set_checked(visible);
        apply_preview_pane(visible);
        preview_button->set_checked(visible);
    };

    // --- Directory view callbacks ----------------------------------------
    directory_view->on_path_change = [&](ByteString const& new_path, bool can_read_in_path, bool can_write_in_path) {
        auto icon = GUI::FileIconProvider::icon_for_path(new_path);
        window->set_icon(icon.bitmap_for_size(16));
        // Vista/Win7 Explorer shows just the current folder name in the title
        // bar (the address bar already carries the full path).
        auto folder_name = LexicalPath::basename(new_path);
        window->set_title(folder_name.is_empty()
            ? ByteString { "文件管理器"sv }
            : ByteString::formatted("{} - 文件管理器", folder_name));

        breadcrumbbar->set_current_path(new_path);
        navigation_pane->set_current_path(new_path);
        refresh_search_scope();
        address_bar->set_navigation_state(directory_view->path_history_position() > 0,
            directory_view->path_history_position() < directory_view->path_history_size() - 1);

        paste_action->set_enabled(can_write_in_path && GUI::Clipboard::the().fetch_mime_type() == "text/uri-list");
        go_forward_action->set_enabled(directory_view->path_history_position() < directory_view->path_history_size() - 1);
        go_back_action->set_enabled(directory_view->path_history_position() > 0);
        open_parent_directory_action->set_enabled(breadcrumbbar->has_parent_segment());
        open_child_directory_action->set_enabled(breadcrumbbar->has_child_segment());
        directory_view->view_as_table_action().set_enabled(can_read_in_path);
        directory_view->view_as_icons_action().set_enabled(can_read_in_path);
        directory_view->view_as_columns_action().set_enabled(can_read_in_path);
        statusbar->set_text(MUST(String::formatted("{} 个项目", directory_view->current_view().model()->row_count())));
        preview_pane->set_selection(selected_paths(), new_path);
    };

    directory_view->on_status_message = [statusbar](StringView message) {
        statusbar->set_text(MUST(String::from_utf8(message)));
    };

    directory_view->on_thumbnail_progress = [&](int done, int total) {
        if (done == total) {
            progressbar.set_visible(false);
            return;
        }
        progressbar.set_range(0, total);
        progressbar.set_value(done);
        progressbar.set_visible(true);
    };

    directory_view->on_selection_change = [&](GUI::AbstractView& view) {
        auto& selection = view.selection();
        bool writable = access(directory_view->path().characters(), W_OK) == 0;
        cut_action->set_enabled(!selection.is_empty() && writable);
        copy_action->set_enabled(!selection.is_empty());
        delete_action->set_enabled(!selection.is_empty() && writable);
        directory_view->rename_action().set_enabled(selection.size() == 1 && writable);
        preview_pane->set_selection(selected_paths(), directory_view->path());
        statusbar->set_text(selection.is_empty()
            ? MUST(String::formatted("{} 个项目", directory_view->current_view().model()->row_count()))
            : MUST(String::formatted("选中 {} 个项目", selection.size())));
    };

    // Menu::popup() is async: the WindowServer shows the menu only after this
    // handler returns, so the Menu object must outlive it. Keep the last-popped
    // menu in a function-scope RefPtr until the next popup replaces it.
    // The menu's action callbacks capture the selection (paths/primary) by
    // value ([=]) because those locals are destroyed when this handler returns.
    RefPtr<GUI::Menu> live_context_menu;
    // --- Context menus (shared with the desktop) --------------------------
    directory_view->on_context_menu_request = [&](GUI::ModelIndex const& index, GUI::ContextMenuEvent const& event) {
        if (!index.is_valid()) {
            auto background_menu = GUI::Menu::construct();
            auto background_actions = ExplorerShared::make_background_actions();
            background_actions->paste = [&]() { do_paste(directory_view->path(), window); };
            background_actions->new_folder = [directory_view]() { directory_view->mkdir_action().activate(); };
            background_actions->new_file = [directory_view]() { directory_view->touch_action().activate(); };
            background_actions->open_in_terminal = [&]() { spawn_terminal(window, directory_view->path()); };
            background_actions->refresh = [directory_view]() { directory_view->refresh(); };
            background_actions->properties = [&]() {
                show_properties(directory_view->path(), directory_view->path(), {}, window);
            };
            background_actions->toggle_hidden_files = [&](bool show) {
                show_hidden_files_action->set_checked(show);
                apply_hidden_files(show);
            };
            background_actions->can_paste = can_paste_here();
            background_actions->can_write = access(directory_view->path().characters(), W_OK) == 0;
            background_actions->hidden_files_checked = show_hidden_files;
            ExplorerShared::append_background_actions(*background_menu, background_actions);
            live_context_menu = background_menu;
            background_menu->popup(event.screen_position());
            return;
        }

        auto& node = directory_view->node(index);
        auto clicked_path = node.full_path();
        Vector<ByteString> paths = selected_paths();
        if (paths.is_empty() || !paths.contains_slow(clicked_path))
            paths = { clicked_path };

        ExplorerShared::ItemContext context;
        for (auto const& path : paths)
            context.entries.append(ExplorerShared::Entry::from_path(path));
        context.can_paste = can_paste_here();
        context.can_write = access(directory_view->path().characters(), W_OK) == 0;

        auto actions = ExplorerShared::make_item_actions();
        auto const primary = paths.first();
        actions->open = [&, primary]() {
            if (FileSystem::is_directory(primary)) {
                directory_view->open(primary);
                return;
            }
            if (ExplorerShared::Entry::is_windows_binary(primary)) {
                ExplorerShared::launch_windows_binary(primary, ExplorerShared::WindowsRunMode::Auto);
                return;
            }
            auto handlers = directory_view->get_launch_handlers(primary);
            if (auto default_handler = directory_view->get_default_launch_handler(handlers))
                directory_view->launch(URL::create_with_file_scheme(primary), *default_handler);
            else
                Desktop::Launcher::open(URL::create_with_file_scheme(primary));
        };
        actions->open_in_new_window = [&, primary]() {
            Desktop::Launcher::open(URL::create_with_file_scheme(primary));
        };
        actions->open_in_terminal = [&, primary]() { spawn_terminal(window, primary); };
        actions->run_with_wine = [primary]() {
            ExplorerShared::launch_windows_binary(primary, ExplorerShared::WindowsRunMode::App);
        };
        actions->run_as_game = [primary]() {
            ExplorerShared::launch_windows_binary(primary, ExplorerShared::WindowsRunMode::Game);
        };
        actions->fill_open_with = [&](GUI::Menu& menu) {
            auto handlers = directory_view->get_launch_handlers(primary);
            for (auto& handler : handlers) {
                menu.add_action(handler->create_launch_action([&, path = primary](auto& chosen) {
                    directory_view->launch(URL::create_with_file_scheme(path), chosen);
                }));
            }
            if (handlers.is_empty()) {
                auto placeholder = GUI::Action::create("（无可用程序）"sv, [](auto&) {});
                placeholder->set_enabled(false);
                menu.add_action(move(placeholder));
            }
        };
        actions->cut = [&, paths]() { do_copy(paths, FileOperation::Move); };
        actions->copy = [&, paths]() { do_copy(paths, FileOperation::Copy); };
        actions->copy_path = [&, paths]() {
            StringBuilder builder;
            builder.join('\n', paths.span());
            GUI::Clipboard::the().set_plain_text(builder.string_view());
        };
        actions->paste = [&]() { do_paste(directory_view->path(), window); };
        actions->remove = [&, paths]() { delete_paths(paths, true, window); };
        actions->rename = [directory_view]() { directory_view->rename_action().activate(); };
        actions->create_shortcut = [&, paths]() { do_create_link(paths, window); };
        actions->compress = [&, paths](bool use_zip) { compress_paths(paths, use_zip); };
        actions->extract = [&, paths]() { do_unzip_archive(paths, window); };
        actions->set_wallpaper = [&, primary]() { do_set_wallpaper(primary, window); };
        actions->properties = [&, paths, primary]() {
            show_properties(directory_view->path(), primary, paths, window);
        };

        auto item_menu = GUI::Menu::construct();
        auto default_action = ExplorerShared::append_item_actions(*item_menu, context, actions);
        live_context_menu = item_menu;
        item_menu->popup(event.screen_position(), default_action);
    };

    // --- Go ---------------------------------------------------------------
    directory_view->open(initial_location);
    directory_view->set_focus(true);
    paste_action->set_enabled(can_paste_here());

    window->restore_size_and_position("FileManager"sv, "Window"sv, { { 900, 600 } });
    window->save_size_and_position_on_close("FileManager"sv, "Window"sv);
    window->show();

    directory_view->set_view_mode_from_string(Config::read_string("FileManager"sv, "DirectoryView"sv, "ViewMode"sv, "Icon"sv));
    statusbar->view_switcher().set_active_view(
        directory_view->view_mode() == DirectoryView::ViewMode::Table ? ExplorerViewSwitcher::Details
        : directory_view->view_mode() == DirectoryView::ViewMode::Columns ? ExplorerViewSwitcher::Tiles
                                                                         : ExplorerViewSwitcher::Icons);

    if (!entry_focused_on_init.is_empty()) {
        auto matches = directory_view->current_view().model()->matches(entry_focused_on_init,
            GUI::Model::MatchesFlag::MatchFull | GUI::Model::MatchesFlag::FirstMatchOnly);
        if (!matches.is_empty())
            directory_view->current_view().set_cursor(matches.first(), GUI::AbstractView::SelectionUpdate::Set);
    }

    return GUI::Application::the()->exec();
}
