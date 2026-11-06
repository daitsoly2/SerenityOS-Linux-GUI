/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCore/Notifier.h>
#include <WindowServer/ScreenBackend.h>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

// Opaque handles from the embedded wlroots compositor ABI (C, see
// WSWlrBridge/host/wlroots_backend.h). Declared in the *global* namespace to
// match that header; the pointer members below stay opaque (never dereferenced
// outside the presenter methods).
struct wswlr_compositor;
struct wswlr_desktop_surface;
struct wswlr_layer;

namespace WindowServer {

// Presents the WindowServer framebuffer inside a host (Linux) window.
//
// The compositor renders into an ordinary memory buffer, exactly like
// VirtualScreenBackend does. The only difference is that after a flush we
// upload the touched rectangles to an SDL texture and present it.
class LinuxScreenBackend final : public ScreenBackend {
public:
    LinuxScreenBackend();
    virtual ~LinuxScreenBackend() override;

    static LinuxScreenBackend* last_instance();

    // Probes the display mode without creating a presenter. Used to build the
    // initial screen layout at the real panel resolution instead of changing
    // resolution right after startup (which forces a device close/reopen).
    // Returns {-1,-1} when DRM is unavailable.
    static Gfx::IntSize probe_display_size(int requested_width, int requested_height);

    // Releases the presenter and puts the display back the way we found it.
    // Called explicitly on shutdown because the port's full destructor chain
    // trips a ref-count assert (long-standing teardown bug).
    void restore_display();

    // Writes the currently displayed buffer as a binary PPM. Useful for
    // verifying the port on a machine without a display server.
    ErrorOr<void> dump_to_ppm(ByteString const& path);

    // Asks the display whether the mode changed (KMS hotplug / mode-set event)
    // and, if so, re-probes it. Returns the new size so WindowServer can apply
    // a fresh screen layout. Returns nothing when nothing happened.
    Optional<Gfx::IntSize> take_display_change();

    // The mode the display is actually running, which may differ from the
    // resolution WindowServer asked for. {-1,-1} when not on KMS.
    Gfx::IntSize display_mode_size() const;

    virtual ErrorOr<void> open() override;
    virtual void set_head_buffer(int index) override;
    virtual ErrorOr<void> flush_framebuffer_rects(int buffer_index, ReadonlySpan<FBRect> rects) override;
    virtual ErrorOr<void> unmap_framebuffer() override;
    virtual ErrorOr<void> map_framebuffer() override;
    virtual ErrorOr<void> flush_framebuffer() override;
    virtual ErrorOr<void> set_head_mode_setting(GraphicsHeadModeSetting) override;
    virtual ErrorOr<void> set_safe_head_mode_setting() override;
    virtual ErrorOr<GraphicsHeadModeSetting> get_head_mode_setting() override;

    // Window layers: one wlroots scene node per native window, so the compositor
    // never has to ship a whole-screen framebuffer for a 200x100 change.
    virtual bool supports_window_layers() const override;
    virtual void* create_window_layer(Gfx::IntRect const&) override;
    virtual void* begin_paint_window_layer(void*, Gfx::IntSize const&, size_t& stride_out) override;
    virtual void commit_window_layer(void*, Gfx::IntPoint const&) override;
    virtual void move_window_layer(void*, Gfx::IntPoint const&) override;
    virtual void set_window_layer_enabled(void*, bool) override;
    virtual void raise_window_layer(void*) override;
    virtual void destroy_window_layer(void*) override;

    SDL_Window* window() const { return m_window; }

    // True when the SDL presenter is in use. When it is false the port has no
    // reason to call into SDL at all (KMS presents straight to scanout).
    bool is_using_sdl_presenter() const { return m_window != nullptr; }

    // Opaque DRM/KMS state; defined in the .cpp so this header stays free of
    // libdrm includes.
    struct KmsPresenter;

private:
    ErrorOr<void> create_presenter();
    void destroy_presenter();
    void present_buffer(int buffer_index);

    // DRM/KMS presenter: the compositor still renders into ordinary memory,
    // but we copy that into scanout buffers and page-flip, with no SDL and no X.
    // Selected with SERENITY_PRESENTER=drm; SDL stays the default so a broken
    // mode set can always be recovered by unsetting the variable.
    ErrorOr<void> kms_probe(int requested_width, int requested_height);
    ErrorOr<void> create_kms_presenter();
    void destroy_kms_presenter();
    void present_buffer_kms(int buffer_index);
    // Partial variant: only the given rects changed, so only those pixels are
    // copied into the scanout buffer before the (unavoidably full) page flip.
    void present_buffer_kms_rects(int buffer_index, ReadonlySpan<FBRect> rects);
    // Waits for the previous flip and queues the next one on buffer_index.
    void flip_kms(int buffer_index);
    KmsPresenter* m_kms { nullptr };

    // Embedded wlroots compositor (SERENITY_PRESENTER=wlroots): WindowServer IS
    // the compositor. The native desktop and foreign X11/Wayland clients become
    // equal scene nodes in one wlroots scene graph.
    ErrorOr<void> create_wlroots_presenter();
    void destroy_wlroots_presenter();
    void present_buffer_wlroots(int buffer_index);
    void present_buffer_wlroots_rects(int buffer_index, ReadonlySpan<FBRect> rects);
    wswlr_compositor* m_wlr { nullptr };
    wswlr_desktop_surface* m_wlr_desktop { nullptr };
    RefPtr<Core::Notifier> m_wlr_notifier;

    SDL_Window* m_window { nullptr };
    SDL_Renderer* m_renderer { nullptr };
    SDL_Texture* m_texture { nullptr };

    int m_width { 0 };
    int m_height { 0 };
    int m_active_buffer { 0 };
    bool m_video_initialized { false };

    static LinuxScreenBackend* s_last_instance;
};

}
