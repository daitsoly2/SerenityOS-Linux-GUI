/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SystemMenu.h"
#include "AppCatalog.h"
#include "StartMenuOverrides.h"
#include "StartMenuShortcuts.h"
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <ShutdownDialog.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/AppFile.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MenuItem.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <Ports/LinuxGUI/Common/AppShortcut.h>
#include <Ports/LinuxGUI/Common/Translation.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <unistd.h>

// The theme submenu is rebuilt in place on theme changes, so it has to outlive
// this function; keeping the state at file scope also keeps the callback free
// of dangling captures.
static Vector<Gfx::SystemThemeMetaData> s_themes;
static RefPtr<GUI::Menu> s_themes_menu;
static GUI::ActionGroup s_themes_group;

// --- Start menu item management ---------------------------------------------
// Menu items are rendered by WindowServer, which reports right-clicks and
// drag-drops back through GUI::Menu callbacks (see WindowClient.ipc). The
// bindings below map (menu, item identifier) -> the underlying launcher entry,
// so the shell can hide/move the right file without having to parse menu text.

enum class EntryKind { SystemApp, LinuxDesktop, UserShortcut };

struct StartEntry {
    ByteString key;      // stable id: source .af / .desktop path
    ByteString name;     // display name, for the confirmation dialog
    ByteString category; // effective category (empty = Start menu root)
    EntryKind kind { EntryKind::SystemApp };
    ByteString path;     // user-owned .af path, when kind == UserShortcut
};

struct MenuBinding {
    GUI::Menu* menu { nullptr };
    ByteString category; // raw category string; empty = Start menu root
    bool accepts_drop { false };
    unsigned generation { 0 };
    HashMap<unsigned, StartEntry> entries;
};

static Vector<MenuBinding> s_bindings;
static HashMap<GUI::Menu*, size_t> s_menu_binding;
static unsigned s_generation { 0 };
static GUI::Window* s_window { nullptr };
static Function<void()> s_rebuild;

static void start_menu_context(size_t binding_index, unsigned identifier);
static void start_menu_dropped(size_t binding_index, unsigned source_identifier, int target_menu_id, int target_identifier, Gfx::IntPoint position);

static size_t bind_menu(GUI::Menu& menu, ByteString category, bool accepts_drop)
{
    auto index = s_bindings.size();
    MenuBinding binding;
    binding.menu = &menu;
    binding.category = move(category);
    binding.accepts_drop = accepts_drop;
    binding.generation = s_generation;
    s_bindings.append(move(binding));
    s_menu_binding.set(&menu, index);

    GUI::Menu::set_item_callbacks(menu,
        [index](unsigned identifier, Gfx::IntPoint) {
            start_menu_context(index, identifier);
        },
        [index](unsigned source_identifier, int target_menu_id, int target_identifier, Gfx::IntPoint position) {
            start_menu_dropped(index, source_identifier, target_menu_id, target_identifier, position);
        });
    return index;
}

static void add_managed_action(GUI::Menu& menu, NonnullRefPtr<GUI::Action> action, StartEntry entry)
{
    menu.add_action(move(action));
    auto index = s_menu_binding.get(&menu);
    if (!index.has_value())
        return;
    auto identifier = static_cast<unsigned>(menu.items().size() - 1);
    s_bindings[index.value()].entries.set(identifier, move(entry));
}

static Optional<StartEntry> find_entry(size_t binding_index, unsigned identifier)
{
    if (binding_index >= s_bindings.size() || s_bindings[binding_index].generation != s_generation)
        return {};
    auto& entries = s_bindings[binding_index].entries;
    auto it = entries.find(identifier);
    if (it == entries.end())
        return {};
    return it->value;
}

static void schedule_rebuild()
{
    // Never destroy the menu from inside its own action/event handler.
    Core::deferred_invoke([] {
        if (s_rebuild)
            s_rebuild();
    });
}

static void apply_remove(StartEntry const& entry)
{
    if (entry.kind == EntryKind::UserShortcut)
        StartMenuShortcuts::remove(entry.path);
    else
        StartMenuOverrides::set_hidden(entry.key, true);
}

static void remove_with_confirmation(StartEntry const& entry)
{
    auto text = ByteString::formatted(
        "确定从开始菜单删除「{}」吗？\n\n仅从开始菜单移除，不会卸载或删除程序。", entry.name);
    auto result = GUI::MessageBox::show(s_window, text.view(), "删除快捷方式"sv,
        GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo);
    if (result != GUI::Dialog::ExecResult::Yes)
        return;
    apply_remove(entry);
    schedule_rebuild();
}

