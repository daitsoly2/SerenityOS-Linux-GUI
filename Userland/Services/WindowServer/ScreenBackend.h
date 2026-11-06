/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "ScreenLayout.h"
#include <AK/Error.h>
#include <AK/Span.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <sys/ioctl.h>

// On SerenityOS these PODs come from the kernel's <sys/ioctl.h>. Host builds
// (the Linux GUI port) use the host libc's <sys/ioctl.h>, so provide the same
// layout locally.
#ifndef __serenity__
struct FBRect {
    int head_index;
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
};

struct GraphicsHeadModeSetting {
    int horizontal_stride;
    int pixel_clock_in_khz;
    int horizontal_active;
    int horizontal_front_porch_pixels;
    int horizontal_sync_time_pixels;
    int horizontal_blank_pixels;
    int vertical_active;
    int vertical_front_porch_lines;
    int vertical_sync_time_lines;
    int vertical_blank_lines;
    int horizontal_offset;
    int vertical_offset;
};
#endif

namespace Gfx {
class Bitmap;
}

namespace WindowServer {

// Handles low-level device interfacing for the screen.
// ScreenBackend is a thin transparent wrapper around framebuffer-related data which is responsible for setting up this data,
// tearing it down, changing its properties like size, and performing flushes.
// The screen is intended to directly access the members to perform its function, but it only ever reads from anything
// except the data in the framebuffer memory.
class ScreenBackend {
public:
    virtual ~ScreenBackend() = default;

    virtual ErrorOr<void> open() = 0;

    virtual void set_head_buffer(int index) = 0;

    virtual ErrorOr<void> flush_framebuffer_rects(int buffer_index, ReadonlySpan<FBRect> rects) = 0;

    virtual ErrorOr<void> unmap_framebuffer() = 0;
    virtual ErrorOr<void> map_framebuffer() = 0;

    virtual ErrorOr<void> flush_framebuffer() = 0;

    virtual ErrorOr<void> set_head_mode_setting(GraphicsHeadModeSetting) = 0;
    virtual ErrorOr<void> set_safe_head_mode_setting() = 0;
    virtual ErrorOr<GraphicsHeadModeSetting> get_head_mode_setting() = 0;

    // ── Window layers (Linux wlroots presenter only) ────────────────────────
    // When the WindowServer is itself the compositor, painting every window into
    // one screen framebuffer and then uploading that whole framebuffer is the
    // wrong shape: it needs a shared damage contract between the two, which is
    // exactly where the stale-half black rectangles came from. Backends that can
    // host a surface per window return true here, and the compositor then renders
    // each window straight into its own layer instead.
    //
    // begin_paint_window_layer returns writable pixels of the given *physical*
    // size, or nullptr when every buffer of that layer is still being read by the
    // renderer (the compositor just skips that window this pass).
    virtual bool supports_window_layers() const { return false; }
    virtual void* create_window_layer(Gfx::IntRect const&) { return nullptr; }
    virtual void* begin_paint_window_layer(void*, Gfx::IntSize const&, size_t& stride_out)
    {
        stride_out = 0;
        return nullptr;
    }
    virtual void commit_window_layer(void*, Gfx::IntPoint const&) { }
    virtual void move_window_layer(void*, Gfx::IntPoint const&) { }
    virtual void set_window_layer_enabled(void*, bool) { }
    virtual void raise_window_layer(void*) { }
    virtual void destroy_window_layer(void*) { }

    bool m_can_device_flush_buffers { true };
    bool m_can_device_flush_entire_framebuffer { true };
    bool m_can_set_head_buffer { false };

    Gfx::ARGB32* m_framebuffer { nullptr };
    size_t m_size_in_bytes { 0 };
    size_t m_max_size_in_bytes { 0 };
    size_t m_back_buffer_offset { 0 };

    int m_pitch { 0 };
};

}
