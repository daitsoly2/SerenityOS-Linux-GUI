/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StartMenuShortcuts.h"
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/Action.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <Ports/LinuxGUI/Common/Translation.h>
#include <Ports/LinuxGUI/Common/hostify.h>

namespace StartMenuShortcuts {

// Only the "Shortcuts" submenu is refreshed after a change. Rebuilding the
// whole Start menu would leave the static theme ActionGroup holding pointers
// to actions owned by the discarded menu, so we deliberately keep the parent
// menu alive and just re-populate this one child.
static RefPtr<GUI::Menu> s_shortcuts_menu;
static GUI::Window* s_window;

static void populate(GUI::Menu&, GUI::Window&);

ByteString directory()
{
    auto dir = ByteString::formatted("{}/.local/share/start-menu", Core::StandardPaths::home_directory());
    MUST(Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes));
    return dir;
}

Vector<Shortcut> load()
{
    Vector<Shortcut> result;
    auto dir = directory();
    Core::DirIterator it { dir, Core::DirIterator::SkipDots };
    while (it.has_next()) {
        auto filename = it.next_path();
        if (!filename.ends_with(".af"sv))
            continue;
        auto path = ByteString::formatted("{}/{}", dir, filename);
        auto config = Core::ConfigFile::open(path);
        if (config.is_error())
            continue;
        auto const& cfg = *config.value();
        auto name = cfg.read_entry("App", "Name").trim_whitespace();
        auto exec = cfg.read_entry("App", "Executable").trim_whitespace();
        if (name.is_empty() || exec.is_empty())
            continue;
        // DesktopIconsWidget reads "Icon"; LibDesktop::AppFile reads
        // "IconPath". User shortcuts write both, but tolerate either here.
        auto icon = cfg.read_entry("App", "Icon").trim_whitespace();
        if (icon.is_empty())
            icon = cfg.read_entry("App", "IconPath").trim_whitespace();
        auto category = cfg.read_entry("App", "Category").trim_whitespace();
        result.append({ name, exec, icon, path, category });
    }
    quick_sort(result, [](auto const& a, auto const& b) { return a.name < b.name; });
    return result;
}

static ByteString sanitized_filename(ByteString const& name)
{
    StringBuilder builder;
    for (auto ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '&' || ch == '.' || ch == '\n')
            builder.append('_');
        else
            builder.append(ch);
    }
    auto base = builder.to_byte_string().trim_whitespace();
    return base.is_empty() ? ByteString { "Shortcut" } : base;
}

bool add(ByteString const& name, ByteString const& exec, ByteString const& icon)
{
    auto base = sanitized_filename(name);
    auto path = ByteString::formatted("{}/{}.af", directory(), base);
    int n = 1;
    while (FileSystem::exists(path))
        path = ByteString::formatted("{}/{}.{}.af", directory(), base, n++);

    // .af is the native Serenity shortcut format. Both icon keys are written
    // so the desktop scanner and LibDesktop::AppFile agree on the icon.
    auto content = ByteString::formatted(
        "[App]\n"
        "Name={}\n"
        "Executable={}\n"
        "Icon={}\n"
        "IconPath={}\n"
        // Must match the raw category key used by /res/apps/*.af (&Utilities
        // has an Alt-shortcut ampersand); a plain "Utilities" would create a
        // second, untranslated category menu.
        "Category=&Utilities\n",
        name, exec, icon, icon);
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error()) {
        warnln("start menu: cannot write {}: {}", path, file.error());
        return false;
    }
    if (file.value()->write_until_depleted(content.bytes()).is_error()) {
        warnln("start menu: cannot write {}", path);
        return false;
    }
    return true;
}

bool remove(ByteString const& path)
{
    if (FileSystem::remove(path, FileSystem::RecursionMode::Disallowed).is_error()) {
        warnln("start menu: cannot remove {}", path);
        return false;
    }
    return true;
}

bool prompt_add(GUI::Window& window)
{
    String name;
    if (GUI::InputBox::show(&window, name, "名称："sv, "添加快捷方式"sv, GUI::InputType::NonemptyText) != GUI::Dialog::ExecResult::OK)
        return false;
    String exec;
    if (GUI::InputBox::show(&window, exec, "命令（例如 /usr/bin/firefox-esr）："sv, "添加快捷方式"sv, GUI::InputType::NonemptyText) != GUI::Dialog::ExecResult::OK)
        return false;
    String icon;
    if (GUI::InputBox::show(&window, icon, "图标路径（可留空）："sv, "添加快捷方式"sv) != GUI::Dialog::ExecResult::OK)
        return false;
    if (!add(name.to_byte_string().trim_whitespace(), exec.to_byte_string().trim_whitespace(), icon.to_byte_string().trim_whitespace())) {
        (void)GUI::MessageBox::show_error(&window, "无法创建快捷方式"sv);
        return false;
    }
    return true;
}

