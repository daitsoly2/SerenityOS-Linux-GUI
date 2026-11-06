/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The only translation unit that includes Xlib. See LinuxX11Presenter.h.

#include "LinuxX11Presenter.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace WindowServer::X11Presenter {

namespace {

Display* s_display { nullptr };
Window s_window { 0 };
GC s_gc { nullptr };
XImage* s_image { nullptr };
int s_width { 0 };
int s_height { 0 };

void drain_events()
{
    // Nobody handles X events yet (resize/close come later), but the queue must
    // not grow without bound while the desktop runs.
    while (s_display && XPending(s_display) > 0) {
        XEvent event {};
        XNextEvent(s_display, &event);
        if (event.type == ConfigureNotify) {
            s_width = event.xconfigure.width;
            s_height = event.xconfigure.height;
        }
    }
}

}

bool open(int width, int height)
{
    if (s_display)
        return true;
    if (width <= 0 || height <= 0)
        return false;

    s_display = XOpenDisplay(nullptr);
    if (!s_display) {
        fprintf(stderr, "x11 presenter: cannot open DISPLAY\n");
        return false;
    }

    int screen = DefaultScreen(s_display);
    auto root = RootWindow(s_display, screen);
    auto black = BlackPixel(s_display, screen);

    s_window = XCreateSimpleWindow(s_display, root, 0, 0,
        static_cast<unsigned>(width), static_cast<unsigned>(height), 0, black, black);
    XStoreName(s_display, s_window, "SerenityOS Desktop");
    XSelectInput(s_display, s_window, ExposureMask | StructureNotifyMask);
    XMapWindow(s_display, s_window);
    XFlush(s_display);

    // ZPixmap at the root depth: 32 bpp on every server we care about, and the
    // framebuffer is ARGB32 (BGRX byte order) which matches little-endian X.
    s_image = XCreateImage(s_display, DefaultVisual(s_display, screen),
        static_cast<unsigned>(DefaultDepth(s_display, screen)), ZPixmap, 0, nullptr,
        static_cast<unsigned>(width), static_cast<unsigned>(height), 32, 0);
    if (!s_image) {
        fprintf(stderr, "x11 presenter: XCreateImage failed\n");
        close();
        return false;
    }
    s_image->data = static_cast<char*>(calloc(1, static_cast<size_t>(s_image->bytes_per_line) * static_cast<size_t>(height)));

    s_gc = XCreateGC(s_display, s_window, 0, nullptr);
    s_width = width;
    s_height = height;

    fprintf(stderr, "x11 presenter: %dx%d on %s\n", width, height, DisplayString(s_display));
    return true;
}

void present(void const* pixels, int pitch, int width, int height)
{
    if (!s_display || !s_image || !s_image->data)
        return;

    int const rows = height < s_height ? height : s_height;
    int const columns = width < s_width ? width : s_width;
    auto const* source = static_cast<char const*>(pixels);
    auto const row_bytes = static_cast<size_t>(columns) * sizeof(unsigned);

    for (int y = 0; y < rows; ++y)
        memcpy(s_image->data + static_cast<size_t>(y) * static_cast<size_t>(s_image->bytes_per_line),
            source + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);

    XPutImage(s_display, s_window, s_gc, s_image, 0, 0, 0, 0,
        static_cast<unsigned>(columns), static_cast<unsigned>(rows));
    XFlush(s_display);
    drain_events();
}

void close()
{
    if (!s_display)
        return;
    if (s_image) {
        // XDestroyImage frees the data pointer we allocated.
        XDestroyImage(s_image);
        s_image = nullptr;
    }
    if (s_gc) {
        XFreeGC(s_display, s_gc);
        s_gc = nullptr;
    }
    if (s_window) {
        XDestroyWindow(s_display, s_window);
        s_window = 0;
    }
    XCloseDisplay(s_display);
    s_display = nullptr;
}

bool is_open()
{
    return s_display != nullptr;
}

int width()
{
    return s_width;
}

int height()
{
    return s_height;
}

}
