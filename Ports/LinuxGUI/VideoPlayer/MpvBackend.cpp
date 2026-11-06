/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MpvBackend.h"

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <LibGfx/Bitmap.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>

// Minimal, ABI-stable subset of libmpv's C API (mpv >= 0.38). We deliberately
// do not include <mpv/client.h> / <mpv/render.h>: the library is resolved at
// runtime with dlopen, so a missing/older libmpv degrades gracefully instead of
// turning the whole app into an unresolved-symbol error at startup.
namespace {

enum MpvFormat : int {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
    MPV_FORMAT_NODE = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP = 8,
    MPV_FORMAT_BYTE_ARRAY = 9,
};

enum MpvRenderParamType : int {
    MPV_RENDER_PARAM_INVALID = 0,
    MPV_RENDER_PARAM_API_TYPE = 1,
    MPV_RENDER_PARAM_OPENGL_INIT_PARAMS = 2,
    MPV_RENDER_PARAM_OPENGL_FBO = 3,
    MPV_RENDER_PARAM_FLIP_Y = 4,
    MPV_RENDER_PARAM_DEPTH = 5,
    MPV_RENDER_PARAM_ICC_PROFILE = 6,
    MPV_RENDER_PARAM_AMBIENT_LIGHT = 7,
    MPV_RENDER_PARAM_X11_DISPLAY = 8,
    MPV_RENDER_PARAM_WL_DISPLAY = 9,
    MPV_RENDER_PARAM_ADVANCED_CONTROL = 10,
    MPV_RENDER_PARAM_NEXT_FRAME_INFO = 11,
    MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME = 12,
    MPV_RENDER_PARAM_SKIP_RENDERING = 13,
    MPV_RENDER_PARAM_DRM_DISPLAY = 14,
    MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE = 15,
    MPV_RENDER_PARAM_DRM_DISPLAY_V2 = 16,
    MPV_RENDER_PARAM_SW_SIZE = 17,
    MPV_RENDER_PARAM_SW_FORMAT = 18,
    MPV_RENDER_PARAM_SW_STRIDE = 19,
    MPV_RENDER_PARAM_SW_POINTER = 20,
};

struct MpvRenderParam {
    MpvRenderParamType type;
    void* data;
};

// MPV_RENDER_API_TYPE_SW: renders into a plain memory surface, which is exactly
// what a LibGUI widget needs -- no GL/Vulkan context required (this VM only has
// llvmpipe / kms_swrast).
char const* const MPV_RENDER_API_TYPE_SW = "sw";
char const* const MPV_SW_PIXEL_FORMAT = "bgr0";

struct MpvApi {
    void* handle { nullptr };

    mpv_handle* (*create)() { nullptr };
    int (*initialize)(mpv_handle*) { nullptr };
    int (*set_option_string)(mpv_handle*, char const*, char const*) { nullptr };
    int (*set_property)(mpv_handle*, char const*, MpvFormat, void*) { nullptr };
    int (*command)(mpv_handle*, char const**) { nullptr };
    int (*get_property)(mpv_handle*, char const*, MpvFormat, void*) { nullptr };
    void (*free)(void*) { nullptr };
    void (*terminate_destroy)(mpv_handle*) { nullptr };
    char const* (*error_string)(int) { nullptr };

    int (*render_context_create)(mpv_render_context**, mpv_handle*, MpvRenderParam*) { nullptr };
    int (*render_context_render)(mpv_render_context*, MpvRenderParam*) { nullptr };
    void (*render_context_free)(mpv_render_context*) { nullptr };
    void (*render_context_set_update_callback)(mpv_render_context*, void (*)(void*), void*) { nullptr };