bool set_category(ByteString const& path, ByteString const& category)
{
    auto opened = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
    if (opened.is_error()) {
        warnln("start menu: cannot open {}: {}", path, opened.error());
        return false;
    }
    auto& cfg = *opened.value();
    if (category.is_empty())
        cfg.remove_entry("App", "Category");
    else
        cfg.write_entry("App", "Category", category);
    if (auto result = cfg.sync(); result.is_error()) {
        warnln("start menu: cannot write {}: {}", path, result.error());
        return false;
    }
    return true;
}

static RefPtr<Gfx::Bitmap const> load_icon(ByteString const& path)
{
    if (path.is_empty())
        return nullptr;
    auto bitmap = Gfx::Bitmap::load_from_file(path);
    if (bitmap.is_error())
        return nullptr;
    return bitmap.release_value();
}

static void refresh()
{
    if (!s_shortcuts_menu || !s_window)
        return;
    populate(*s_shortcuts_menu, *s_window);
}

static void populate(GUI::Menu& menu, GUI::Window& window)
{
    menu.remove_all_actions();
    auto shortcuts = load();

    for (auto const& shortcut : shortcuts) {
        auto icon = load_icon(shortcut.icon);
        menu.add_action(GUI::Action::create(shortcut.name, icon, [shortcut, &window](auto&) {
            auto program = PortHostify::resolve_executable(shortcut.exec);
            GUI::Process::spawn_or_show_error(&window, program, ReadonlySpan<StringView> {});
        }));
    }

    menu.add_separator();

    // "Add shortcut..." - three prompts: name, command, optional icon.
    menu.add_action(GUI::Action::create(
        PortTranslation::tr("Menu"sv, "AddShortcut"sv, "添加快捷方式(&A)..."sv),
        [&window](auto&) {
            String name;
            if (GUI::InputBox::show(&window, name, "名称："sv, "添加快捷方式"sv, GUI::InputType::NonemptyText) != GUI::Dialog::ExecResult::OK)
                return;
            String exec;
            if (GUI::InputBox::show(&window, exec, "命令（例如 /usr/bin/firefox-esr）："sv, "添加快捷方式"sv, GUI::InputType::NonemptyText) != GUI::Dialog::ExecResult::OK)
                return;
            String icon;
            if (GUI::InputBox::show(&window, icon, "图标路径（可留空）："sv, "添加快捷方式"sv) != GUI::Dialog::ExecResult::OK)
                return;
            if (!add(name.to_byte_string().trim_whitespace(), exec.to_byte_string().trim_whitespace(), icon.to_byte_string().trim_whitespace())) {
                (void)GUI::MessageBox::show_error(&window, "无法创建快捷方式"sv);
                return;
            }
            // Deferred: the menu that owns this action is still open here.
            Core::deferred_invoke([] { refresh(); });
        }));

    // "Remove shortcut..." - one entry per user shortcut.
    auto remove_menu = menu.add_submenu(MUST(String::from_byte_string(
        PortTranslation::tr("Menu"sv, "RemoveShortcut"sv, "删除快捷方式(&R)"sv))));
    if (shortcuts.is_empty()) {
        auto empty = GUI::Action::create(ByteString { "（无自定义快捷方式）" }, [](auto&) {});
        empty->set_enabled(false);
        remove_menu->add_action(empty);
    } else {
        for (auto const& shortcut : shortcuts) {
            remove_menu->add_action(GUI::Action::create(shortcut.name, [shortcut](auto&) {
                if (remove(shortcut.path))
                    Core::deferred_invoke([] { refresh(); });
            }));
        }
    }
}

void build_submenu(GUI::Menu& start_menu, GUI::Window& window)
{
    s_window = &window;
    s_shortcuts_menu = start_menu.add_submenu(MUST(String::from_byte_string(
        PortTranslation::tr("Menu"sv, "Shortcuts"sv, "快捷方式(&S)"sv))));
    populate(*s_shortcuts_menu, window);
}

}
