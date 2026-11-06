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
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibGUI/Action.h>
#include <LibGUI/Menu.h>
#include <LibGfx/Bitmap.h>

namespace ExplorerShared {

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