static void apply_category(StartEntry const& entry, ByteString const& category)
{
    if (entry.kind == EntryKind::UserShortcut)
        StartMenuShortcuts::set_category(entry.path, category);
    else
        StartMenuOverrides::set_category(entry.key, category);
}

static void start_menu_context(size_t binding_index, unsigned identifier)
{
    auto entry = find_entry(binding_index, identifier);
    if (!entry.has_value())
        return;
    remove_with_confirmation(*entry);
}

static void start_menu_dropped(size_t binding_index, unsigned source_identifier, int target_menu_id, int target_identifier, Gfx::IntPoint)
{
    auto entry = find_entry(binding_index, source_identifier);
    if (!entry.has_value())
        return;

    // Dragged out of every menu window = "remove from Start" (still confirmed).
    if (target_menu_id < 0) {
        remove_with_confirmation(*entry);
        return;
    }

    MenuBinding const* target = nullptr;
    for (auto const& binding : s_bindings) {
        if (binding.generation == s_generation && binding.menu && binding.menu->menu_id() == target_menu_id) {
            target = &binding;
            break;
        }
    }
    if (!target)
        return;

    ByteString new_category = target->category;
    bool accepts = target->accepts_drop;

    // A drop that lands on a category *item* in the root menu means "put it in
    // that submenu", even though the hit was reported against the parent menu.
    if (target_identifier >= 0 && target->menu) {
        auto const& items = target->menu->items();
        if (static_cast<size_t>(target_identifier) < items.size()) {
            GUI::Menu const* submenu = items[target_identifier]->submenu();
            if (submenu) {
                for (auto const& binding : s_bindings) {
                    if (binding.generation == s_generation && binding.menu == submenu) {
                        new_category = binding.category;
                        accepts = binding.accepts_drop;
                        break;
                    }
                }
            }
        }
    }

    if (!accepts || new_category == entry->category)
        return;
    apply_category(*entry, new_category);
    schedule_rebuild();
}

void set_start_menu_rebuilder(Function<void()> rebuild)
{
    s_rebuild = move(rebuild);
}

// Not every app that upstream's menu points at is built for the host (Help needs
// LibWeb, SystemMonitor needs LibSymbolication). Skip dead entries instead of
// letting the user click into "posix_spawn: No such file or directory".
static bool app_is_installed(StringView serenity_path)
{
    auto resolved = PortHostify::hostify_executable(ByteString { serenity_path });
    return access(resolved.characters(), X_OK) == 0;
}

