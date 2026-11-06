/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ports/LinuxGUI/LinuxScreenBackend.h>
#include <Ports/LinuxGUI/LinuxX11Presenter.h>

#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
#include "wlroots_backend.h"
#include <LibCore/Notifier.h>
#include <Ports/LinuxGUI/LinuxInput.h>

namespace {
// 外来窗口的「角色」由桥以 flags 上报，这里映射回 Serenity 自己的窗口概念：
// 普通 toplevel 进窗口列表/任务栏；override-redirect（X11 菜单/提示/工具条）
// 和 xdg_popup 是「附属窗口」，无框、跟随父窗口、不进任务栏。
constexpr unsigned FLAG_OVERRIDE_REDIRECT = 1 << 0;
constexpr unsigned FLAG_DIALOG = 1 << 1;
constexpr unsigned FLAG_POPUP = 1 << 2;

void wlr_on_new_foreign(void* surface, char const* kind, char const* title, char const* app_id, unsigned flags, void*)
{
    (void)surface;
    char const* role = "toplevel";
    if (flags & FLAG_OVERRIDE_REDIRECT)
        role = "override-redirect";
    else if (flags & FLAG_POPUP)
        role = "popup";
    else if (flags & FLAG_DIALOG)
        role = "dialog";
    dbgln("ws-wlr: foreign {} window '{}' ({}) role={} flags=0x{:x}",
        kind, title ? title : "(none)", app_id ? app_id : "(none)", role, flags);
}
void wlr_on_destroy_foreign(void*, void*) { }

// 合成器独占 libinput；命中不到外来 surface 的事件回流到这里，走与直连 libinput
// 完全相同的翻译（keymap / 修饰位 / 绝对坐标归一化），所以原生桌面观感不变。
void wlr_pointer_motion_absolute(double x, double y, void*)
{
    WindowServer::LinuxInput::feed_pointer_motion_absolute(x, y);
}
void wlr_pointer_button(uint32_t button, bool pressed, void*)
{
    WindowServer::LinuxInput::feed_pointer_button(button, pressed);
}
void wlr_pointer_axis(double vertical, double horizontal, void*)
{
    WindowServer::LinuxInput::feed_pointer_axis(vertical, horizontal);
}
void wlr_keyboard_key(uint32_t evdev_code, bool pressed, void*)
{
    WindowServer::LinuxInput::feed_keyboard_key(evdev_code, pressed);
}

static wswlr_input_callbacks const s_wlr_input {
    wlr_pointer_motion_absolute,
    wlr_pointer_button,
    wlr_pointer_axis,
    wlr_keyboard_key,
};
static wswlr_callbacks const s_wlr_callbacks { wlr_on_new_foreign, wlr_on_destroy_foreign, nullptr, s_wlr_input };
}
#endif
#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/Try.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibGfx/Color.h>
#include <SDL2/SDL.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// The kernel uapi defines this, but libdrm's public headers do not export it.
#ifndef DRM_EVENT_HOTPLUG
#    define DRM_EVENT_HOTPLUG 0x1
#endif

namespace WindowServer {

LinuxScreenBackend* LinuxScreenBackend::s_last_instance = nullptr;

LinuxScreenBackend::LinuxScreenBackend()
{
    s_last_instance = this;
}

Gfx::IntSize LinuxScreenBackend::probe_display_size(int requested_width, int requested_height)
{
    for (auto const* path : { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2" }) {
        int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        Gfx::IntSize size { -1, -1 };
        if (auto* resources = drmModeGetResources(fd)) {
            for (int i = 0; i < resources->count_connectors && size.width() < 0; ++i) {
                auto* connector = drmModeGetConnector(fd, resources->connectors[i]);
                if (!connector)
                    continue;
                if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
                    // Same rule as kms_probe(): the driver's "preferred" mode
                    // wins. On virtio-gpu that is the dynamic mode tracking the
                    // host window size, so we follow it instead of latching onto
                    // a static mode that happens to equal the requested size.
                    size = { -1, -1 };
                    for (int m = 0; m < connector->count_modes && size.width() < 0; ++m) {
                        if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
                            size = { connector->modes[m].hdisplay, connector->modes[m].vdisplay };
                    }
                    // No preferred mode: exact size, else the smallest mode
                    // that covers it, else the closest.
                    bool found = size.width() >= 0;
                    for (int m = 0; m < connector->count_modes && !found; ++m) {
                        if (connector->modes[m].hdisplay == requested_width
                            && connector->modes[m].vdisplay == requested_height) {
                            size = { requested_width, requested_height };
                            found = true;
                        }
                    }
                    // Smallest mode that still covers the requested size.
                    int64_t best = -1;
                    for (int m = 0; m < connector->count_modes && !found; ++m) {
                        auto const& candidate = connector->modes[m];
                        if (candidate.hdisplay < requested_width || candidate.vdisplay < requested_height)
                            continue;
                        int64_t area = static_cast<int64_t>(candidate.hdisplay) * candidate.vdisplay;
                        if (best < 0 || area < best) {
                            best = area;
                            size = { candidate.hdisplay, candidate.vdisplay };
                        }
                    }
                }
                drmModeFreeConnector(connector);
            }
            drmModeFreeResources(resources);
        }
        close(fd);
        if (size.width() > 0)
            return size;
    }
    return { -1, -1 };
}

LinuxScreenBackend* LinuxScreenBackend::last_instance()
{
    return s_last_instance;
}

LinuxScreenBackend::~LinuxScreenBackend()
{
    MUST(unmap_framebuffer());
}

struct LinuxScreenBackend::KmsPresenter {
    int fd { -1 };
    uint32_t connector_id { 0 };
    uint32_t crtc_id { 0 };
    drmModeModeInfo mode {};
    drmModeCrtc* saved_crtc { nullptr };
    bool probed { false };
    int display_width { 0 };
    int display_height { 0 };

