/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ports/LinuxGUI/GbmScreenBackend.h>

#include <AK/Debug.h>
#include <AK/Try.h>
#include <LibCore/System.h>

#include <gbm.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

namespace WindowServer {

// DRM cards we are willing to try, in order. The first one with a connected
// connector wins (a compute card ahead of the display card must not take us
// down).
static constexpr const char* g_drm_candidates[] {
    "/dev/dri/card0",
    "/dev/dri/card1",
    "/dev/dri/card2",
};

static EGLBoolean g_have_egl_platform_gbm = EGL_FALSE;
static PFNEGLGETPLATFORMDISPLAYEXTPROC g_egl_get_platform_display_ext = nullptr;

GbmScreenBackend::GbmScreenBackend() = default;

GbmScreenBackend::~GbmScreenBackend()
{
    // unmap_framebuffer() tears the whole stack down. It is called explicitly
    // on shutdown (and from Screen) because the destructor chain of the host
    // port trips a ref-count assert (long-standing teardown bug).
    MUST(unmap_framebuffer());
}

ErrorOr<void> GbmScreenBackend::open()
{
    m_can_device_flush_buffers = true;   // drmModePageFlip
    m_can_device_flush_entire_framebuffer = true;
    m_can_set_head_buffer = true;

    TRY(open_drm_device());
    TRY(init_egl());
    return {};
}

void GbmScreenBackend::set_head_buffer(int index)
{
    VERIFY(index == 0 || index == 1);
    m_active_buffer = index;
}

ErrorOr<void> GbmScreenBackend::flush_framebuffer_rects(int buffer_index, ReadonlySpan<FBRect> rects)
{
    copy_to_scanout(buffer_index, rects);
    flip(buffer_index);
    return {};
}

ErrorOr<void> GbmScreenBackend::unmap_framebuffer()
{
    release_scanout_buffers();

    if (m_crtc_active && m_drm_fd >= 0 && m_saved_crtc) {
        // Hand the display back to fbcon instead of resurrecting a possibly
        // dangling framebuffer (see LinuxScreenBackend::destroy_kms_presenter).
        drmModeSetCrtc(m_drm_fd, m_crtc_id, 0, 0, 0, nullptr, 0, nullptr);
        drmModeFreeCrtc(m_saved_crtc);
        m_saved_crtc = nullptr;
        m_crtc_active = false;
    }

    shutdown_egl();
    shutdown_drm();

    if (m_framebuffer) {
        free(m_framebuffer);
        m_framebuffer = nullptr;
        m_size_in_bytes = 0;
    }
    m_back_buffer_offset = 0;
    return {};
}

ErrorOr<void> GbmScreenBackend::map_framebuffer()
{
    VERIFY(m_width > 0 && m_height > 0);
    VERIFY(m_pitch > 0);

    m_size_in_bytes = static_cast<size_t>(m_pitch) * static_cast<size_t>(m_height) * 2;
    m_framebuffer = static_cast<Gfx::ARGB32*>(calloc(1, m_size_in_bytes));
    if (!m_framebuffer)
        return Error::from_errno(ENOMEM);

    // Both buffers live in one contiguous allocation, just like the virtual
    // backend: front at offset 0, back at offset m_size_in_bytes / 2.
    m_back_buffer_offset = m_size_in_bytes / 2;
    m_active_buffer = 0;

    TRY(create_scanout_buffers());

    if (!m_crtc_active) {
        TRY(set_crtc(m_scanout_fb[0]));
        m_crtc_active = true;
    }

    dbgln("GbmScreenBackend: scanout on {}x{} (pitch {})", m_width, m_height, m_pitch);
    return {};
}

ErrorOr<void> GbmScreenBackend::flush_framebuffer()
{
    copy_to_scanout(m_active_buffer, {});
    flip(m_active_buffer);
    return {};
}

ErrorOr<void> GbmScreenBackend::set_head_mode_setting(GraphicsHeadModeSetting mode_setting)
{
    if (mode_setting.horizontal_active <= 0 || mode_setting.vertical_active <= 0)
        return Error::from_string_literal("Invalid screen resolution");

    auto pitch = static_cast<uint32_t>(mode_setting.horizontal_active) * static_cast<uint32_t>(sizeof(Gfx::ARGB32));
    if (mode_setting.horizontal_stride != 0 && static_cast<uint32_t>(mode_setting.horizontal_stride) != pitch)
        return Error::from_string_literal("Unsupported pitch");

    m_width = mode_setting.horizontal_active;
    m_height = mode_setting.vertical_active;
    m_pitch = pitch;
    return {};
}

ErrorOr<GraphicsHeadModeSetting> GbmScreenBackend::get_head_mode_setting()
{
    return GraphicsHeadModeSetting {
        .horizontal_stride = static_cast<int>(m_pitch),
        .pixel_clock_in_khz = 0,
        .horizontal_active = m_width,
        .horizontal_front_porch_pixels = 0,
        .horizontal_sync_time_pixels = 0,
        .horizontal_blank_pixels = 0,
        .vertical_active = m_height,
        .vertical_front_porch_lines = 0,
        .vertical_sync_time_lines = 0,
        .vertical_blank_lines = 0,
        .horizontal_offset = 0,
        .vertical_offset = 0,
    };
}

ErrorOr<void> GbmScreenBackend::set_safe_head_mode_setting()
{
    // There is no "safe" mode we can program here: the display is already
    // running the mode we probed. Nothing to do.
    return {};
}

// ── DRM / GBM device ────────────────────────────────────────────────────────

ErrorOr<void> GbmScreenBackend::open_drm_device()
{
    drmModeRes* resources = nullptr;
    drmModeConnector* connector = nullptr;
    int fd = -1;

    for (auto const* path : g_drm_candidates) {
        fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        resources = drmModeGetResources(fd);
        if (!resources) {
            ::close(fd);
            fd = -1;
            continue;
        }
        for (int i = 0; i < resources->count_connectors && !connector; ++i) {
            auto* candidate = drmModeGetConnector(fd, resources->connectors[i]);
            if (!candidate)
                continue;
            if (candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes > 0)
                connector = candidate;
            else
                drmModeFreeConnector(candidate);
        }
        if (connector)
            break;
        drmModeFreeResources(resources);
        resources = nullptr;
        ::close(fd);
        fd = -1;
    }

    if (fd < 0)
        return Error::from_string_literal("no /dev/dri/card* with a connected connector");
    if (!resources)
        return Error::from_string_literal("drmModeGetResources failed");
    if (!connector)
        return Error::from_string_literal("no connected DRM connector on any /dev/dri/card*");

    // Take DRM master so modesets do not fail with EACCES (a leftover master
    // such as plymouthd would otherwise block us). Warn and continue if we
    // cannot: some seats already grant us master implicitly.
    if (drmSetMaster(fd) != 0)
        dbgln("GbmScreenBackend: drmSetMaster failed ({}), another DRM master may hold the device", strerror(errno));

    // Pick a CRTC: bound encoder's CRTC -> any bound encoder -> compatible free CRTC -> first CRTC.
    uint32_t crtc_id = 0;
    if (connector->encoder_id) {
        if (auto* encoder = drmModeGetEncoder(fd, connector->encoder_id)) {
            crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
    }
    for (int i = 0; i < connector->count_encoders && !crtc_id; ++i) {
        auto* encoder = drmModeGetEncoder(fd, connector->encoders[i]);
        if (!encoder)
            continue;
        if (encoder->crtc_id) {
            crtc_id = encoder->crtc_id;
        } else {
            for (int c = 0; c < resources->count_crtcs; ++c) {
                if (encoder->possible_crtcs & (1u << c)) {
                    crtc_id = resources->crtcs[c];
                    break;
                }
            }
        }
        drmModeFreeEncoder(encoder);
    }
    if (!crtc_id && resources->count_crtcs > 0)
        crtc_id = resources->crtcs[0];
    if (!crtc_id) {
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        ::close(fd);
        return Error::from_string_literal("no CRTC for the connected connector");
    }

    // Honour the driver's preferred mode (e.g. virtio-gpu's dynamic mode),
    // then an exact match, then the smallest covering mode, then the closest.
    drmModeModeInfo mode {};
    bool mode_found = false;
    for (int i = 0; i < connector->count_modes && !mode_found; ++i) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = connector->modes[i];
            mode_found = true;
        }
    }
    for (int i = 0; i < connector->count_modes && !mode_found; ++i) {
        if (connector->modes[i].hdisplay == m_width && connector->modes[i].vdisplay == m_height) {
            // m_width/m_height may still be 0 here (open() runs before the
            // mode setting is known); an exact match only matters once set.
            mode = connector->modes[i];
            mode_found = true;
        }
    }
    if (!mode_found) {
        int64_t best_area = -1;
        for (int i = 0; i < connector->count_modes; ++i) {
            auto const& candidate = connector->modes[i];
            if (m_width > 0 && (candidate.hdisplay < m_width || candidate.vdisplay < m_height))
                continue;
            int64_t area = static_cast<int64_t>(candidate.hdisplay) * candidate.vdisplay;
            if (best_area < 0 || area < best_area) {
                best_area = area;
                mode = candidate;
                mode_found = true;
            }
        }
    }
    if (!mode_found) {
        mode = connector->modes[0];
        mode_found = true;
    }

