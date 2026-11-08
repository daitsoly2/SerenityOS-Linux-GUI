/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * 裁减版 wlroots：只做 Wayland/Xwayland 协议服务端，不做任何合成。
 *
 * 这是「WS 是唯一合成层」架构的核心。与旧的 wswlr_* 桥的区别：
 *   - 不创建 wlr_backend / wlr_renderer / wlr_allocator
 *   - 不创建 wlr_scene / wlr_scene_output / wlr_output / wlr_output_layout
 *   - 不创建 wlr_cursor、不开 libinput（输入归 WindowServer 的 LinuxInput）
 *   - 不持有任何 wlr_buffer，不做 texture 上传，不碰 DRM
 *
 * 因此旧路径上的三处黑块来源（GLES 纹理缓存、damage ring + swapchain
 * buffer-age、被 alpha 误解释的像素）在这条路径上根本不存在。
 *
 * 客户端提交的像素通过 wlr_buffer_begin_data_ptr_access(READ) 以 CPU 指针
 * 交给上层（Serenity 的 GUI::Window），由 WindowServer 统一合成后经 KMS 直出。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wlroots_proto wlroots_proto;

struct wlroots_proto_callbacks {
    void* user;

    /* 新的 xdg_toplevel 出现（尚未 commit，尺寸可能为 0）。 */
    void (*toplevel_created)(void* user, void* surface, char const* title, char const* app_id);

    /*
     * surface 提交了新 buffer。pixels 指向 buffer 起始地址，stride 是字节跨度，
     * format 是 DRM_FORMAT_*。damage_* 是 buffer 坐标系下的脏矩形。
     * 这三个字段只在回调返回前有效（end_data_ptr_access 之后指针即失效）。
     */
    void (*toplevel_committed)(void* user, void* surface, void const* pixels,
        int width, int height, int stride, uint32_t format,
        int damage_x, int damage_y, int damage_w, int damage_h);

    void (*toplevel_title_changed)(void* user, void* surface, char const* title);
    void (*toplevel_mapped)(void* user, void* surface);
    void (*toplevel_unmapped)(void* user, void* surface);
    void (*toplevel_destroyed)(void* user, void* surface);

    /*
     * 任意 wl_surface 创建 / 销毁（含 Xwayland 的裸 surface、subsurface）。
     * id 是 Wayland object id，**per-client**；client_pid 来自
     * wl_client_get_credentials，用来把 Xwayland 的 id 空间和别的客户端分开。
     * 上层用它把 XWM 报的 WL_SURFACE_ID 配对到 wlr_surface。
     */
    void (*surface_created)(void* user, void* surface, uint32_t id, int32_t client_pid);
    void (*surface_destroyed)(void* user, void* surface);

    /*
     * xdg_popup（Firefox 菜单走这条）：wlroots 已经完成 FLIP/SLIDE 约束调整与
     * grab，这里只把"几何"转译成 Serenity 窗口。popup 是顶层 toplevel 的子窗口，
     * 上层按 toplevel_surface 找到父 HostedWindow，再按 (lx,ly) 偏移定位。
     * reposition 表示客户重排了菜单（子菜单长大），上层重新查询并挪窗口。
     */
    void (*popup_created)(void* user, void* popup, void* surface, void* toplevel_surface);
    void (*popup_destroyed)(void* user, void* popup);
    void (*popup_repositioned)(void* user, void* popup);
    /* Fired when wlroots emits xdg_popup.configure. At this point the final
     * FLIP/SLIDE-adjusted geometry is available in popup->scheduled.geometry;
     * clients like GTK ack the configure but do NOT attach a new buffer for a
     * pure position change, so no further commit arrives and the upper layer
     * must reposition the popup window here instead of waiting for one. */
    void (*popup_configured)(void* user, void* popup);

