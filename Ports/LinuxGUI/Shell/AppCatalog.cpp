/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AppCatalog.h"
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibFileSystem/FileSystem.h>
#include <Ports/LinuxGUI/Common/AppIcons.h>
#include <Ports/LinuxGUI/Common/WinApps.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <unistd.h>

// Sort category names while ignoring '&' hotkey markers, so that "&Games"
// lands under G and a parent ("Games") precedes its children ("Games/Puzzles").
static bool less_than_ignore_hotkey(ByteString const& a, ByteString const& b)
{
    auto a_it = a.begin();
    auto b_it = b.begin();
    while (a_it != a.end() && b_it != b.end()) {
        if (*a_it == '&') {
            ++a_it;
            continue;
        }
        if (*b_it == '&') {
            ++b_it;
            continue;
        }
        if (*a_it < *b_it)
            return true;
        if (*a_it > *b_it)
            return false;
        ++a_it;
        ++b_it;
    }
    return a_it == a.end() && b_it != b.end();
}

// GUI::Icon only hands out non-owning bitmaps, so take a reference of our own
// before caching them.
static RefPtr<Gfx::Bitmap const> retain_bitmap(Gfx::Bitmap const* bitmap)
{
    if (!bitmap)
        return {};
    const_cast<Gfx::Bitmap*>(bitmap)->ref();
    return RefPtr<Gfx::Bitmap const> { bitmap };
}

AppCatalog& AppCatalog::the()
{
    static AppCatalog s_catalog;
    return s_catalog;
}

Vector<NonnullRefPtr<Desktop::AppFile>> const& AppCatalog::apps()
{
    if (!m_scanned)
        scan();
    return m_apps;
}

Vector<ByteString> const& AppCatalog::categories()
{
    if (!m_scanned)
        scan();
    return m_categories;
}

void AppCatalog::rescan()
{
    m_scanned = false;
    scan();
}

void AppCatalog::scan()
{
    m_apps.clear();
    m_categories.clear();
    m_scanned = true;

    HashTable<ByteString> seen_categories;

    // Stored as a plain lambda; the for_each call sites use inline wrappers
    // because passing a stored lambda variable to AK::Function fails its
    // lvalue-lambda constraint (inline is the proven pattern in Launcher/Taskbar).
    auto ingest = [&](auto app) {
        if (app->exclude_from_system_menu())
            return;

        // "/bin/Calculator" -> "serenity-calculator"; relative names go through
        // $SERENITY_BIN / $PATH. Apps that are not installed on this host stay
        // out of the menu entirely.
        auto executable = PortHostify::resolve_executable(app->executable());
        if (access(executable.characters(), X_OK) != 0)
            return;

        seen_categories.set(app->category());
        m_apps.append(move(app));
    };

    // System apps shipped with the port.
    Desktop::AppFile::for_each([&](auto app) { ingest(app); });

    // User-installed Windows programs registered by WinAppsSettings each write
    // an .af (Category=&Windows 应用) here, so they surface under their own
    // submenu. Move any legacy Games entry over first.
    WinApps::migrate_menu_category();
    auto user_apps_dir = WinApps::user_apps_dir();
    if (FileSystem::is_directory(user_apps_dir))
        Desktop::AppFile::for_each([&](auto app) { ingest(app); }, user_apps_dir);

    quick_sort(m_apps, [](auto& a, auto& b) { return a->name() < b->name(); });

    for (auto const& category : seen_categories)
        m_categories.append(category);
    quick_sort(m_categories, less_than_ignore_hotkey);
}

RefPtr<Gfx::Bitmap const> AppCatalog::icon_for(Desktop::AppFile const& app, int size)
{
    auto key = ByteString::formatted("{}@{}", app.filename(), size);
    if (auto it = m_icons.find(key); it != m_icons.end())
        return it->value;

    // Icon= may be an absolute path, a freedesktop theme name or a Windows
    // executable; PortIcons resolves all three (and caches the decode).
    auto bitmap = icon_for_path(app.icon_path(), size);
    if (!bitmap) {
        auto icon = app.icon();
        bitmap = retain_bitmap(icon.bitmap_for_size(size));
    }

    m_icons.set(key, bitmap);
    return bitmap;
}

RefPtr<Gfx::Bitmap const> AppCatalog::icon_for_path(ByteString const& path, int size)
{
    if (path.is_empty())
        return {};

    auto key = ByteString::formatted("{}@{}", path, size);
    if (auto it = m_icons.find(key); it != m_icons.end())
        return it->value;

    // Handles absolute paths, .ico, freedesktop theme names (PNG or SVG) and
    // Windows executables; scales to `size` once instead of on every paint.
    auto bitmap = PortIcons::resolve(path, size);

    m_icons.set(key, bitmap);
    return bitmap;
}
