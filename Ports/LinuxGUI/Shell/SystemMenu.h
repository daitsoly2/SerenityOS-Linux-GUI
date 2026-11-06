/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibGUI/Forward.h>
#include <LibGUI/Menu.h>

// Builds the start menu (About / app categories / themes / settings / help /
// run / exit). Lived in Userland/Services/Taskbar/main.cpp before the shell
// merge; it now reads AppCatalog so the menu and the desktop agree on what is
// installed.
//
// Every app entry (system .af, XDG .desktop, or user .af shortcut) can be
// right-clicked to remove it from the Start menu, or dragged onto another
// category submenu to reclassify it. Removing/recategorising a system entry is
// recorded in ~/.config/start-menu-overrides.ini; user .af files are edited
// directly. Both operations rebuild the menu, so the shell must install a
// rebuilder before it can react to them.
ErrorOr<NonnullRefPtr<GUI::Menu>> build_system_menu(GUI::Window& taskbar_window);
void set_start_menu_rebuilder(Function<void()>);