    bool valid() const
    {
        return handle && create && initialize && set_option_string && set_property && command
            && get_property && terminate_destroy && error_string
            && render_context_create && render_context_render
            && render_context_free && render_context_set_update_callback;
    }
};

MpvApi& mpv_api()
{
    static MpvApi api = [] {
        MpvApi a;
        static char const* candidates[] = {
            "libmpv.so.2",
            "libmpv.so",
            "/serenity/lib/libmpv.so.2",
        };
        for (auto const* candidate : candidates) {
            a.handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
            if (a.handle)
                break;
        }
        if (!a.handle) {
            warnln("VideoPlayer: libmpv not available: {}", dlerror());
            return a;
        }

#define RESOLVE(field, symbol) a.field = reinterpret_cast<decltype(a.field)>(dlsym(a.handle, symbol))
        RESOLVE(create, "mpv_create");
        RESOLVE(initialize, "mpv_initialize");
        RESOLVE(set_option_string, "mpv_set_option_string");
        RESOLVE(set_property, "mpv_set_property");
        RESOLVE(command, "mpv_command");
        RESOLVE(get_property, "mpv_get_property");
        RESOLVE(free, "mpv_free");
        RESOLVE(terminate_destroy, "mpv_terminate_destroy");
        RESOLVE(error_string, "mpv_error_string");
        RESOLVE(render_context_create, "mpv_render_context_create");
        RESOLVE(render_context_render, "mpv_render_context_render");
        RESOLVE(render_context_free, "mpv_render_context_free");
        RESOLVE(render_context_set_update_callback, "mpv_render_context_set_update_callback");
#undef RESOLVE
        if (!a.valid())
            warnln("VideoPlayer: libmpv is missing one or more required symbols");
        return a;
    }();
    return api;
}

ByteString error_text(int code)
{
    auto& api = mpv_api();
    if (code >= 0)
        return {};
    if (api.error_string)
        return ByteString { api.error_string(code) };
    return ByteString::formatted("libmpv error {}", code);
}

}

namespace VideoPlayer {

MpvBackend::~MpvBackend()
{
    auto& api = mpv_api();
    // The render context must be freed before the core is destroyed.
    if (m_render_context && api.render_context_free)
        api.render_context_free(m_render_context);
    if (m_mpv && api.terminate_destroy)
        api.terminate_destroy(m_mpv);
    m_render_context = nullptr;
    m_mpv = nullptr;
}

bool MpvBackend::is_available()
{
    return mpv_api().valid();
}

void MpvBackend::render_update_callback(void* context)
{
    // Called on mpv's own thread. Touching any mpv API from here is forbidden,
    // so we only flip an atomic and let the GUI event loop do the rendering.
    auto* backend = static_cast<MpvBackend*>(context);
    backend->m_frame_pending.store(true, std::memory_order_relaxed);
}

bool MpvBackend::initialize(ByteString& error_message)
{
    auto& api = mpv_api();
    if (!api.valid()) {
        error_message = "libmpv.so.2 could not be loaded (install the libmpv2 package)"sv;
        return false;
    }

    m_mpv = api.create();
    if (!m_mpv) {
        error_message = "mpv_create() failed"sv;
        return false;
    }

    // Options are advisory: an unknown option only produces -5 and is ignored,
    // so this stays compatible with a range of mpv releases.
    struct Option {
        char const* name;
        char const* value;
    };
    static constexpr Option options[] = {
        { "config", "no" },              // do not read the host user's mpv.conf
        { "vo", "libmpv" },              // required by the render API
        { "ao", "pulse" },               // same sound server as LibAudio on this port
        { "hwdec", "no" },               // no usable GPU in the VM
        { "video-timing-offset", "0" },  // do not block render() until display time
        { "osc", "no" },                 // no mpv on-screen controller
        { "osd-level", "0" },
        { "keep-open", "yes" },
        { "idle", "yes" },
        { "input-default-bindings", "no" },
        { "input-vo-keyboard", "no" },
        { "audio-display", "no" },
        { "msg-level", "all=warn" },
    };
    for (auto const& option : options)
        api.set_option_string(m_mpv, option.name, option.value);

    int result = api.initialize(m_mpv);
    if (result < 0) {
        error_message = ByteString::formatted("mpv_initialize(): {}", error_text(result));
        return false;
    }

    MpvRenderParam params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    result = api.render_context_create(&m_render_context, m_mpv, params);
    if (result < 0) {
        error_message = ByteString::formatted("software renderer unavailable: {}", error_text(result));
        return false;
    }
    api.render_context_set_update_callback(m_render_context, &MpvBackend::render_update_callback, this);
    return true;
}

bool MpvBackend::load_file(ByteString const& path, ByteString& error_message)
{
    auto& api = mpv_api();
    if (!m_mpv) {
        error_message = "libmpv is not initialized"sv;
        return false;
    }

    // mpv_command() takes a NULL-terminated array of C strings; using the array
    // form (instead of mpv_command_string) avoids quoting problems with paths
    // containing spaces or shell metacharacters.
    char const* arguments[] = { "loadfile", path.characters(), "replace", nullptr };
    int result = api.command(m_mpv, arguments);
    if (result < 0) {
        error_message = ByteString::formatted("could not open '{}': {}", path, error_text(result));
        return false;
    }

    m_has_media = true;
    set_paused(false);
    return true;
}

void MpvBackend::close_file()
{
    if (!m_mpv)
        return;
    auto& api = mpv_api();
    char const* arguments[] = { "stop", nullptr };
    api.command(m_mpv, arguments);
    m_has_media = false;
}

void MpvBackend::set_paused(bool paused)
{
    if (!m_mpv)
        return;
    // NOTE: mpv_set_option_string() only works before mpv_initialize(); after
    // that, options are properties, so pause/volume must be set with
    // mpv_set_property(). Using the option API here silently did nothing.
    int value = paused ? 1 : 0;
    mpv_api().set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &value);
}