    m_saved_crtc = drmModeGetCrtc(fd, crtc_id);
    m_connector_id = connector->connector_id;
    m_crtc_id = crtc_id;
    m_mode = mode;

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);

    m_gbm_device = gbm_create_device(fd);
    if (!m_gbm_device) {
        ::close(fd);
        return Error::from_errno(errno);
    }

    m_drm_fd = fd;
    return {};
}

void GbmScreenBackend::shutdown_drm()
{
    if (m_drm_fd >= 0) {
        ::close(m_drm_fd);
        m_drm_fd = -1;
    }
    if (m_gbm_device) {
        gbm_device_destroy(m_gbm_device);
        m_gbm_device = nullptr;
    }
}

// ── EGL (EGL_PLATFORM_GBM_KHR) ─────────────────────────────────────────────

ErrorOr<void> GbmScreenBackend::init_egl()
{
    if (!g_egl_get_platform_display_ext) {
        g_egl_get_platform_display_ext = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (g_egl_get_platform_display_ext)
            g_have_egl_platform_gbm = EGL_TRUE;
    }

    if (!g_have_egl_platform_gbm)
        return Error::from_string_literal("EGL does not support EGL_PLATFORM_GBM_KHR");

    m_egl_display = g_egl_get_platform_display_ext(EGL_PLATFORM_GBM_KHR, m_gbm_device, nullptr);
    if (m_egl_display == EGL_NO_DISPLAY)
        return Error::from_errno(errno);

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_egl_display, &major, &minor)) {
        return Error::from_string_literal("eglInitialize failed");
    }

    EGLint config_attributes[] {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (!eglChooseConfig(m_egl_display, config_attributes, &config, 1, &config_count) || config_count == 0) {
        return Error::from_string_literal("eglChooseConfig failed (no ARGB8888 config)");
    }

    EGLint context_attributes[] {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    m_egl_context = eglCreateContext(m_egl_display, config, EGL_NO_CONTEXT, context_attributes);
    if (m_egl_context == EGL_NO_CONTEXT)
        return Error::from_string_literal("eglCreateContext failed");

    // Bind the context without a surface: this is purely the platform binding
    // to the GBM/DRM device. No GL rendering is performed by this backend yet.
    if (!eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_egl_context))
        return Error::from_string_literal("eglMakeCurrent failed");

    dbgln("GbmScreenBackend: EGL {} bound to GBM device", eglQueryString(m_egl_display, EGL_VERSION));
    return {};
}