ErrorOr<NonnullRefPtr<GUI::Menu>> build_system_menu(GUI::Window& window)
{
    // A rebuild destroys the previous menu tree. Action's destructor does not
    // unregister from an ActionGroup, so detach the old theme actions first.
    s_themes_group.clear();
    s_bindings.clear();
    s_menu_binding.clear();
    ++s_generation;
    s_window = &window;

    auto system_menu = GUI::Menu::construct("\xE2\x9A\xA1"_string); // HIGH VOLTAGE SIGN
    bind_menu(*system_menu, ByteString {}, true);

    system_menu->add_action(GUI::Action::create(PortTranslation::tr("Menu"sv, "About"sv, "&About SerenityOS"sv), TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/ladyball.png"sv)), [&](auto&) {
        GUI::Process::spawn_or_show_error(&window, "/bin/About"sv);
    }));

    system_menu->add_separator();

    // First we construct all the necessary app category submenus.
    auto category_icons = TRY(Core::ConfigFile::open("/res/icons/SystemMenu.ini"));
    HashMap<ByteString, NonnullRefPtr<GUI::Menu>> app_category_menus;

    Function<void(ByteString const&)> create_category_menu;
    create_category_menu = [&](ByteString const& category) {
        if (app_category_menus.contains(category))
            return;
        ByteString parent_category, child_category = category;
        for (ssize_t i = category.length() - 1; i >= 0; i--) {
            if (category[i] == '/') {
                parent_category = category.substring(0, i);
                child_category = category.substring(i + 1);
            }
        }
        GUI::Menu* parent_menu;
        if (parent_category.is_empty()) {
            parent_menu = system_menu;
        } else {
            if (!app_category_menus.contains(parent_category))
                create_category_menu(parent_category);
            parent_menu = app_category_menus.get(parent_category).value();
        }
        auto category_menu = parent_menu->add_submenu(String::from_byte_string(child_category).release_value_but_fixme_should_propagate_errors());
        auto category_icon_path = category_icons->read_entry("16x16", category);
        if (!category_icon_path.is_empty()) {
            auto icon_or_error = Gfx::Bitmap::load_from_file(category_icon_path);
            if (!icon_or_error.is_error())
                category_menu->set_icon(icon_or_error.release_value());
        }
        bind_menu(*category_menu, category, true);
        app_category_menus.set(category, category_menu);
    };

    auto menu_for_category = [&](ByteString const& category) -> GUI::Menu* {
        if (category.is_empty())
            return system_menu;
        if (!app_category_menus.contains(category))
            create_category_menu(category);
        return app_category_menus.get(category).value();
    };

    for (auto const& category : AppCatalog::the().categories())
        create_category_menu(category);

    // Serenity applications (/res/apps/*.af, read-only). Hiding and moving are
    // recorded as per-user overrides instead of touching the installed tree.
    for (auto const& app : AppCatalog::the().apps()) {
        auto key = app->filename();
        auto override = StartMenuOverrides::get(key);
        if (override.hidden)
            continue;
        auto category = override.category.is_empty() ? app->category() : override.category;
        auto icon = AppCatalog::the().icon_for(*app, 16);
        auto* parent_menu = menu_for_category(category);
        add_managed_action(*parent_menu,
            GUI::Action::create(app->menu_name(), icon, [app, &window](auto&) {
                app->spawn_with_escalation_or_show_error(window);
            }),
            { key, app->menu_name(), category, EntryKind::SystemApp, {} });
    }

    // Linux-native applications: freedesktop .desktop launchers from the
    // standard XDG directories (user dir shadows local shadows system), in
    // their own submenu so they never crowd out the SerenityOS apps.
    struct LinuxDesktopApp {
        ByteString name;
        ByteString program;
        Vector<ByteString> arguments;
        ByteString icon_name;
        ByteString path;
        ByteString category;
        bool hidden { false };
    };
    Vector<LinuxDesktopApp> linux_apps;
    {
        // One unified scan: .desktop files are converted to the native .af
        // shape once, saved, and reused on later runs (see AppShortcut.h).
        // belongs_in_start_menu() is what keeps console tools / setup helpers
        // that ship with Terminal=false out of the menu.
        for (auto& record : PortShortcuts::scan(PortShortcuts::xdg_application_directories())) {
            if (!PortShortcuts::belongs_in_start_menu(record))
                continue;
            LinuxDesktopApp app;
            app.name = record.name;
            app.program = record.program;
            app.arguments = record.arguments;
            app.icon_name = record.icon_spec;
            app.path = record.source_file;
            auto override = StartMenuOverrides::get(record.source_file);
            app.hidden = override.hidden;
            app.category = override.category;
            TRY(linux_apps.try_append(move(app)));
        }
        quick_sort(linux_apps, [](auto& a, auto& b) { return a.name < b.name; });
    }

    bool has_linux_apps = false;
    for (auto const& app : linux_apps) {
        if (app.hidden || !app.category.is_empty())
            continue;
        has_linux_apps = true;
        break;
    }
    RefPtr<GUI::Menu> linux_menu;
    if (has_linux_apps) {
        linux_menu = system_menu->add_submenu(MUST(String::from_byte_string(PortTranslation::tr("Menu"sv, "LinuxApps"sv, "&Linux Apps"sv))));
        bind_menu(*linux_menu, ByteString {}, false);
    }

    int linux_app_identifier = 0;
    for (auto const& app : linux_apps) {
        if (app.hidden)
            continue;
        auto* parent_menu = app.category.is_empty() ? linux_menu.ptr() : menu_for_category(app.category);
        if (!parent_menu)
            continue;
        // Path, theme name (PNG or SVG) or an executable - one resolver.
        auto icon = PortIcons::resolve(app.icon_name, 16);
        auto captured_identifier = linux_app_identifier;
        add_managed_action(*parent_menu,
            GUI::Action::create(app.name, icon, [captured_identifier, &window, linux_apps](auto&) {
                auto const& app = linux_apps[captured_identifier];
                // Exec= in a .desktop file is normally a bare program name. Turn
                // it into the absolute host path here so the child inherits a
                // launch we have already proven works, instead of relying on
                // PATH lookup inside posix_spawn.
                auto program = PortHostify::resolve_executable(app.program);
                GUI::Process::spawn_or_show_error(&window, program, app.arguments.span());
            }),
            { app.path, app.name, app.category, EntryKind::LinuxDesktop, {} });
        ++linux_app_identifier;
    }

    // User-owned .af shortcuts are ordinary Start menu entries: they land in
    // the category named by their Category= key (or at the top level) and can
    // be right-clicked / dragged exactly like the shipped applications.
    for (auto const& shortcut : StartMenuShortcuts::load()) {
        auto override = StartMenuOverrides::get(shortcut.path);
        if (override.hidden)
            continue;
        auto category = override.category.is_empty() ? shortcut.category : override.category;
        auto* parent_menu = menu_for_category(category);
        auto program = PortHostify::resolve_executable(shortcut.exec);
        add_managed_action(*parent_menu,
            GUI::Action::create(shortcut.name, nullptr, [program, &window](auto&) {
                GUI::Process::spawn_or_show_error(&window, program, ReadonlySpan<StringView> {});
            }),
            { shortcut.path, shortcut.name, category, EntryKind::UserShortcut, shortcut.path });
    }

    system_menu->add_separator();

    // Adding a shortcut is a single top-level action, not a management submenu.
    system_menu->add_action(GUI::Action::create(
        PortTranslation::tr("Menu"sv, "AddShortcut"sv, "添加快捷方式(&A)..."sv),
        [&window](auto&) {
            if (StartMenuShortcuts::prompt_add(window))
                schedule_rebuild();
        }));

    system_menu->add_separator();

    s_themes_group.set_exclusive(true);
    s_themes_group.set_unchecking_allowed(false);

    s_themes_menu = system_menu->add_submenu(MUST(String::from_byte_string(PortTranslation::tr("Menu"sv, "Themes"sv, "&Themes"sv))));
    s_themes_menu->set_icon(TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/themes.png"sv)));

    s_themes = TRY(Gfx::list_installed_system_themes());
    auto current_theme_name = GUI::ConnectionToWindowServer::the().get_system_theme();

    {
        for (auto& theme : s_themes) {
            auto action = GUI::Action::create_checkable(theme.menu_name, [theme, current_theme_name, &window](auto&) {
                dbgln("Theme switched from {} to {} at path {}", current_theme_name, theme.name, theme.path);
                if (window.main_widget()->palette().color_scheme_path() != ""sv)
                    VERIFY(GUI::ConnectionToWindowServer::the().set_system_theme(theme.path, theme.name, false, GUI::ConnectionToWindowServer::the().get_preferred_color_scheme()));
                else
                    VERIFY(GUI::ConnectionToWindowServer::the().set_system_theme(theme.path, theme.name, false, "Custom"sv));
            });
            if (theme.name == current_theme_name)
                action->set_checked(true);
            s_themes_group.add_action(action);
            s_themes_menu->add_action(action);
        }
    }

    GUI::Application::the()->on_theme_change = [] {
        if (s_themes_menu->is_visible())
            return;
        auto current_theme_name = GUI::ConnectionToWindowServer::the().get_system_theme();
        auto theme_overridden = GUI::ConnectionToWindowServer::the().is_system_theme_overridden();
        VERIFY(s_themes.size() == s_themes_menu->items().size());
        for (size_t index = 0; index < s_themes.size(); ++index) {
            auto* action = s_themes_menu->action_at(index);
            auto& theme = s_themes[index];
            action->set_checked(!theme_overridden && theme.name == current_theme_name);
        }
    };

    system_menu->add_action(GUI::Action::create(PortTranslation::tr("Menu"sv, "Settings"sv, "&Settings"sv), TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/app-settings.png"sv)), [&](auto&) {
        GUI::Process::spawn_or_show_error(&window, "/bin/Settings"sv);
    }));

    system_menu->add_separator();
    if (app_is_installed("/bin/Help"sv)) {
        system_menu->add_action(GUI::Action::create(PortTranslation::tr("Menu"sv, "Help"sv, "&Help"sv), TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/app-help.png"sv)), [&](auto&) {
            GUI::Process::spawn_or_show_error(&window, "/bin/Help"sv);
        }));
    }
    system_menu->add_action(GUI::Action::create(PortTranslation::tr("Menu"sv, "Run"sv, "&Run..."sv), TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/app-run.png"sv)), [&](auto&) {
        GUI::Process::spawn_or_show_error(&window, "/bin/Run"sv, ReadonlySpan<StringView> {}, Core::StandardPaths::home_directory());
    }));
    system_menu->add_separator();
    system_menu->add_action(GUI::Action::create(PortTranslation::tr("Menu"sv, "Exit"sv, "E&xit..."sv), TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/power.png"sv)), [&](auto&) {
        if (auto command = ShutdownDialog::show(); command.has_value())
            GUI::Process::spawn_or_show_error(&window, command->executable, command->arguments);
    }));

    return system_menu;
}