    struct Buffer {
        uint32_t handle { 0 };
        uint32_t fb_id { 0 };
        void* map { nullptr };
        size_t size { 0 };
        uint32_t pitch { 0 };
    } buffers[2];

    bool flip_pending { false };
    bool flip_done { false };
    // Damage blitted into the buffer that is currently on screen but not yet
    // into the other one. With double buffering the buffer we flip to next was
    // presented two frames ago, so it is missing whatever changed in between;
    // those rects are replayed into it on the next present, while it is still
    // off-screen.
    //   pending = what the buffer written next is missing
    //   accum   = what the buffer written now received during this frame
    // At flip time accum becomes pending, so a frame with several flushes still
    // transfers everything the other buffer missed.
    // A horizontal pointer sweep across a toolbar emits one small rect per
    // button hover-in/out, easily 30+ per frame. 16 slots overflowed, forced
    // pending_full_sync and left the two scanout buffers a frame apart. 64 is
    // enough for realistic sweeps; overflow still falls back to a full copy.
    FBRect pending_sync[64] {};
    int pending_sync_count { 0 };
    FBRect accum_sync[64] {};
    int accum_sync_count { 0 };
    // Set when a frame had more damage than the lists above can hold: the next
    // present copies the whole frame instead.
    bool pending_full_sync { false };
    // Set while re-probing after a hotplug: the connection and the original
    // CRTC state must survive so we can put the display back on exit.
    bool keep_alive { false };
};

static void on_page_flip(int, unsigned, unsigned, unsigned, void* data)
{
    auto* state = static_cast<LinuxScreenBackend::KmsPresenter*>(data);
    state->flip_done = true;
    state->flip_pending = false;
}

// KMS is the default: SDL is only kept as a fallback for windowed development
// (running the port inside X11/Wayland) and for hosts without DRM. Setting
// SERENITY_PRESENTER=sdl forces the old path. The wlroots path owns DRM itself,
// so it must also opt out of the libdrm KMS presenter.
static bool presenter_should_use_drm()
{
    auto const* value = getenv("SERENITY_PRESENTER");
    if (value && (!strcmp(value, "sdl") || !strcmp(value, "x11") || !strcmp(value, "wlroots")))
        return false;
    return true;
}

// The desktop as one X client: lets the whole session run inside Xwayland (or a
// plain Xorg), with a real compositor owning the display.
static bool presenter_should_use_x11()
{
    auto const* value = getenv("SERENITY_PRESENTER");
    return value && !strcmp(value, "x11");
}

// WindowServer embeds wlroots and becomes the compositor itself: the native
// desktop and foreign X11/Wayland clients are all equal scene nodes.
static bool presenter_should_use_wlroots()
{
#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
    auto const* value = getenv("SERENITY_PRESENTER");
    return value && !strcmp(value, "wlroots");
#else
    return false;
#endif
}

ErrorOr<void> LinuxScreenBackend::open()
{
    m_can_device_flush_buffers = true;
    m_can_device_flush_entire_framebuffer = false;
    m_can_set_head_buffer = true;
    return {};
}

void LinuxScreenBackend::set_head_buffer(int index)
{
    VERIFY(index == 0 || index == 1);
    m_active_buffer = index;
}

ErrorOr<void> LinuxScreenBackend::set_safe_head_mode_setting()
{
    return {};
}

ErrorOr<void> LinuxScreenBackend::set_head_mode_setting(GraphicsHeadModeSetting mode_setting)
{
    if (mode_setting.horizontal_active <= 0 || mode_setting.vertical_active <= 0)
        return Error::from_string_literal("Invalid screen resolution");

    auto pitch = mode_setting.horizontal_active * static_cast<int>(sizeof(Gfx::ARGB32));
    if (mode_setting.horizontal_stride != 0 && mode_setting.horizontal_stride != pitch)
        return Error::from_string_literal("Unsupported pitch");

    m_width = mode_setting.horizontal_active;
    m_height = mode_setting.vertical_active;
    m_pitch = pitch;

    // The KMS mode may differ from what WindowServer asked for, and WindowServer
    // sizes its framebuffer from the values we leave here, so decide now.
    if (presenter_should_use_drm())
        TRY(kms_probe(m_width, m_height));
    return {};
}

ErrorOr<GraphicsHeadModeSetting> LinuxScreenBackend::get_head_mode_setting()
{
    return GraphicsHeadModeSetting {
        .horizontal_stride = m_pitch,
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

// Probe the display and decide the mode *before* the framebuffer is allocated:
// WindowServer sizes its buffer from m_width/m_height, so changing the
// resolution later would leave us writing past the allocation.
ErrorOr<void> LinuxScreenBackend::kms_probe(int requested_width, int requested_height)
{
    if (m_kms && m_kms->probed)
        return {};

    auto* state = m_kms ? m_kms : new KmsPresenter;
    m_kms = state;

    // Try every card until one of them has a connected connector. Stopping at
    // "the first card that opens" is not enough on a machine with a discrete
    // GPU ahead of the one the panel is wired to (e.g. a compute card with no
    // display outputs): the probe would fail and take the whole session down.
    char const* candidates[] = { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2" };
    drmModeRes* resources = nullptr;
    drmModeConnector* connector = nullptr;
    for (auto const* path : candidates) {
        state->fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (state->fd < 0)
            continue;
        resources = drmModeGetResources(state->fd);
        if (!resources) {
            ::close(state->fd);
            state->fd = -1;
            continue;
        }
        for (int i = 0; i < resources->count_connectors && !connector; ++i) {
            auto* candidate = drmModeGetConnector(state->fd, resources->connectors[i]);
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
        ::close(state->fd);
        state->fd = -1;
    }
    if (state->fd < 0)
        return Error::from_string_literal("no /dev/dri/card* available for KMS");
    if (!resources)
        return Error::from_string_literal("drmModeGetResources failed");
    if (!connector)
        return Error::from_string_literal("no connected DRM connector on any /dev/dri/card*");

    // Ask for the master role explicitly. The kernel normally hands it to the
    // first opener, but a userspace master that was already there (plymouthd
    // during boot) keeps it, and every modeset afterwards fails with EACCES
    // even though the card opened fine.
    if (drmSetMaster(state->fd) != 0)
        dbgln("LinuxScreenBackend: drmSetMaster failed ({}), another DRM master probably holds the device", strerror(errno));

    auto cleanup = [&](drmModeConnector* connector) {
        if (connector)
            drmModeFreeConnector(connector);
        if (resources)
            drmModeFreeResources(resources);
    };

    // Pick a CRTC for this connector.
    //
    // The naive version (follow connector->encoder_id, then look for an encoder
    // whose encoder_id equals it) only works when somebody already drove this
    // connector. After a compositor is stopped - GDM/GNOME leaving the VT, a
    // CRTTC disabled by an atomic commit - the connector is *unbound*:
    // encoder_id is 0 and no encoder claims it, so the probe gave up with
    // "no CRTC for the connected connector" and the desktop fell back to SDL.
    // The correct order is: bound encoder's CRTC -> any encoder this connector
    // can use that is already bound -> a free CRTC that encoder is compatible
    // with (drmModeEncoder::possible_crtcs bitmask).
    uint32_t crtc_id = 0;
    if (connector->encoder_id) {
        if (auto* encoder = drmModeGetEncoder(state->fd, connector->encoder_id)) {
            crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
    }
    for (int i = 0; i < connector->count_encoders && !crtc_id; ++i) {
        auto* encoder = drmModeGetEncoder(state->fd, connector->encoders[i]);
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
    // Some drivers leave possible_crtcs empty on every encoder; anything is
    // better than refusing to start, so fall back to the first CRTC.
    if (!crtc_id && resources->count_crtcs > 0)
        crtc_id = resources->crtcs[0];
    if (!crtc_id) {
        cleanup(connector);
        return Error::from_string_literal("no CRTC for the connected connector");
    }

    // A driver "preferred" mode wins above all else. On virtio-gpu this is the
    // dynamic mode that tracks the host window size (e.g. 1337x717), so
    // honouring it is what makes the desktop follow a GTK/QEMU window resize.
    // Without this the probe latches onto the first static mode that happens to
    // equal the requested size (1280x960) and never adapts.
    drmModeModeInfo mode {};
    bool mode_found = false;
    for (int i = 0; i < connector->count_modes && !mode_found; ++i) {
        auto const& candidate = connector->modes[i];
        if (candidate.type & DRM_MODE_TYPE_PREFERRED) {
            mode = candidate;
            mode_found = true;
        }
    }

    // No preferred mode: exact match wins; otherwise the smallest mode that
    // still covers the requested size; otherwise whatever is closest.
    // modes[0] is often 480x360 or 640x480 and must never be picked just
    // because it comes first.
    for (int i = 0; i < connector->count_modes && !mode_found; ++i) {
        auto const& candidate = connector->modes[i];
        if (candidate.hdisplay == requested_width && candidate.vdisplay == requested_height) {
            mode = candidate;
            mode_found = true;
            break;
        }
    }
    if (!mode_found) {
        int64_t best_area = -1;
        for (int i = 0; i < connector->count_modes; ++i) {
            auto const& candidate = connector->modes[i];
            if (candidate.hdisplay < requested_width || candidate.vdisplay < requested_height)
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
        int64_t best_delta = -1;
        for (int i = 0; i < connector->count_modes; ++i) {
            auto const& candidate = connector->modes[i];
            auto delta = static_cast<int64_t>(candidate.hdisplay) * candidate.vdisplay
                - static_cast<int64_t>(requested_width) * requested_height;
            if (delta < 0)
                delta = -delta;
            if (best_delta < 0 || delta < best_delta) {
                best_delta = delta;
                mode = candidate;
                mode_found = true;
            }
        }
    }

    state->saved_crtc = drmModeGetCrtc(state->fd, crtc_id);
    state->connector_id = connector->connector_id;
    state->crtc_id = crtc_id;
    state->mode = mode;
    state->probed = true;
    state->display_width = mode.hdisplay;
    state->display_height = mode.vdisplay;

    // Deliberately do NOT touch m_width/m_height here: WindowServer sizes its
    // framebuffer from those and expects them to match the layout it applied.
    // Reporting the real mode and letting the caller apply a layout change keeps
    // both sides consistent (see display_mode_size()).
    dbgln("LinuxScreenBackend: display mode {}x{} (WindowServer asked for {}x{})",
        state->display_width, state->display_height, requested_width, requested_height);

    cleanup(connector);
    return {};
}

ErrorOr<void> LinuxScreenBackend::create_kms_presenter()
{
    if (!m_kms || !m_kms->probed)
        return Error::from_string_literal("KMS presenter was not probed");

    auto& state = *m_kms;

    auto mode_width = static_cast<uint32_t>(state.display_width);
    auto mode_height = static_cast<uint32_t>(state.display_height);

    auto create_buffer = [&](KmsPresenter::Buffer& buffer) -> ErrorOr<void> {
        drm_mode_create_dumb create {};
        create.width = mode_width;
        create.height = mode_height;
        create.bpp = 32;
        if (drmIoctl(state.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
            return Error::from_errno(errno);

        buffer.handle = create.handle;
        buffer.pitch = create.pitch;
        buffer.size = create.size;

        if (drmModeAddFB(state.fd, mode_width, mode_height,
                24, 32, buffer.pitch, buffer.handle, &buffer.fb_id)
            < 0)
            return Error::from_errno(errno);

        drm_mode_map_dumb map_request {};
        map_request.handle = buffer.handle;
        if (drmIoctl(state.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) < 0)
            return Error::from_errno(errno);

        buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, state.fd, map_request.offset);
        if (buffer.map == MAP_FAILED) {
            buffer.map = nullptr;
            return Error::from_errno(errno);
        }
        return {};
    };

    TRY(create_buffer(state.buffers[0]));
    TRY(create_buffer(state.buffers[1]));

    if (drmModeSetCrtc(state.fd, state.crtc_id, state.buffers[0].fb_id, 0, 0,
            &state.connector_id, 1, &state.mode)
        < 0) {
        auto saved_errno = errno;
        // EACCES/EBUSY here means we are not the DRM master: some other
        // userspace client (plymouthd at boot, a display manager that has not
        // quit) still owns the device. Say so instead of leaving the caller
        // with a bare "Permission denied".
        if (saved_errno == EACCES || saved_errno == EBUSY)
            dbgln("LinuxScreenBackend: cannot take over the display ({}): another DRM master is holding it - quit plymouth / the display manager first", strerror(saved_errno));
        return Error::from_errno(saved_errno);
    }

    dbgln("LinuxScreenBackend: KMS presenter on {}x{}", state.display_width, state.display_height);
    return {};
}

static void release_kms_buffers(LinuxScreenBackend::KmsPresenter& state)
{
    for (auto& buffer : state.buffers) {
        if (buffer.map)
            munmap(buffer.map, buffer.size);
        if (buffer.fb_id)
            drmModeRmFB(state.fd, buffer.fb_id);
        if (buffer.handle) {
            drm_mode_destroy_dumb destroy {};
            destroy.handle = buffer.handle;
            drmIoctl(state.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        buffer = {};
    }
}

Gfx::IntSize LinuxScreenBackend::display_mode_size() const
{
    if (!m_kms || !m_kms->probed)
        return { -1, -1 };
    return Gfx::IntSize { m_kms->display_width, m_kms->display_height };
}

Optional<Gfx::IntSize> LinuxScreenBackend::take_display_change()
{
    if (!m_kms || m_kms->fd < 0)
        return {};

    pollfd pfd {};
    pfd.fd = m_kms->fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0)
        return {};

    drm_event event {};
    if (read(m_kms->fd, &event, sizeof(event)) != static_cast<ssize_t>(sizeof(event)))
        return {};
    if (event.type != DRM_EVENT_HOTPLUG)
        return {};

    // Ask kms_probe() for "whatever the driver now prefers" rather than the old
    // resolution: passing the stale size back made the probe re-select the very
    // mode we are trying to leave (the desktop never followed a resize). With
    // the preferred-mode rule in kms_probe() the requested size is effectively
    // ignored on virtio-gpu, but a sane fallback keeps non-virtio cards happy.
    auto requested_width = m_kms->display_width;
    auto requested_height = m_kms->display_height;

    // Keep the connection alive, drop the scanout buffers, and re-probe so the
    // new mode (and any resolution change) is picked up.
    m_kms->keep_alive = true;
    release_kms_buffers(*m_kms);
    m_kms->probed = false;

    if (kms_probe(requested_width, requested_height).is_error())
        return {};

    dbgln("LinuxScreenBackend: display changed, new mode {}x{}",
        m_kms->display_width, m_kms->display_height);
    return Gfx::IntSize { m_kms->display_width, m_kms->display_height };
}

void LinuxScreenBackend::destroy_kms_presenter()
{
    if (!m_kms)
        return;

    auto* state = m_kms;
    if (state->keep_alive) {
        // Mid-reprobe: scanout buffers are already gone, keep the rest.
        release_kms_buffers(*state);
        return;
    }

    if (state->saved_crtc) {
        // Do NOT resurrect the saved CRTC: its buffer_id may belong to a
        // framebuffer a previous session already destroyed, which leaves the
        // display scanning out a dangling FB (frozen image, and the next
        // drmModeSetCrtc fails). Disabling the CRTC hands the display back to
        // fbcon, which repaints a valid mode on its own.
        drmModeSetCrtc(state->fd, state->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
        drmModeFreeCrtc(state->saved_crtc);
    }

    release_kms_buffers(*state);
    if (state->fd >= 0)
        close(state->fd);
    delete state;
    m_kms = nullptr;
}

void LinuxScreenBackend::present_buffer_kms(int buffer_index)
{
    if (!m_kms || !m_framebuffer || !m_kms->buffers[0].map)
        return;

    auto& state = *m_kms;
    auto& buffer = state.buffers[buffer_index == 0 ? 0 : 1];
    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* source = reinterpret_cast<u8*>(m_framebuffer) + offset;

    // Never read past what WindowServer allocated, even if the mode changed
    // underneath us.
    auto rows = min(state.display_height, m_height);
    rows = min(rows, static_cast<int>(m_size_in_bytes / 2 / static_cast<size_t>(m_pitch)));
    auto row_bytes = min(static_cast<size_t>(m_width) * sizeof(Gfx::ARGB32), static_cast<size_t>(m_pitch));
    for (int y = 0; y < rows; ++y) {
        memcpy(reinterpret_cast<u8*>(buffer.map) + static_cast<size_t>(y) * buffer.pitch,
            source + static_cast<size_t>(y) * static_cast<size_t>(m_pitch), row_bytes);
    }

    // This buffer now holds the whole frame; the flip that follows carries
    // whatever the other buffer still misses.
    flip_kms(buffer_index);
}

// Copies just the damaged rects into the scanout buffer, then flips. The flip
// itself is always whole-screen - only the memcpy gets cheaper.
void LinuxScreenBackend::present_buffer_kms_rects(int buffer_index, ReadonlySpan<FBRect> rects)
{
    if (!m_kms || !m_framebuffer || !m_kms->buffers[0].map || rects.is_empty()) {
        present_buffer_kms(buffer_index);
        return;
    }

    auto& state = *m_kms;
    if (state.pending_full_sync) {
        // A previous frame had too much damage to track: pay for one full copy
        // (which resyncs both buffers) and go back to rects.
        state.pending_full_sync = false;
        state.pending_sync_count = 0;
        state.accum_sync_count = 0;
        present_buffer_kms(buffer_index);
        return;
    }

    auto& buffer = state.buffers[buffer_index == 0 ? 0 : 1];
    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* source = reinterpret_cast<u8*>(m_framebuffer) + offset;

    auto rows = min(state.display_height, m_height);
    rows = min(rows, static_cast<int>(m_size_in_bytes / 2 / static_cast<size_t>(m_pitch)));
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
            memcpy(reinterpret_cast<u8*>(buffer.map) + static_cast<size_t>(y) * buffer.pitch + x0,
                source + static_cast<size_t>(y) * static_cast<size_t>(m_pitch) + x0, x1 - x0);
        }
    };

    // Replay what this buffer missed while it sat in the background, then
    // apply this frame's damage. Both happen while it is off-screen.
    for (int i = 0; i < state.pending_sync_count; ++i)
        copy_rect(state.pending_sync[i]);
    for (auto const& rect : rects)
        copy_rect(rect);

    // Remember this frame's damage: the flip hands it to the other buffer.
    for (auto const& rect : rects) {
        if (!rect.width || !rect.height)
            continue;
        if (state.accum_sync_count == static_cast<int>(sizeof(state.accum_sync) / sizeof(state.accum_sync[0]))) {
            state.pending_full_sync = true;
            state.accum_sync_count = 0;
            state.pending_sync_count = 0;
            break;
        }
        state.accum_sync[state.accum_sync_count++] = rect;
    }

    flip_kms(buffer_index);
}

void LinuxScreenBackend::flip_kms(int buffer_index)
{
    auto& state = *m_kms;
    auto& buffer = state.buffers[buffer_index == 0 ? 0 : 1];

    if (state.flip_pending) {
        drmEventContext context {};
        context.version = 2;
        context.page_flip_handler = on_page_flip;
        pollfd pfd {};
        pfd.fd = state.fd;
        pfd.events = POLLIN;
        while (state.flip_pending && poll(&pfd, 1, 16) > 0)
            drmHandleEvent(state.fd, &context);
    }

    state.flip_done = false;
    if (drmModePageFlip(state.fd, state.crtc_id, buffer.fb_id, DRM_MODE_PAGE_FLIP_EVENT, &state) < 0) {
        drmModeSetCrtc(state.fd, state.crtc_id, buffer.fb_id, 0, 0, &state.connector_id, 1, &state.mode);
        return;
    }
    state.flip_pending = true;

    // The buffer we just wrote is on screen and complete; the other one now
    // misses everything this frame applied to it.
    state.pending_sync_count = state.accum_sync_count;
    for (int i = 0; i < state.accum_sync_count; ++i)
        state.pending_sync[i] = state.accum_sync[i];
    state.accum_sync_count = 0;

    drmEventContext context {};
    context.version = 2;
    context.page_flip_handler = on_page_flip;
    pollfd pfd {};
    pfd.fd = state.fd;
    pfd.events = POLLIN;
    while (!state.flip_done && poll(&pfd, 1, 16) > 0)
        drmHandleEvent(state.fd, &context);
}

#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
ErrorOr<void> LinuxScreenBackend::create_wlroots_presenter()
{
    bool headless = getenv("WSWLR_HEADLESS") && strcmp(getenv("WSWLR_HEADLESS"), "1") == 0;
    bool xwayland = getenv("WSWLR_XWAYLAND") && strcmp(getenv("WSWLR_XWAYLAND"), "1") == 0;
    m_wlr = wswlr_compositor_create((uint32_t)m_width, (uint32_t)m_height, headless, xwayland, &s_wlr_callbacks);
    if (!m_wlr)
        return Error::from_string_literal("wlroots compositor init failed");
    m_wlr_desktop = wswlr_compositor_create_desktop(m_wlr, (uint32_t)m_width, (uint32_t)m_height);
    int fd = wswlr_compositor_event_fd(m_wlr);
    m_wlr_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    m_wlr_notifier->on_activation = [this] {
        wswlr_compositor_dispatch(m_wlr, 0);
    };
    dbgln("ws-wlr: embedded compositor up, socket={} headless={} xwayland={}",
        wswlr_compositor_socket(m_wlr), headless, xwayland);
    return {};
}

void LinuxScreenBackend::destroy_wlroots_presenter()
{
    m_wlr_notifier = nullptr;
    if (m_wlr) {
        wswlr_compositor_destroy(m_wlr);
        m_wlr = nullptr;
        m_wlr_desktop = nullptr;
    }
}

void LinuxScreenBackend::present_buffer_wlroots_rects(int buffer_index, ReadonlySpan<FBRect> rects)
{
    if (!m_wlr || !m_wlr_desktop || !m_framebuffer) {
        return;
    }
    if (rects.is_empty()) {
        present_buffer_wlroots(buffer_index);
        return;
    }

    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* pixels = reinterpret_cast<u8*>(m_framebuffer) + offset;

    // FBRect is C++-only; hand the bridge a plain POD array.
    constexpr size_t max_rects = 32;
    wswlr_rect converted[max_rects];
    size_t count = 0;
    for (auto const& rect : rects) {
        if (count == max_rects) {
            // Too much damage to track cheaply: one full upload is simpler.
            present_buffer_wlroots(buffer_index);
            return;
        }
        if (rect.width == 0 || rect.height == 0)
            continue;
        converted[count++] = {
            static_cast<int32_t>(rect.x),
            static_cast<int32_t>(rect.y),
            static_cast<int32_t>(rect.width),
            static_cast<int32_t>(rect.height),
        };
    }
    if (count == 0)
        return;

    wswlr_compositor_present_desktop_rects(m_wlr, m_wlr_desktop, buffer_index,
        reinterpret_cast<uint32_t const*>(pixels), static_cast<uint32_t>(m_pitch),
        static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
        converted, count);
}

void LinuxScreenBackend::present_buffer_wlroots(int buffer_index)
{
    if (!m_wlr || !m_wlr_desktop || !m_framebuffer)
        return;
    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* pixels = reinterpret_cast<u8*>(m_framebuffer) + offset;
    wswlr_compositor_present_desktop(m_wlr, m_wlr_desktop,
        reinterpret_cast<u32 const*>(pixels), static_cast<u32>(m_pitch),
        static_cast<u32>(m_width), static_cast<u32>(m_height));
}

bool LinuxScreenBackend::supports_window_layers() const
{
    return m_wlr != nullptr;
}

void* LinuxScreenBackend::create_window_layer(Gfx::IntRect const& rect)
{
    if (!m_wlr)
        return nullptr;
    return wswlr_compositor_create_layer(m_wlr, rect.x(), rect.y(),
        static_cast<uint32_t>(rect.width()), static_cast<uint32_t>(rect.height()));
}

void* LinuxScreenBackend::begin_paint_window_layer(void* layer, Gfx::IntSize const& physical_size, size_t& stride_out)
{
    stride_out = 0;
    if (!m_wlr || !layer)
        return nullptr;
    uint32_t stride = 0;
    auto* pixels = wswlr_layer_begin_paint(static_cast<wswlr_layer*>(layer),
        static_cast<uint32_t>(physical_size.width()), static_cast<uint32_t>(physical_size.height()), &stride);
    stride_out = stride;
    return pixels;
}

void LinuxScreenBackend::commit_window_layer(void* layer, Gfx::IntPoint const& position)
{
    if (!m_wlr || !layer)
        return;
    wswlr_layer_commit(static_cast<wswlr_layer*>(layer), position.x(), position.y());
}

void LinuxScreenBackend::move_window_layer(void* layer, Gfx::IntPoint const& position)
{
    if (!m_wlr || !layer)
        return;
    wswlr_layer_set_position(static_cast<wswlr_layer*>(layer), position.x(), position.y());
}

void LinuxScreenBackend::set_window_layer_enabled(void* layer, bool enabled)
{
    if (!m_wlr || !layer)
        return;
    wswlr_layer_set_enabled(static_cast<wswlr_layer*>(layer), enabled);
}

void LinuxScreenBackend::raise_window_layer(void* layer)
{
    if (!m_wlr || !layer)
        return;
    wswlr_layer_raise(static_cast<wswlr_layer*>(layer));
}

void LinuxScreenBackend::destroy_window_layer(void* layer)
{
    if (!layer)
        return;
    wswlr_layer_destroy(static_cast<wswlr_layer*>(layer));
}

#else
// Built without wlroots: there is no compositor to hand a window surface to, so
// the screen framebuffer stays the only output path (the compositor checks
// supports_window_layers() and keeps painting windows into it).
bool LinuxScreenBackend::supports_window_layers() const { return false; }
void* LinuxScreenBackend::create_window_layer(Gfx::IntRect const&) { return nullptr; }
void* LinuxScreenBackend::begin_paint_window_layer(void*, Gfx::IntSize const&, size_t& stride_out)
{
    stride_out = 0;
    return nullptr;
}
void LinuxScreenBackend::commit_window_layer(void*, Gfx::IntPoint const&) { }
void LinuxScreenBackend::move_window_layer(void*, Gfx::IntPoint const&) { }
void LinuxScreenBackend::set_window_layer_enabled(void*, bool) { }
void LinuxScreenBackend::raise_window_layer(void*) { }
void LinuxScreenBackend::destroy_window_layer(void*) { }

#endif

ErrorOr<void> LinuxScreenBackend::create_presenter()
{
    if (presenter_should_use_drm()) {
        // A resolution change tears the presenter down and builds it again
        // (Screen::apply_layout closes and reopens the device), so probing must
        // be re-done here rather than assuming startup already did it.
        if (!m_kms || !m_kms->probed) {
            auto probe = kms_probe(m_width, m_height);
            if (probe.is_error())
                dbgln("LinuxScreenBackend: KMS probe failed ({}), falling back to SDL", probe.error());
        }
        if (m_kms && m_kms->probed) {
            auto result = create_kms_presenter();
            if (result.is_error())
                dbgln("LinuxScreenBackend: KMS presenter unavailable ({}), falling back to SDL", result.error());
            else
                return {};
        }
    }

#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
    if (presenter_should_use_wlroots()) {
        auto result = create_wlroots_presenter();
        if (result.is_error())
            dbgln("LinuxScreenBackend: wlroots presenter unavailable ({}), falling back to SDL", result.error());
        else
            return {};
    }
#endif

    if (m_window) {
        // Resolution change on an existing window: keep the window (the user
        // may have dragged it) and just rebuild the streaming texture.
        if (m_texture)
            SDL_DestroyTexture(m_texture);
        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
        if (!m_texture)
            return Error::from_string_literal("SDL_CreateTexture failed after resize");
        return {};
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        dbgln("LinuxScreenBackend: SDL_InitSubSystem failed: {}", SDL_GetError());
        return Error::from_string_literal("SDL video initialization failed");
    }
    m_video_initialized = true;

    m_window = SDL_CreateWindow("SerenityOS WindowServer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        m_width, m_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        dbgln("LinuxScreenBackend: SDL_CreateWindow failed: {}", SDL_GetError());
        return Error::from_string_literal("SDL_CreateWindow failed");
    }

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer)
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    if (!m_renderer) {
        dbgln("LinuxScreenBackend: SDL_CreateRenderer failed: {}", SDL_GetError());
        return Error::from_string_literal("SDL_CreateRenderer failed");
    }

    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
    if (!m_texture) {
        dbgln("LinuxScreenBackend: SDL_CreateTexture failed: {}", SDL_GetError());
        return Error::from_string_literal("SDL_CreateTexture failed");
    }

    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderPresent(m_renderer);
    return {};
}

void LinuxScreenBackend::destroy_presenter()
{
    destroy_kms_presenter();
#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
    destroy_wlroots_presenter();
#endif
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_video_initialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        m_video_initialized = false;
    }
}

ErrorOr<void> LinuxScreenBackend::map_framebuffer()
{
    VERIFY(m_width > 0 && m_height > 0);
    VERIFY(m_pitch > 0);

    m_size_in_bytes = static_cast<size_t>(m_pitch) * static_cast<size_t>(m_height) * 2;
    m_framebuffer = static_cast<Gfx::ARGB32*>(calloc(1, m_size_in_bytes));
    if (!m_framebuffer)
        return Error::from_errno(ENOMEM);

    // 诊断：未画过的区域用洋红标出来，和「真黑」区分开。
    if (getenv("SERENITY_FB_MAGENTA")) {
        auto* words = reinterpret_cast<uint32_t*>(m_framebuffer);
        for (size_t i = 0; i < m_size_in_bytes / 4; ++i)
            words[i] = 0xFFFF00FFu;
    }

    // The wlroots presenter has no page flip and no way to synchronise two
    // halves: WindowServer only paints damaged rects into the back half, so the
    // two halves drift apart and uploading one of them shows stale areas (the
    // black rectangles). Collapse both halves onto the same buffer instead —
    // then any rect we flush is part of the single, complete image we upload.
    // One backing image for both presenters. With a half-screen offset the
    // half WindowServer is not currently drawing into stays a frame behind;
    // any flush that touches it puts stale pixels on screen (buttons vanish
    // until hovered). wlroots already used 0 for exactly this reason.
    m_back_buffer_offset = 0;
    m_active_buffer = 0;

    return create_presenter();
}

ErrorOr<void> LinuxScreenBackend::unmap_framebuffer()
{
    destroy_presenter();
    free(m_framebuffer);
    m_framebuffer = nullptr;
    m_size_in_bytes = 0;
    return {};
}

void LinuxScreenBackend::present_buffer(int buffer_index)
{
#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
    if (presenter_should_use_wlroots()) {
        present_buffer_wlroots(buffer_index);
        return;
    }
#endif
    if (m_kms) {
        present_buffer_kms(buffer_index);
        return;
    }

    if (presenter_should_use_x11()) {
        if (!m_framebuffer)
            return;
        if (!WindowServer::X11Presenter::is_open() && !WindowServer::X11Presenter::open(m_width, m_height))
            return;
        auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
        auto* pixels = reinterpret_cast<u8*>(m_framebuffer) + offset;
        WindowServer::X11Presenter::present(pixels, m_pitch, m_width, m_height);
        return;
    }

    if (!m_texture || !m_framebuffer)
        return;

    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* pixels = reinterpret_cast<u8*>(m_framebuffer) + offset;

    SDL_UpdateTexture(m_texture, nullptr, pixels, m_pitch);
    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);
}

ErrorOr<void> LinuxScreenBackend::flush_framebuffer_rects(int buffer_index, ReadonlySpan<FBRect> rects)
{
    if (m_kms) {
        // Full copy: measured at ~1.3 ms per 1872x984 frame on this VM, which
        // is ~8% of a 60 fps budget. Damage-rect merging repeatedly produced
        // partially painted frames, so correctness wins here. The rect path
        // is still available via SERENITY_KMS_PARTIAL_PRESENT=1.
        static bool const partial_present = getenv("SERENITY_KMS_PARTIAL_PRESENT") != nullptr;
        if (partial_present)
            present_buffer_kms_rects(buffer_index, rects);
        else
            present_buffer_kms(buffer_index);
        return {};
    }
#if defined(SERENITY_HAVE_WLROOTS_PRESENTER)
    // 只上传脏区：WindowServer 只画了这些矩形，整幅拷贝既浪费带宽，
    // 又会把另一块半帧的旧内容带上来（闪烁）。桥里负责 pending 重放 +
    // 首帧全量，保证 wlr_buffer 始终是完整快照。
    if (presenter_should_use_wlroots()) {
        present_buffer_wlroots_rects(buffer_index, rects);
        return {};
    }
#endif
    if (!m_texture || !m_framebuffer)
        return {};

    auto offset = buffer_index == 0 ? 0 : m_back_buffer_offset;
    auto* base = reinterpret_cast<u8*>(m_framebuffer) + offset;

    for (auto const& rect : rects) {
        if (rect.width == 0 || rect.height == 0)
            continue;

        SDL_Rect dst {
            .x = static_cast<int>(rect.x),
            .y = static_cast<int>(rect.y),
            .w = static_cast<int>(rect.width),
            .h = static_cast<int>(rect.height),
        };
        auto* pixels = base + static_cast<size_t>(dst.y) * static_cast<size_t>(m_pitch)
            + static_cast<size_t>(dst.x) * sizeof(Gfx::ARGB32);
        SDL_UpdateTexture(m_texture, &dst, pixels, m_pitch);
    }

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);
    return {};
}

ErrorOr<void> LinuxScreenBackend::flush_framebuffer()
{
    present_buffer(m_active_buffer);
    return {};
}

void LinuxScreenBackend::restore_display()
{
    destroy_presenter();
}

ErrorOr<void> LinuxScreenBackend::dump_to_ppm(ByteString const& path)
{
    if (!m_framebuffer || m_width <= 0 || m_height <= 0)
        return Error::from_string_literal("No framebuffer to dump");

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));

    auto header = ByteString::formatted("P6\n{} {}\n255\n", m_width, m_height);
    TRY(file->write_until_depleted(header.bytes()));

    auto offset = m_active_buffer == 0 ? 0 : m_back_buffer_offset;
    auto* base = reinterpret_cast<u8*>(m_framebuffer) + offset;

    Vector<u8> row;
    TRY(row.try_resize(static_cast<size_t>(m_width) * 3));
    for (int y = 0; y < m_height; ++y) {
        auto* source = reinterpret_cast<u32 const*>(base + static_cast<size_t>(y) * static_cast<size_t>(m_pitch));
        for (int x = 0; x < m_width; ++x) {
            auto pixel = source[x];
            row[x * 3 + 0] = static_cast<u8>((pixel >> 16) & 0xff);
            row[x * 3 + 1] = static_cast<u8>((pixel >> 8) & 0xff);
            row[x * 3 + 2] = static_cast<u8>(pixel & 0xff);
        }
        TRY(file->write_until_depleted(row.span()));
    }

    return {};
}

}