void GbmScreenBackend::shutdown_egl()
{
    if (m_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(m_egl_display, m_egl_context);
            m_egl_context = EGL_NO_CONTEXT;
        }
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
    }
}

// ── Scanout buffers ─────────────────────────────────────────────────────────

ErrorOr<void> GbmScreenBackend::create_scanout_buffers()
{
    auto create_buffer = [&](int index) -> ErrorOr<void> {
        auto* bo = gbm_bo_create(m_gbm_device,
            static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
            GBM_FORMAT_ARGB8888,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!bo)
            return Error::from_errno(errno);

        uint32_t stride = 0;
        void* map_data = nullptr;
        void* map = gbm_bo_map(bo, 0, 0, static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
            GBM_BO_TRANSFER_READ_WRITE, &stride, &map_data);
        if (!map) {
            gbm_bo_destroy(bo);
            return Error::from_errno(errno);
        }

        uint32_t fb_id = 0;
        if (drmModeAddFB(m_drm_fd, static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
                24, 32, gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, &fb_id) < 0) {
            gbm_bo_unmap(bo, map_data);
            gbm_bo_destroy(bo);
            return Error::from_errno(errno);
        }

        m_scanout_bo[index] = bo;
        m_scanout_map[index] = map_data;
        m_scanout_fb[index] = fb_id;
        return {};
    };

    TRY(create_buffer(0));
    TRY(create_buffer(1));
    return {};
}

