/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Context-menu wording / order / icon set shared by the desktop and the file
// manager. Both processes build their menus from the same append_* helpers and
// inject process-specific behaviour through ItemActions / BackgroundActions.
//
// Before this existed the desktop menu was Chinese and the file manager menu
// was English; the desktop had "游戏模式启动" and no Paste, the file manager had
// "用 Wine 运行" and Copy Path, and item order differed. One helper now decides
// the menu, so a given file always offers the same actions in the same order.

#pragma once

#include "ExplorerEntries.h"
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include "hostify.h"
#include "AppShortcut.h"
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/Launcher.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/Action.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibURL/URL.h>
#include <unistd.h>

namespace ExplorerShared {

// ---------------------------------------------------------------------------
// Broken shortcuts
//
// A shortcut can outlive its program (uninstalled app, deleted .exe) or be a
// truncated / hand-edited .af that no longer parses. Both the desktop and the
// file manager double-click such a file and get nothing. Detect that here and
// offer to remove it instead. System-owned launchers are never deleted.

enum class ShortcutStatus {
    NotAShortcut,
    OK,
    Broken,
};

// Wine / Proton launchers are a special case: the shortcut's real target is the
// .lnk (or .exe) in the arguments, while the wrapper binary may only exist
// inside the winapp sandbox. Checking `wine` alone paints a red cross over a
// perfectly usable game shortcut.
inline bool is_windows_launcher(ByteString const& program)
{
    auto name = LexicalPath::basename(program);
    static constexpr StringView wrappers[] = {
        "wine"sv, "wine64"sv, "wine-preloader"sv, "wine64-preloader"sv,
        "umu-run"sv, "proton"sv, "serenity-winapp"sv,
    };
    for (auto wrapper : wrappers) {
        if (name == wrapper)
            return true;
    }
    return false;
}

// Walk up from WorkingDirectory until a directory that actually contains a
// wine prefix (drive_c) is found.
inline Optional<ByteString> wine_prefix_root(PortShortcuts::Record const& record)
{
    auto directory = record.working_directory;
    while (!directory.is_empty() && directory != "/"sv) {
        if (FileSystem::exists(ByteString::formatted("{}/drive_c", directory)))
            return directory;
        auto parent = LexicalPath::dirname(directory);
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
}

// C:\users\Public\Desktop\game.lnk -> <prefix>/drive_c/users/Public/Desktop/game.lnk
inline ByteString windows_argument_to_host(ByteString const& prefix_root, ByteString argument)
{
    argument = argument.replace("\\\\"sv, "/"sv);
    argument = argument.replace("\\"sv, "/"sv);
    while (argument.contains("//"sv))
        argument = argument.replace("//"sv, "/"sv);
    if (argument.length() >= 2 && argument.characters()[1] == ':')
        argument = argument.substring(2);
    if (!argument.starts_with("/"sv))
        argument = ByteString::formatted("/{}", argument);

    auto candidate = ByteString::formatted("{}/drive_c{}", prefix_root, argument);
    return FileSystem::exists(candidate) ? candidate : ByteString {};
}

// Checks a shortcut that is already parsed, so callers that just ran
// PortShortcuts::scan do not have to read the file a second time.
inline ShortcutStatus shortcut_status_for_record(PortShortcuts::Record const& record)
{
    auto resolved = PortHostify::resolve_executable(record.program);
    if (access(resolved.characters(), X_OK) == 0)
        return ShortcutStatus::OK;
    // A wrapper script or a Windows binary behind a launcher is still valid
    // as long as it exists, even if it lost its executable bit.
    if (FileSystem::exists(resolved))
        return ShortcutStatus::OK;

    if (is_windows_launcher(record.program)) {
        // Without a prefix we cannot tell what the shortcut points at - stay
        // quiet instead of offering to delete a launcher that may still work.
        auto root = wine_prefix_root(record);
        if (!root.has_value())
            return ShortcutStatus::OK;
        for (auto const& argument : record.arguments) {
            if (!windows_argument_to_host(root.value(), argument).is_empty())
                return ShortcutStatus::OK;
        }
    }
    return ShortcutStatus::Broken;
}

inline ShortcutStatus shortcut_status(StringView path)
{
    if (!PortShortcuts::is_shortcut_or_executable(path))
        return ShortcutStatus::NotAShortcut;
    ByteString value { path };
    if (PortIcons::is_windows_executable(path))
        return FileSystem::exists(path) ? ShortcutStatus::OK : ShortcutStatus::Broken;

    auto record = PortShortcuts::parse(path);
    if (!record.has_value())
        return ShortcutStatus::Broken; // unreadable / truncated shortcut file
    return shortcut_status_for_record(*record);
}

inline bool is_user_owned(StringView path)
{
    auto home = Core::StandardPaths::home_directory();
    if (home.is_empty() || !path.starts_with(home))
        return false;
    for (auto prefix : { "/usr/"sv, "/etc/"sv, "/res/"sv }) {
        if (path.starts_with(prefix))
            return false;
    }
    return true;
}

// Returns true when the path was a broken shortcut and the user was prompted
// (callers should stop their normal open path in that case).
inline bool offer_delete_broken_shortcut(GUI::Window* window, StringView path)
{
    if (shortcut_status(path) != ShortcutStatus::Broken)
        return false;
    ByteString value { path };
    auto label = LexicalPath::basename(path);

    if (!is_user_owned(path)) {
        GUI::MessageBox::show(window,
            ByteString::formatted("快捷方式“{}”指向的程序已不存在，但它由系统管理，无法删除。", label),
            "无效的快捷方式"sv, GUI::MessageBox::Type::Warning);
        return true;
    }

    auto answer = GUI::MessageBox::show(window,
        ByteString::formatted("快捷方式“{}”无法打开（文件损坏或指向的程序已被删除）。\n是否删除这个快捷方式？", label),
        "无效的快捷方式"sv, GUI::MessageBox::Type::Warning,
        GUI::MessageBox::InputType::YesNo);
    if (answer != GUI::Dialog::ExecResult::Yes)
        return true;

    if (auto result = FileSystem::remove(value, FileSystem::RecursionMode::Disallowed); result.is_error()) {
        GUI::MessageBox::show(window,
            ByteString::formatted("无法删除 {}：{}", value, result.error()),
            "删除快捷方式"sv, GUI::MessageBox::Type::Error);
        return true;
    }
    // Also drop the converted .af cache so a stale entry cannot resurrect it.
    if (value.ends_with(".desktop"sv)) {
        auto cached = PortShortcuts::cache_path_for(path);
        if (!cached.is_empty() && FileSystem::exists(cached))
            (void)FileSystem::remove(cached, FileSystem::RecursionMode::Disallowed);
    }
    return true;
}

// "打开方式" body shared by the desktop and the file manager.
//
// Both used to build this list themselves: the desktop iterated the whole app
// catalog (offering a game for a .zip), the file manager used the MIME
// handler database. One implementation now asks the launcher database, and
// the caller may append catalog entries when nothing is registered.
struct OpenWithApp {
    ByteString name;
    ByteString executable;
};

inline Vector<OpenWithApp> registered_open_with_apps(ByteString const& path)
{
    Vector<OpenWithApp> apps;
    if (path.is_empty())
        return apps;

    auto url = URL::create_with_file_scheme(path);
    for (auto const& details : Desktop::Launcher::get_handlers_with_details_for_url(url)) {
        if (details->executable.is_empty())
            continue;
        OpenWithApp app;
        app.name = details->name.is_empty() ? details->executable : details->name;
        app.executable = details->executable;
        apps.append(move(app));
    }
    return apps;
}

inline void launch_open_with(ByteString const& executable, ByteString const& path)
{
    if (executable.is_empty() || path.is_empty())
        return;
    auto host_executable = PortHostify::resolve_executable(executable);
    Array<ByteString, 1> arguments { path };
    if (auto result = Core::Process::spawn(host_executable.view(),
            ReadonlySpan<ByteString> { arguments.data(), arguments.size() },
            LexicalPath::dirname(path));
        result.is_error())
        warnln("explorer: open-with {} failed: {}", host_executable, result.error());
}

// Adds the actions; a disabled placeholder is better than an empty submenu,
// which shows an arrow that opens nothing.
inline void fill_open_with_menu(GUI::Menu& menu, Vector<OpenWithApp> const& apps, ByteString const& path)
{
    if (apps.is_empty()) {
        auto placeholder = GUI::Action::create("（没有可用的程序）"sv, [](auto&) {});
        placeholder->set_enabled(false);
        menu.add_action(move(placeholder));
        return;
    }
    for (auto const& app : apps) {
        menu.add_action(GUI::Action::create(app.name, [app, path](auto&) {
            launch_open_with(app.executable, path);
        }));
    }
}

// "属性" shared by both sides: the file manager binary owns the real dialog
// (size, permissions, type-specific tabs), so ask it for a single window
// instead of opening the system Settings app.
inline void show_file_properties(ByteString const& path)
{
    if (path.is_empty())
        return;
    auto executable = PortHostify::resolve_executable("serenity-file-manager"sv);
    Array<ByteString, 2> arguments { ByteString { "--properties" }, path };
    if (auto result = Core::Process::spawn(executable.view(),
            ReadonlySpan<ByteString> { arguments.data(), arguments.size() },
            LexicalPath::dirname(path));
        result.is_error())
        warnln("explorer: properties {} failed: {}", path, result.error());
}


inline RefPtr<Gfx::Bitmap const> menu_icon(StringView name)
{
    static HashMap<ByteString, RefPtr<Gfx::Bitmap const>> cache;
    ByteString key { name };
    if (auto it = cache.find(key); it != cache.end())
        return it->value;

    auto bitmap_or_error = Gfx::Bitmap::load_from_uri(ByteString::formatted("resource://icons/16x16/{}", name));
    if (bitmap_or_error.is_error()) {
        cache.set(move(key), nullptr);
        return nullptr;
    }
    RefPtr<Gfx::Bitmap const> bitmap = bitmap_or_error.release_value();
    cache.set(move(key), bitmap);
    return bitmap;
}

// Ref-counted so the callbacks outlive the synchronous menu popup even though
// AK::Function is move-only (it cannot be copied into every Action lambda).
struct ItemActions : public RefCounted<ItemActions> {
    Function<void()> open;
    Function<void()> open_in_new_window;
    Function<void()> open_in_terminal;
    Function<void()> run_with_wine;   // upstream wine, explicit
    Function<void()> run_as_game;     // umu-run + DWProton
    Function<void(GUI::Menu&)> fill_open_with;
    Function<void()> cut;
    Function<void()> copy;
    Function<void()> copy_path;
    Function<void()> paste;
    Function<void()> remove;
    Function<void()> rename;
    Function<void()> create_shortcut;
    Function<void(bool use_zip)> compress;
    Function<void()> extract;
    Function<void()> set_wallpaper;
    Function<void()> properties;
};

struct ItemContext {
    Vector<Entry> entries;
    bool can_paste { false };
    bool can_write { true };
    bool show_copy_path { true };
};

inline NonnullRefPtr<ItemActions> make_item_actions()
{
    return adopt_ref(*new ItemActions);
}

// Appends the common file/folder actions to `menu` and returns the action that
// should be the menu's default (the bold "Open" entry).
inline RefPtr<GUI::Action> append_item_actions(GUI::Menu& menu, ItemContext const& context, NonnullRefPtr<ItemActions> const& actions)
{
    if (context.entries.is_empty())
        return nullptr;

    auto const& first = context.entries.first();
    bool const single = context.entries.size() == 1;
    bool const is_directory = single && first.is_directory();
    bool const is_windows_binary = single && first.is_windows_binary();
    bool const is_archive = single && first.is_archive();
    bool const is_image = single && first.is_image();
    bool const is_shortcut = single && first.is_shortcut;

    RefPtr<GUI::Action> default_action;

    if (actions->open) {
        auto action = GUI::Action::create("打开"sv, menu_icon("open.png"sv), [actions](auto&) {
            if (actions->open)
                actions->open();
        });
        default_action = action;
        menu.add_action(move(action));
    }

    if (is_directory && actions->open_in_new_window) {
        menu.add_action(GUI::Action::create("在新窗口中打开"sv, menu_icon("app-file-manager.png"sv), [actions](auto&) {
            if (actions->open_in_new_window)
                actions->open_in_new_window();
        }));
    }

    if (is_directory && actions->open_in_terminal) {
        menu.add_action(GUI::Action::create("在终端中打开"sv, menu_icon("app-terminal.png"sv), [actions](auto&) {
            if (actions->open_in_terminal)
                actions->open_in_terminal();
        }));
    }

    // Windows executables: "打开" is the auto path (registry-aware launcher),
    // with explicit game/app overrides underneath, identical in both UIs.
    if (is_windows_binary && actions->run_as_game) {
        menu.add_action(GUI::Action::create("以游戏模式启动"sv, menu_icon("filetype-executable.png"sv), [actions](auto&) {
            if (actions->run_as_game)
                actions->run_as_game();
        }));
    }
    if (is_windows_binary && actions->run_with_wine) {
        menu.add_action(GUI::Action::create("用原版 Wine 运行"sv, menu_icon("filetype-executable.png"sv), [actions](auto&) {
            if (actions->run_with_wine)
                actions->run_with_wine();
        }));
    }

    if (actions->fill_open_with) {
        auto open_with = menu.add_submenu("打开方式"_string);
        actions->fill_open_with(*open_with);
    }

    menu.add_separator();
    if (actions->cut) {
        menu.add_action(GUI::Action::create("剪切"sv, menu_icon("cut.png"sv), [actions](auto&) {
            if (actions->cut)
                actions->cut();
        }));
    }
    if (actions->copy) {
        menu.add_action(GUI::Action::create("复制"sv, menu_icon("copy.png"sv), [actions](auto&) {
            if (actions->copy)
                actions->copy();
        }));
    }
    if (context.show_copy_path && actions->copy_path) {
        menu.add_action(GUI::Action::create("复制路径"sv, [actions](auto&) {
            if (actions->copy_path)
                actions->copy_path();
        }));
    }
    if (actions->paste) {
        auto paste = GUI::Action::create("粘贴"sv, menu_icon("paste.png"sv), [actions](auto&) {
            if (actions->paste)
                actions->paste();
        });
        paste->set_enabled(context.can_paste);
        menu.add_action(move(paste));
    }

    menu.add_separator();
    if (actions->remove) {
        menu.add_action(GUI::Action::create("删除"sv, menu_icon("delete.png"sv), [actions](auto&) {
            if (actions->remove)
                actions->remove();
        }));
    }
    if (single && !is_shortcut && actions->rename) {
        menu.add_action(GUI::Action::create("重命名"sv, menu_icon("rename.png"sv), [actions](auto&) {
            if (actions->rename)
                actions->rename();
        }));
    }
    if (actions->create_shortcut) {
        menu.add_action(GUI::Action::create("创建快捷方式"sv, menu_icon("filetype-symlink.png"sv), [actions](auto&) {
            if (actions->create_shortcut)
                actions->create_shortcut();
        }));
    }

    menu.add_separator();
    if (actions->compress) {
        auto compress = menu.add_submenu("压缩为"_string);
        compress->add_action(GUI::Action::create("ZIP 压缩包"sv, menu_icon("filetype-archive.png"sv), [actions](auto&) {
            if (actions->compress)
                actions->compress(true);
        }));
        compress->add_action(GUI::Action::create("Tar.gz 压缩包"sv, menu_icon("filetype-archive.png"sv), [actions](auto&) {
            if (actions->compress)
                actions->compress(false);
        }));
    }
    if (is_archive && actions->extract) {
        menu.add_action(GUI::Action::create("解压到当前文件夹"sv, menu_icon("filetype-archive.png"sv), [actions](auto&) {
            if (actions->extract)
                actions->extract();
        }));
    }
    if (is_image && actions->set_wallpaper) {
        menu.add_action(GUI::Action::create("设为桌面壁纸"sv, menu_icon("app-display-settings.png"sv), [actions](auto&) {
            if (actions->set_wallpaper)
                actions->set_wallpaper();
        }));
    }

    menu.add_separator();
    if (actions->properties) {
        menu.add_action(GUI::Action::create("属性"sv, menu_icon("properties.png"sv), [actions](auto&) {
            if (actions->properties)
                actions->properties();
        }));
    }

    return default_action;
}

struct BackgroundActions : public RefCounted<BackgroundActions> {
    Function<void()> refresh;
    Function<void()> paste;
    Function<void()> new_folder;
    Function<void()> new_file;
    Function<void()> open_in_terminal;
    Function<void()> properties;
    Function<void(bool)> toggle_hidden_files;
    // The desktop has templates and "New > Shortcut" in addition to the two
    // default items. Let it take over the whole "New" submenu instead of
    // ending up with two identically named submenus.
    Function<void(GUI::Menu&)> fill_new_menu;
    bool suppress_default_new_items { false };
    bool can_paste { false };
    bool can_write { true };
    bool hidden_files_checked { false };
};

inline NonnullRefPtr<BackgroundActions> make_background_actions()
{
    return adopt_ref(*new BackgroundActions);
}

inline void append_background_actions(GUI::Menu& menu, NonnullRefPtr<BackgroundActions> const& actions)
{
    if (actions->paste) {
        auto paste = GUI::Action::create("粘贴"sv, menu_icon("paste.png"sv), [actions](auto&) {
            if (actions->paste)
                actions->paste();
        });
        paste->set_enabled(actions->can_paste);
        menu.add_action(move(paste));
    }

    menu.add_separator();
    auto new_menu = menu.add_submenu("新建"_string);
    if (!actions->suppress_default_new_items) {
        if (actions->new_folder) {
            auto folder = GUI::Action::create("文件夹"sv, menu_icon("mkdir.png"sv), [actions](auto&) {
                if (actions->new_folder)
                    actions->new_folder();
            });
            folder->set_enabled(actions->can_write);
            new_menu->add_action(move(folder));
        }
        if (actions->new_file) {
            auto file = GUI::Action::create("文本文档"sv, menu_icon("new.png"sv), [actions](auto&) {
                if (actions->new_file)
                    actions->new_file();
            });
            file->set_enabled(actions->can_write);
            new_menu->add_action(move(file));
        }
    }
    if (actions->fill_new_menu)
        actions->fill_new_menu(*new_menu);

    if (actions->open_in_terminal) {
        menu.add_action(GUI::Action::create("在终端中打开"sv, menu_icon("app-terminal.png"sv), [actions](auto&) {
            if (actions->open_in_terminal)
                actions->open_in_terminal();
        }));
    }
    if (actions->toggle_hidden_files) {
        auto hidden = GUI::Action::create_checkable("显示隐藏文件"sv, [actions](auto& action) {
            if (actions->toggle_hidden_files)
                actions->toggle_hidden_files(action.is_checked());
        });
        hidden->set_checked(actions->hidden_files_checked);
        menu.add_action(hidden);
    }
    if (actions->refresh) {
        menu.add_action(GUI::Action::create("刷新"sv, menu_icon("undo.png"sv), [actions](auto&) {
            if (actions->refresh)
                actions->refresh();
        }));
    }

    menu.add_separator();
    if (actions->properties) {
        menu.add_action(GUI::Action::create("属性"sv, menu_icon("properties.png"sv), [actions](auto&) {
            if (actions->properties)
                actions->properties();
        }));
    }
}

}
