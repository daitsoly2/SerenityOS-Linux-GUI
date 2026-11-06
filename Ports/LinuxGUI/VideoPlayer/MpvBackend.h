/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/RefPtr.h>
#include <LibGfx/Size.h>
#include <atomic>

struct mpv_handle;
struct mpv_render_context;

namespace VideoPlayer {

// Playback backend backed by libmpv (loaded with dlopen, so the app still
// starts when the library is missing). Renders into an in-memory BGRx surface
// through mpv's software render API and hands that surface to LibGUI as a
// Gfx::Bitmap. This replaces the port's LibMedia backend, which could only
// decode VP8/VP9 inside a Matroska container.
class MpvBackend {
public:
    MpvBackend() = default;
    ~MpvBackend();

    MpvBackend(MpvBackend const&) = delete;
    MpvBackend& operator=(MpvBackend const&) = delete;

    static bool is_available();

    bool initialize(ByteString& error_message);
    bool load_file(ByteString const& path, ByteString& error_message);
    void close_file();

    void set_paused(bool paused);
    bool is_paused() const;
    void toggle_pause();
    void seek_seconds(double seconds);
    void set_volume(double percent);

    double duration() const;
    double position() const;
    double volume() const;
    bool has_media() const { return m_has_media; }

    // Display size advertised by the decoder (aspect corrected), capped so the
    // software renderer never allocates an unreasonable surface.
    Gfx::IntSize video_size() const;

    // Set by mpv's render update callback (which runs on mpv's own thread).
    bool frame_pending() const { return m_frame_pending.load(std::memory_order_relaxed); }

    // Renders the newest frame into a width x height BGRx surface. Returns the
    // wrapped bitmap, or nullptr when nothing could be drawn.
    Gfx::Bitmap const* render_frame(int width, int height);

private:
    static void render_update_callback(void*);
    bool ensure_surface(int width, int height);

    mpv_handle* m_mpv { nullptr };
    mpv_render_context* m_render_context { nullptr };

    void* m_surface { nullptr };
    size_t m_surface_size { 0 };
    size_t m_surface_stride { 0 };
    int m_surface_width { 0 };
    int m_surface_height { 0 };
    RefPtr<Gfx::Bitmap> m_surface_bitmap;

    std::atomic<bool> m_frame_pending { false };
    bool m_has_media { false };
};

}