    /*
     * wl_subsurface（GTK/gedit 菜单走这条，旧桥 wayland_host.cpp:901 注释）：
     * 子 surface 的整块 buffer 就是菜单内容，没有 xdg role。上层按 parent_surface
     * 找到父 HostedWindow，按 (sub_x, sub_y) 偏移定位（坐标相对父 surface buffer，
     * 含 CSD；上层再减掉 geometry 偏移落到窗口内容区）。
     */
    void (*subsurface_created)(void* user, void* subsurface, void* surface, void* parent_surface,
        int sub_x, int sub_y);
    void (*subsurface_repositioned)(void* user, void* subsurface);
    void (*subsurface_destroyed)(void* user, void* subsurface);

    /*
     * 已获得焦点的 text_input 提交了新状态（光标矩形/surrounding text 等）。
     * 内置输入法要靠它把候选窗重新定位到光标下方。surface 非 NULL 表示
     * 该 surface 所属客户端当前持有文本输入焦点。
     */
    void (*text_input_state_changed)(void* user, void* surface);
};

/* socket_name 为空时用 wayland-serenity-0（与 serenitid 导出的名字一致）。
 * output_width/height 只用于 wl_output 的 geometry/mode 上报，不参与合成。
 * X11/Wine 兼容不在这里用 wlr_xwayland 重做，而是借鉴旧 WaylandHost 的
 * XwaylandBridge + XWM（已验证可用）另行接入；本文件只管原生 Wayland。 */
wlroots_proto* wlroots_proto_create(char const* socket_name,
    int output_width, int output_height,
    struct wlroots_proto_callbacks const* callbacks);
void wlroots_proto_destroy(wlroots_proto*);

int wlroots_proto_event_fd(wlroots_proto*);
void wlroots_proto_dispatch(wlroots_proto*, int timeout_ms);
void wlroots_proto_flush(wlroots_proto*);
char const* wlroots_proto_socket(wlroots_proto*);

/*
 * 输入映射：WindowServer 的 LinuxInput/ScreenInput 是唯一的事件源和焦点决策者，
 * 这里只把它的状态单向同步给 wl_seat（对外来客户端而言就是「合成器的 seat」）。
 */
void wlroots_proto_pointer_enter(wlroots_proto*, void* surface, double sx, double sy);
void wlroots_proto_pointer_motion(wlroots_proto*, double sx, double sy, uint32_t time_msec);
void wlroots_proto_pointer_button(wlroots_proto*, uint32_t button, bool pressed, uint32_t time_msec);
void wlroots_proto_pointer_axis(wlroots_proto*, double vertical, double horizontal, uint32_t time_msec);
void wlroots_proto_pointer_leave(wlroots_proto*);

void wlroots_proto_keyboard_enter(wlroots_proto*, void* surface);
void wlroots_proto_keyboard_leave(wlroots_proto*);
/* serenity_mods 是 LibGUI KeyEvent::modifiers() 的位掩码，用来同步 wl_keyboard.modifiers。 */
void wlroots_proto_keyboard_key(wlroots_proto*, uint32_t evdev_code, bool pressed,
    uint32_t time_msec, unsigned serenity_mods);

/* 窗口几何/状态由 WindowServer 决定后回灌给客户端。 */
void wlroots_proto_toplevel_set_size(wlroots_proto*, void* surface, int width, int height);
void wlroots_proto_toplevel_set_activated(wlroots_proto*, void* surface, bool activated);
void wlroots_proto_toplevel_close(wlroots_proto*, void* surface);
/* 相当于 Wayland 的 wl_surface.frame：告诉客户端「这一帧画完了，可以再提交」。 */
void wlroots_proto_surface_frame_done(wlroots_proto*, void* surface);

/*
 * 给一个「裸」wl_surface（没有 xdg_toplevel role，典型是 Xwayland 给某个 X
 * toplevel 建的那块 surface）挂 commit 转发：buffer 的 CPU 像素走
 * toplevel_committed 回调，和原生 xdg toplevel 共用同一条 GUI::Window 路径。
 *
 * 返回非 NULL 表示这个 surface 确实是裸 surface 且已挂好监听；返回 NULL
 * 表示它已经有 xdg toplevel role（上层据此拒绝错配）。幂等，重复调用不会
 * 挂第二个 commit listener；挂上时若 current buffer 已存在会立即补一帧，
 * 覆盖「X11 MapNotify 早于/晚于 wl_surface 提交」的两种竞态。
 */
