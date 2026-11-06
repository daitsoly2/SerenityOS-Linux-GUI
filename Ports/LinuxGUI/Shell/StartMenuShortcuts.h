/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibGUI/Forward.h>

// User-owned start-menu shortcuts.
//
// Classic Windows let you pin/unpin anything to the Start menu. This port used
// to build the Start menu once at startup from /res/apps only, so there was no
// way to add or remove an entry without editing the installed tree (which is
// read-only on the live ISO). These shortcuts live in the user's home instead:
//
//   ~/.local/share/start-menu/<Name>.af
//
//   [App]
//   Name=My Program
//   Executable=/usr/bin/my-program
//   Icon=/res/icons/32x32/app-foo.png
//
// .af is the native Serenity format and is what AppCatalog / DesktopIconsWidget
// already read, so a shortcut created here behaves exactly like a shipped app.
namespace StartMenuShortcuts {

struct Shortcut {
    ByteString name;
    ByteString exec;
    ByteString icon;
    ByteString path;
    ByteString category;
};

// The user shortcut directory (created on demand).
ByteString directory();

Vector<Shortcut> load();

// Write a new .af shortcut. Returns false on I/O failure or duplicate name.
bool add(ByteString const& name, ByteString const& exec, ByteString const& icon);

// Delete a shortcut by its .af path.
bool remove(ByteString const& path);

// Rewrite the Category= key of a user-owned .af (used by drag-to-reclassify).
// An empty category means "top level of the Start menu".
bool set_category(ByteString const& path, ByteString const& category);

// Ask for name / command / icon and create the .af. Returns true when a new
// shortcut was written (the caller is responsible for rebuilding the menu).
bool prompt_add(GUI::Window& window);

// Append the "Shortcuts" submenu (entries + Add / Remove) to the Start menu.
// After a change it asks SystemMenu.cpp to rebuild the Start menu; the rebuild
// is deferred so the currently-open menu is not destroyed in its own handler.
void build_submenu(GUI::Menu& start_menu, GUI::Window& window);

}
