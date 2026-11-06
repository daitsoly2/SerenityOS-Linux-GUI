/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibDesktop/AppFile.h>
#include <LibGfx/Bitmap.h>

// One app scan for the whole desktop shell.
//
// The start menu and the desktop's "New" menu used to walk /res/apps separately
// (Taskbar/main.cpp and DesktopIcons/desktop_icons.cpp): two scans, two
// launchability checks and two sets of icon decodes at startup, with the two
// lists free to disagree. They share this catalog now - same app list, same
// order, same bitmap cache - which is the whole point of merging the processes.
class AppCatalog {
public:
    static AppCatalog& the();

    // Scan /res/apps once. Apps whose host executable is missing are dropped:
    // they would only fail to launch, and that is what used to leave the start
    // menu empty before "/bin/Name" was mapped to "serenity-<kebab-case>".
    Vector<NonnullRefPtr<Desktop::AppFile>> const& apps();
    Vector<ByteString> const& categories();

    // Re-read /res/apps (desktop context menu -> Refresh).
    void rescan();

    // Icons are decoded once and cached by source path + size. The returned
    // bitmaps stay alive for the lifetime of the process.
    RefPtr<Gfx::Bitmap const> icon_for(Desktop::AppFile const&, int size);
    RefPtr<Gfx::Bitmap const> icon_for_path(ByteString const&, int size);

private:
    AppCatalog() = default;

    void scan();

    Vector<NonnullRefPtr<Desktop::AppFile>> m_apps;
    Vector<ByteString> m_categories;
    HashMap<ByteString, RefPtr<Gfx::Bitmap const>> m_icons;
    bool m_scanned { false };
};