void* wlroots_proto_watch_surface(wlroots_proto*, void* surface);
/* 补发一次当前已提交的 buffer（GUI::Window 建好之后调用，避免丢首帧）。 */
void wlroots_proto_surface_repaint(wlroots_proto*, void* surface);

/* ── xdg_popup 转译 API ─────────────────────────────────────────────────────
 * wlroots 内部已经把 positioner 的 anchor/gravity/constraint_adjustment 算好
 * （FLIP/SLIDE 避让），这里只负责"把算好的几何交给 Serenity 窗口"。 */
/* 把 popup 的可用约束框（顶层 toplevel surface 坐标系）交给 wlroots 做避让。 */
void wlroots_proto_popup_unconstrain(wlroots_proto*, void* popup, int x, int y, int w, int h);
/* 取 popup 在顶层 toplevel 窗口几何坐标系下的偏移 (lx,ly) 与尺寸 (w,h)。 */
void wlroots_proto_popup_geometry(wlroots_proto*, void* popup, int* lx, int* ly, int* w, int* h);
/* 取 popup 的"根 toplevel"的 wl_surface（上层据此找父 HostedWindow）。 */
void* wlroots_proto_popup_toplevel_surface(wlroots_proto*, void* popup);
/* 客户端调过 xdg_popup.grab 才算 grabbing（gedit）；Firefox 的 XUL 菜单不调。 */
int wlroots_proto_popup_is_grabbing(wlroots_proto*, void* popup);
/* 发 popup_done 并置 inert（等于旧桥 xdg_popup_resource_destroy）。 */
void wlroots_proto_popup_dismiss(wlroots_proto*, void* popup);

/* ── wl_subsurface 转译 API ──────────────────────────────────────────────── */
/* 取 subsurface 相对其父 surface 的当前偏移 (x,y)。 */
void wlroots_proto_subsurface_offset(wlroots_proto*, void* subsurface, int* x, int* y);
/* 取某 toplevel surface 的 window_geometry 偏移（CSD 阴影厚度），用于把
 * subsurface 的父-buffer 坐标换算到窗口内容区坐标。 */
void wlroots_proto_toplevel_geometry_offset(wlroots_proto*, void* surface, int* gx, int* gy);

/* ── 内置输入法（text-input-v3 前端）─────────────────────────────────────── *
 * 桥现在自己就是「输入法前端」：客户端通过 text_input_v3 声明要输文本，
 * 桥把按键交给引擎（serenityime socket → fcitx5 的 rime 等），再把上屏文本
 * 用 commit_string 回灌；候选窗由桥用 LibGUI 画（Serenity 外观）。
 *
 * 因此 input-method-v2（让 fcitx5 自己当 IM、自己弹候选窗）默认**不启用**，
 * 需要时用环境变量 SERENITY_IM_V2=1 打开做对照实验。
 */
/* 当前是否有客户端持有文本输入焦点。 */
int wlroots_proto_text_input_active(wlroots_proto*);
/* 焦点 text_input 报告的光标矩形（surface 坐标，可能全 0）。 */
void wlroots_proto_text_input_cursor_rect(wlroots_proto*, int* x, int* y, int* w, int* h);
/* 上屏：commit_string + done。 */
void wlroots_proto_text_input_commit(wlroots_proto*, char const* text);
void wlroots_proto_text_input_preedit(wlroots_proto*, char const* text, int cursor_begin, int cursor_end);
void wlroots_proto_text_input_delete(wlroots_proto*, unsigned before_length, unsigned after_length);
void wlroots_proto_text_input_done(wlroots_proto*);

#ifdef __cplusplus
}
#endif
