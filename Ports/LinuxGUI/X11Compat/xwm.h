/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// XWM: the X-side half of the Xwayland integration.
//
// Rootless Xwayland creates one bare wl_surface per X toplevel and parks the
// matching X window's id in the `_XWAYLAND_WL_SURFACE_ID` property, then waits
// for a window manager to do the pairing. This backend is that window
// manager: it talks Xlib to the Xwayland display and reports every toplevel
// (plus its Wayland surface id) back to the compositor front-end, which owns
// the actual wl_surface objects.
//
// Deliberately free of X11 headers here - X.h defines KeyCode/None/Bool which
// collide with LibGUI/AK, so Xlib lives in xwm.cpp only.

#pragma once

#include <functional>
#include <string>

namespace XWM {

struct WindowInfo {
    unsigned long xid { 0 };
    // Server-side object id of the wl_surface Xwayland created for this X
    // window. Announced by Xwayland 24.x via a root-window ClientMessage
    // (atom WL_SURFACE_ID); much older Xwayland set the
    // _XWAYLAND_WL_SURFACE_ID property instead (still read as a fallback).
    unsigned long wl_surface_id { 0 };
    std::string title;
    int x { 0 };
    int y { 0 };
    int width { 0 };
    int height { 0 };
    // Override-redirect popups (menus, tooltips, combo boxes): they position
    // themselves and must render without a window-manager frame.
    bool override_redirect { false };
    // A managed window whose _NET_WM_WINDOW_TYPE is a popup-ish type (popup
    // menu / utility / tooltip / combo / dropdown menu / menu / notification),
    // e.g. an IME candidate window. These must also be frameless and must not
    // take focus, even though they are not override-redirect.
    bool is_popup { false };
    // WM_TRANSIENT_FOR of the window (popups point at their parent toplevel).
    unsigned long transient_for { 0 };
};

// Connect to the Xwayland display (e.g. ":100") and become its window
// manager: acquire the WM_S<n> selection and redirect structure requests.
bool connect(char const* display_name);

// Non-blocking probe: has Xwayland finished booting far enough to answer X
// connection setups? Gate connect() on this - Xwayland's startup does blocking
// Wayland roundtrips against us, and connecting too early deadlocks the pair.
bool ready(char const* display_name);

// Callbacks. Set them right after connect(), before the first pump().
// on_window_mapped: a toplevel was mapped (or gained a surface id later).
// on_title_changed: WM_NAME of a managed window changed.
// on_window_destroyed: a managed window went away.
extern std::function<void(WindowInfo const&)> on_window_mapped;
extern std::function<void(unsigned long, std::string const&)> on_title_changed;
extern std::function<void(unsigned long)> on_window_destroyed;

// Dispatch all pending X events. Call periodically from the event loop.
void pump();

// Host -> X actions.
void focus_window(unsigned long xid);
void close_window(unsigned long xid);
void resize_window(unsigned long xid, int width, int height);

} // namespace XWM