bool MpvBackend::is_paused() const
{
    if (!m_mpv)
        return true;
    int paused = 1;
    if (mpv_api().get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0)
        return true;
    return paused != 0;
}

void MpvBackend::toggle_pause()
{
    set_paused(!is_paused());
}

void MpvBackend::seek_seconds(double seconds)
{
    if (!m_mpv)
        return;
    ByteString value = ByteString::formatted("{}", seconds);
    char const* arguments[] = { "seek", value.characters(), "absolute", nullptr };
    mpv_api().command(m_mpv, arguments);
}

void MpvBackend::set_volume(double percent)
{
    if (!m_mpv)
        return;
    double value = percent;
    mpv_api().set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &value);
}

double MpvBackend::duration() const
{
    if (!m_mpv)
        return 0;
    double value = 0;
    if (mpv_api().get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &value) < 0)
        return 0;
    return value;
}

double MpvBackend::position() const
{
    if (!m_mpv)
        return 0;
    double value = 0;
    if (mpv_api().get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &value) < 0)
        return 0;
    return value;
}

double MpvBackend::volume() const
{
    if (!m_mpv)
        return 100;
    double value = 100;
    if (mpv_api().get_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &value) < 0)
        return 100;
    return value;
}

Gfx::IntSize MpvBackend::video_size() const
{
    if (!m_mpv)
        return {};
    int64_t width = 0;
    int64_t height = 0;
    if (mpv_api().get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &width) < 0 || width <= 0)
        return {};
    if (mpv_api().get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &height) < 0 || height <= 0)
        return {};

    // Cap the software-rendering surface while preserving the aspect ratio.
    double scale = min(1.0, min(1280.0 / static_cast<double>(width), 720.0 / static_cast<double>(height)));
    int w = max(16, static_cast<int>(width * scale));
    int h = max(16, static_cast<int>(height * scale));
    return { w, h };
}

bool MpvBackend::ensure_surface(int width, int height)
{
    if (m_surface && width == m_surface_width && height == m_surface_height)
        return true;

    // Drop our reference before unmapping. The VideoFrameWidget may still hold
    // the previous bitmap; its destruction callback owns the mmap and will free
    // it when the widget releases that reference.
    m_surface_bitmap = nullptr;
    m_surface = nullptr;
    m_surface_size = 0;

    size_t stride = (static_cast<size_t>(width) * 4 + 63) & ~static_cast<size_t>(63);
    size_t bytes = stride * static_cast<size_t>(height);
    void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED)
        return false;

    auto bitmap = Gfx::Bitmap::create_wrapper(
        Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { width, height }, 1, stride, data,
        [data, bytes] { munmap(data, bytes); });
    if (bitmap.is_error()) {
        munmap(data, bytes);
        return false;
    }

    m_surface = data;
    m_surface_size = bytes;
    m_surface_stride = stride;
    m_surface_width = width;
    m_surface_height = height;
    m_surface_bitmap = bitmap.release_value();
    return true;
}

Gfx::Bitmap const* MpvBackend::render_frame(int width, int height)
{
    auto& api = mpv_api();
    if (!m_render_context)
        return nullptr;

    m_frame_pending.store(false, std::memory_order_relaxed);

    width = clamp(width, 16, 1920);
    height = clamp(height, 16, 1080);
    if (!ensure_surface(width, height))
        return nullptr;

    int size[2] = { width, height };
    int no_wait = 0;
    MpvRenderParam params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, &size[0] },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(MPV_SW_PIXEL_FORMAT) },
        { MPV_RENDER_PARAM_SW_STRIDE, &m_surface_stride },
        // SW_POINTER's data IS the buffer pointer (verified against mpv 0.40).
        { MPV_RENDER_PARAM_SW_POINTER, m_surface },
        // Never block the GUI event loop waiting for a frame's display time.
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &no_wait },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };

    int result = api.render_context_render(m_render_context, params);
    if (result < 0)
        return nullptr;
    return m_surface_bitmap.ptr();
}

}
