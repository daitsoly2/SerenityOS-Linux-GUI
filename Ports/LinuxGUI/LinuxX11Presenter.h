/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// X11 presenter for the Linux port: the whole desktop becomes one X client.
//
// With SERENITY_PRESENTER=x11 the WindowServer stops talking DRM and instead
// presents its framebuffer into an ordinary X window. That is what makes
// "the desktop runs inside Xwayland under a real compositor" work:
//
//     labwc (Wayland) -> Xwayland -> serenity-window-server (X client)
//
// Xlib is kept out of every other translation unit on purpose: X.h defines
// Bool, None, Status and friends, which collide with AK and LibGUI.

#pragma once

#include <cstddef>

namespace WindowServer::X11Presenter {

// Opens a connection, creates the window at the given size and allocates the
// shim image. Returns false when there is no DISPLAY to talk to.
bool open(int width, int height);

// Copies the framebuffer into the window. `pitch` is bytes per source row;
// the source is ARGB32, which matches BGRX on little-endian X servers.
void present(void const* pixels, int pitch, int width, int height);

void close();

bool is_open();

// Size the X server gave us (ConfigureNotify), for future resize handling.
int width();
int height();

}
