/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles for the embedded wlroots compositor. The compositor owns the
// real screen (KMS/DRM) and runs a single wlroots scene graph into which BOTH
// the native Serenity desktop and foreign (X11/Wayland) clients are added as
// equal scene nodes — so they can be freely stacked.
typedef struct wswlr_compositor wswlr_compositor;
typedef struct wswlr_desktop_surface wswlr_desktop_surface;

// kind is "xdg" or "xwayland". Fired when a foreign client maps a toplevel so
// WindowServer can track it in its own window list / taskbar.
//
// flags carries the window's *role*, so the desktop can map it onto Serenity's
// own WindowType instead of the bridge silently dropping it:
//   WSWLR_FOREIGN_OVERRIDE_REDIRECT — X11 menu/tooltip/toolbar (no WM frame,
//       transient to its parent; must not appear in the taskbar);
//   WSWLR_FOREIGN_DIALOG            — has a parent (TRANSIENT_FOR / xdg parent);
//   WSWLR_FOREIGN_POPUP             — xdg_popup (menu/dropdown), never top-level.
enum {
    WSWLR_FOREIGN_OVERRIDE_REDIRECT = 1 << 0,
    WSWLR_FOREIGN_DIALOG = 1 << 1,
    WSWLR_FOREIGN_POPUP = 1 << 2,
};

typedef void (*wswlr_on_new_foreign_fn)(void* surface, char const* kind,
    char const* title, char const* app_id, unsigned flags, void* user);
typedef void (*wswlr_on_destroy_foreign_fn)(void* surface, void* user);

// The embedded compositor owns libinput (it is the session's input consumer),
// so the native desktop no longer reads evdev itself: everything that does NOT
// hit a foreign surface is handed back through these callbacks. Coordinates are
// output-layout pixels; button/key codes are raw evdev codes, exactly what
// LinuxInput's translation tables already expect.
typedef struct {
    void (*pointer_motion_absolute)(double x, double y, void* user);
    void (*pointer_button)(uint32_t evdev_button, bool pressed, void* user);
    void (*pointer_axis)(double vertical, double horizontal, void* user);
    void (*keyboard_key)(uint32_t evdev_code, bool pressed, void* user);
} wswlr_input_callbacks;

typedef struct {
    wswlr_on_new_foreign_fn on_new_foreign;
    wswlr_on_destroy_foreign_fn on_destroy_foreign;
    void* user;
    wswlr_input_callbacks input;
} wswlr_callbacks;

// Creates the embedded wlroots compositor. width/height seed the headless
// backend (and act as a hint for KMS). When headless is false the KMS/DRM
// backend is auto-created and owns the real screen + input. xwayland enables
// X11 application support.
wswlr_compositor* wswlr_compositor_create(uint32_t width, uint32_t height,
    bool headless, bool xwayland, wswlr_callbacks const* cb);
void wswlr_compositor_destroy(wswlr_compositor*);
const char* wswlr_compositor_socket(wswlr_compositor*);

// Integrate wlroots's event loop into the host's main loop: watch event_fd for
// readability and call wswlr_compositor_dispatch() when it fires.
int wswlr_compositor_event_fd(wswlr_compositor*);
void wswlr_compositor_dispatch(wswlr_compositor*, int timeout_ms);

// A damaged rectangle in desktop-buffer coordinates (WindowServer's FBRect is
// C++-only; the bridge stays a pure C ABI).
typedef struct {
    int32_t x, y, width, height;
} wswlr_rect;

// The native Serenity desktop, fed as a server-side buffer (not a Wayland
// client). It sits at the bottom of the scene; foreign clients stack above it.
wswlr_desktop_surface* wswlr_compositor_create_desktop(wswlr_compositor*,
    uint32_t w, uint32_t h);
void wswlr_compositor_present_desktop(wswlr_compositor*, wswlr_desktop_surface*,
    uint32_t const* pixels, uint32_t pitch_bytes, uint32_t w, uint32_t h);
// Partial present: only `rects` changed since the last call. WindowServer paints
// just the damaged rects into its own framebuffer, so uploading the whole frame
// would both waste bandwidth and show stale halves — this keeps one complete
// wlr_buffer by replaying the previous frame's rects, then applies the new ones.
// buffer_index 是 WindowServer 的 framebuffer 半区（0/1）：两块必须各自演进，
// 只写脏区 —— 整幅拷贝会把另一半区的陈旧内容（黑块）搬过来。
void wswlr_compositor_present_desktop_rects(wswlr_compositor*, wswlr_desktop_surface*,
    int buffer_index, uint32_t const* pixels, uint32_t pitch_bytes, uint32_t w, uint32_t h,
    wswlr_rect const* rects, size_t rect_count);

void wswlr_compositor_resize_desktop(wswlr_compositor*, wswlr_desktop_surface*,
    uint32_t w, uint32_t h);

// ── 单层合成：每个原生窗口一块 ──────────────────────────────────────────────
// 一层合成的核心：WindowServer 不再把整屏合成到一张位图，而是把每个窗口
// 渲染到自己的 buffer，交给 wlroots 直接合成（与 labwc 的 scene 模型一致）。
// 这样每个窗口的更新只影响它自己那块，damage 天然局部化。
typedef struct wswlr_layer wswlr_layer;

wswlr_layer* wswlr_compositor_create_layer(wswlr_compositor*, int x, int y,
    uint32_t w, uint32_t h);
void wswlr_layer_present(wswlr_layer*, uint32_t const* pixels, uint32_t pitch_bytes,
    uint32_t w, uint32_t h);

// 绘制路径（窗口层专用）：直接拿到一块属于本层、当前没被渲染器读取的内存，
// 由调用方在里头画完再 commit。这样「窗口位图 → wlr_buffer」不再需要两次
// 全窗口 memcpy（大窗口每帧几 MB 的浪费），也不需要为掉帧保存影子副本。
// begin_paint 返回 NULL 表示两块缓冲都还在用：跳过这一帧，下一次 damage 会再画。
void* wswlr_layer_begin_paint(wswlr_layer*, uint32_t w, uint32_t h, uint32_t* stride_bytes);
void wswlr_layer_commit(wswlr_layer*, int x, int y);

void wswlr_layer_set_position(wswlr_layer*, int x, int y);
void wswlr_layer_set_enabled(wswlr_layer*, bool enabled);
void wswlr_layer_raise(wswlr_layer*);
void wswlr_layer_destroy(wswlr_layer*);

#ifdef __cplusplus
}
#endif