void GbmScreenBackend::release_scanout_buffers()
{
    for (int i = 0; i < 2; ++i) {
        if (m_scanout_map[i] && m_scanout_bo[i])
            gbm_bo_unmap(m_scanout_bo[i], m_scanout_map[i]);
        if (m_scanout_fb[i])
            drmModeRmFB(m_drm_fd, m_scanout_fb[i]);
        if (m_scanout_bo[i])
            gbm_bo_destroy(m_scanout_bo[i]);
        m_scanout_map[i] = nullptr;
        m_scanout_fb[i] = 0;
        m_scanout_bo[i] = nullptr;
    }
}

ErrorOr<void> GbmScreenBackend::set_crtc(uint32_t framebuffer_id)
{
    if (drmModeSetCrtc(m_drm_fd, m_crtc_id, framebuffer_id, 0, 0,
            &m_connector_id, 1, &m_mode) < 0) {
        return Error::from_errno(errno);
    }
    return {};
}

void GbmScreenBackend::on_page_flip(int, unsigned int, unsigned int, unsigned int, void* data)
{
    auto* state = static_cast<GbmScreenBackend*>(data);
    state->m_flip_done = true;
    state->m_flip_pending = false;
}

void GbmScreenBackend::flip(int buffer_index)
{
    VERIFY(buffer_index == 0 || buffer_index == 1);

    auto& fb_id = m_scanout_fb[buffer_index];

    if (m_flip_pending) {
        drmEventContext context {};
        context.version = 2;
        context.page_flip_handler = on_page_flip;
        pollfd pfd {};
        pfd.fd = m_drm_fd;
        pfd.events = POLLIN;
        while (m_flip_pending && poll(&pfd, 1, 16) > 0)
            drmHandleEvent(m_drm_fd, &context);
    }

    m_flip_done = false;
    if (drmModePageFlip(m_drm_fd, m_crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, this) < 0) {
        // Fall back to a blocking modeset if page-flip is not supported.
        drmModeSetCrtc(m_drm_fd, m_crtc_id, fb_id, 0, 0, &m_connector_id, 1, &m_mode);
        return;
    }
    m_flip_pending = true;

    drmEventContext context {};
    context.version = 2;
    context.page_flip_handler = on_page_flip;
    pollfd pfd {};
    pfd.fd = m_drm_fd;
    pfd.events = POLLIN;
    while (!m_flip_done && poll(&pfd, 1, 16) > 0)
        drmHandleEvent(m_drm_fd, &context);
}

void GbmScreenBackend::copy_to_scanout(int buffer_index, ReadonlySpan<FBRect> rects)
{
    VERIFY(m_framebuffer && m_scanout_map[buffer_index]);

    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* source = reinterpret_cast<const u8*>(m_framebuffer) + offset;
    auto* destination = reinterpret_cast<u8*>(m_scanout_map[buffer_index]);

    auto rows = m_height;
    auto row_bytes_limit = min(static_cast<size_t>(m_width) * sizeof(Gfx::ARGB32), static_cast<size_t>(m_pitch));

    auto copy_rect = [&](FBRect const& rect) {
        if (!rect.width || !rect.height)
            return;
        auto y0 = static_cast<int>(rect.y);
        auto y1 = static_cast<int>(rect.y + rect.height);
        if (y0 >= rows)
            return;
        if (y1 > rows)
            y1 = rows;
        auto x0 = min(static_cast<size_t>(rect.x) * sizeof(Gfx::ARGB32), row_bytes_limit);
        auto x1 = min((static_cast<size_t>(rect.x) + static_cast<size_t>(rect.width)) * sizeof(Gfx::ARGB32), row_bytes_limit);
        if (x1 <= x0)
            return;
        for (int y = y0; y < y1; ++y) {
            memcpy(destination + static_cast<size_t>(y) * gbm_bo_get_stride(m_scanout_bo[buffer_index]) + x0,
                source + static_cast<size_t>(y) * static_cast<size_t>(m_pitch) + x0, x1 - x0);
        }
    };

    if (rects.is_empty()) {
        // Whole frame: the compositor already rendered into m_framebuffer, so a
        // single memcpy per row is enough (the scanout buffer and the
        // framebuffer share the same layout/pitch).
        for (int y = 0; y < rows; ++y)
            memcpy(destination + static_cast<size_t>(y) * gbm_bo_get_stride(m_scanout_bo[buffer_index]),
                source + static_cast<size_t>(y) * static_cast<size_t>(m_pitch), row_bytes_limit);
        return;
    }

    for (auto const& rect : rects)
        copy_rect(rect);
}

}
