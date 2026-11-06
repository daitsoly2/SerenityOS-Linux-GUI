/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <EGL/egl.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <xf86drmMode.h>

#include <WindowServer/ScreenBackend.h>

struct gbm_device;
struct gbm_bo;

namespace WindowServer {

// Presents the WindowServer framebuffer on the host (Linux) port using Mesa GBM
// for the scanout buffers and EGL (EGL_PLATFORM_GBM_KHR) to bind the DRM
// device.
//
// The LibGfx compositor still renders into an ordinary memory buffer (exactly
// like VirtualScreenBackend / the legacy KMS presenter). On every flush the
// damaged (or whole) region is copied into the GBM scanout buffer and committed
// to the display with drmModePageFlip, so no SDL, X11 or Wayland is involved.
//
// EGL's role here is the platform binding to the DRM/GBM device
// (eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR) + eglInitialize +
// eglCreateContext + eglMakeCurrent). This makes it a real "GBM + EGL" backend
// and keeps the display device exclusive; the actual pixel work is done by the
// CPU painter today, but the EGL context is ready for a future OpenGL/ES
// rendering path.
//
// Selected with SERENITY_WS_BACKEND=gbm on the Linux GUI port.
class GbmScreenBackend final : public ScreenBackend {
public:
    GbmScreenBackend();
    virtual ~GbmScreenBackend() override;

    virtual ErrorOr<void> open() override;
    virtual void set_head_buffer(int index) override;
    virtual ErrorOr<void> flush_framebuffer_rects(int buffer_index, ReadonlySpan<FBRect> rects) override;
    virtual ErrorOr<void> unmap_framebuffer() override;
    virtual ErrorOr<void> map_framebuffer() override;
    virtual ErrorOr<void> flush_framebuffer() override;
    virtual ErrorOr<void> set_head_mode_setting(GraphicsHeadModeSetting) override;
    virtual ErrorOr<void> set_safe_head_mode_setting() override;
    virtual ErrorOr<GraphicsHeadModeSetting> get_head_mode_setting() override;

private:
    ErrorOr<void> open_drm_device();
    void shutdown_drm();
    ErrorOr<void> init_egl();
    void shutdown_egl();
    ErrorOr<void> create_scanout_buffers();
    void release_scanout_buffers();
    ErrorOr<void> set_crtc(uint32_t framebuffer_id);
    void flip(int buffer_index);
    void copy_to_scanout(int buffer_index, ReadonlySpan<FBRect> rects);

    static void on_page_flip(int, unsigned int, unsigned int, unsigned int, void* data);

    int m_drm_fd { -1 };
    struct gbm_device* m_gbm_device { nullptr };
    struct gbm_bo* m_scanout_bo[2] { nullptr, nullptr };
    void* m_scanout_map[2] { nullptr, nullptr };
    uint32_t m_scanout_fb[2] { 0, 0 };

    EGLDisplay m_egl_display { EGL_NO_DISPLAY };
    EGLContext m_egl_context { EGL_NO_CONTEXT };

    uint32_t m_connector_id { 0 };
    uint32_t m_crtc_id { 0 };
    drmModeModeInfo m_mode {};
    drmModeCrtc* m_saved_crtc { nullptr };
    bool m_crtc_active { false };

    int m_width { 0 };
    int m_height { 0 };
    uint32_t m_pitch { 0 };
    int m_active_buffer { 0 };

    bool m_flip_pending { false };
    bool m_flip_done { false };
};

}
