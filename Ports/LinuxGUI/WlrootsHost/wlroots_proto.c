/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * 裁减版 wlroots 协议服务端 —— 详见 wlroots_proto.h 顶部的说明。
 *
 * 这里刻意只保留「协议兼容」所需的最小集合：
 *   wl_display / wlr_compositor(renderer=NULL) / wlr_shm / xdg_shell /
 *   wlr_seat / data_device_manager / xdg_decoration
 *
 * 一行渲染、合成、DRM、输入采集的代码都没有。
 */

#define _GNU_SOURCE
#ifndef WLR_USE_UNSTABLE
#    define WLR_USE_UNSTABLE
#endif

#include "wlroots_proto.h"

#include <drm_fourcc.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct toplevel_node {
    /* 反查宿主协议对象。不要用 wl_container_of(node, p, toplevels)：node 指向
     * toplevel_node 起始，而 p->toplevels 是 wlroots_proto 里的 wl_list 哨兵，
     * 两者偏移量毫不相干，算出来的 p 是野指针（gedit 一 set_title 就 SIGSEGV）。 */
    struct wlroots_proto* proto;
    struct wlr_xdg_toplevel* toplevel;
    struct wlr_surface* surface;
    bool size_clamped;
    bool configure_sent;   /* 首帧 configure 只补发一次（见 on_surface_commit） */

    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener set_title;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener configure; /* xdg_surface->events.configure */

    struct wl_list link; // wlroots_proto.toplevels
};

/*
 * 所有 wl_surface 的登记表（含 Xwayland 的裸 surface 与 subsurface）。
 * Wayland object id 是 per-client 的，所以同时记 client_pid；上层把
 * XWM 报来的 WL_SURFACE_ID 与 (pid, id) 配对，不会和 GTK 客户端撞号。
 */
struct surface_registry_node {
    struct wlroots_proto* proto;
    struct wlr_surface* surface;
    uint32_t id;
    int32_t pid;
    struct wl_listener destroy;
    /* 每个 surface 都可能挂子 subsurface；这里监听 new_subsurface。 */
    struct wl_listener new_subsurface;
    struct wl_list link;
};

/*
 * xdg_surface 登记表：wlroots 0.20 里 popup 信号挂在「父 xdg_surface」的
 * events.new_popup 上，而不是 xdg_shell->events.new_popup（后者在本 build 不触发）。
 * 而且 wl_surface 创建早于 wlr_xdg_surface，所以在 on_new_surface 里通过
 * try_from_wlr_surface 拿 xdg_surface 永远是 NULL。正确做法是用
 * xdg_shell->events.new_surface 钩子（每个 xdg_surface 创建时触发），在拿到
 * wlr_xdg_surface 之后才挂 new_popup。这个载体随 xdg_surface 生命周期创建/销毁，
 * 避免 wl_list_empty(listener_list) 断言 abort。
 */
struct xdg_surface_node {
    struct wlroots_proto* proto;
    struct wlr_xdg_surface* xdg_surface;
    struct wl_listener new_popup;  /* xdg_surface->events.new_popup */
    struct wl_listener destroy;    /* xdg_surface->events.destroy */
    struct wl_list link;           /* wlroots_proto.xdg_surfaces */
};

/*
 * xdg_popup 节点：wlroots 已经完成了 FLIP/SLIDE 约束调整与 grab，这里只把它的
 * wl_surface 接进"裸 surface 帧转发"（整块 buffer 即菜单内容，无 CSD / 无
 * window_geometry），并把生命周期与 reposition 转给上层。
 */
struct popup_node {
    struct wlroots_proto* proto;
    struct wlr_xdg_popup* popup;
    struct wlr_surface* surface;
    struct wl_listener destroy;        /* popup->events.destroy */
    struct wl_listener reposition;     /* popup->events.reposition */
    struct wl_listener configure;      /* popup->base->events.configure */
    struct wl_listener commit;         /* surface->events.commit：首帧 configure 触发点 */
    bool configure_sent;              /* 首帧 configure 只发一次 */
    /* 约束框（顶层 surface 坐标系）。不能存完就立刻 unconstrain —— 那个函数
     * 末尾会自己调 wlr_xdg_surface_schedule_configure（已反汇编确认），而此处
     * initialized 仍为假，必崩。故先存起来，首次 commit 后统一生效。 */
    struct wlr_box constraint_box;
    bool has_constraint_box;
    struct wl_list link;
};

/*
 * wl_subsurface 节点（GTK/gedit 菜单走这条）。子 surface 的整块 buffer 是菜单
 * 内容；记录父 surface 与偏移，转发帧，并在位置变化时通知上层重定位。
 */
struct subsurface_node {
    struct wlroots_proto* proto;
    struct wlr_subsurface* subsurface;
    struct wlr_surface* surface;       /* 子 surface */
    struct wlr_surface* parent;        /* 父 surface */
    struct wl_listener destroy;        /* subsurface->events.destroy */
    struct wl_listener surface_destroy;/* 子 surface->events.destroy */
    struct wl_listener parent_commit;  /* 父 surface->events.commit（位置随父提交生效） */
    struct wl_list link;
};

/* forward decls（在 on_new_surface / on_new_toplevel 里用到） */
static void* popup_toplevel_surface(struct wlr_xdg_popup* popup);
static void on_new_popup(struct wl_listener* listener, void* data);
static void on_new_xdg_surface(struct wl_listener* listener, void* data);
static void on_new_subsurface(struct wl_listener* listener, void* data);
static void on_toplevel_configure(struct wl_listener* listener, void* data);

/*
 * 裸 surface（无 xdg_toplevel role）的帧转发节点。Xwayland 给每个 X
 * toplevel 建的就是这种 surface；它的 buffer 就是窗口内容，没有 CSD 阴影、
 * 没有 window_geometry、也没有 configure 状态，所以单独一个 commit handler，
 * 但复用同一个 toplevel_committed 回调进 GUI::Window。
 */
struct plain_surface_node {
    struct wlroots_proto* proto;
    struct wlr_surface* surface;
    bool logged_first_frame;
    bool logged_no_buffer;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_list link;
};

/* 手写 wl_output global 的绑定记录：surface 创建/输出绑定都要互发
 * wl_surface.enter（旧桥 send_surface_output_enter 的等价物）。 */
struct output_bind {
    struct wl_resource* resource;
    struct wl_listener destroy;
    struct wl_list link;
};

struct wlroots_proto {
    struct wl_display* display;
    struct wlr_compositor* compositor;
    struct wlr_shm* shm;
    struct wlr_xdg_shell* xdg_shell;
    struct wlr_seat* seat;
    /* 虚拟键盘只用来给 seat 提供 xkb keymap，不采集任何物理输入。 */
    struct wlr_keyboard* keyboard;
    struct wl_global* output_global;
    struct wl_list output_resources; /* output_bind */
    uint32_t last_keyboard_mods;
    uint32_t last_frame_tick;
    int output_width;
    int output_height;
    struct wlr_xdg_decoration_manager_v1* decoration;
    /* GTK/gedit 菜单走 wl_subsurface（旧桥 wayland_host.cpp:901 注释）。
     * 不创建这个 global，客户端就拿不到 wl_subcompositor，subsurface 路径
     * 永远触发不了 —— 之前漏了这行，导致 on_new_subsurface 对真机客户端是死代码。 */
    struct wlr_subcompositor* subcompositor;

    /* 客户端复制（set_selection）必须由 compositor 显式接受：
     * 不监听 request_set_selection 并调 wlr_seat_set_selection()，selection
     * 永远是空的 —— 所有应用的复制粘贴都会失效（wl-copy/wl-paste、Xwayland、
     * Firefox 自身都一样）。 */
    struct wl_listener request_set_selection;

    struct wl_listener new_toplevel;
    struct wl_listener new_decoration;
    struct wl_listener new_surface;
    struct wl_listener new_xdg_surface; /* xdg_shell->events.new_surface */
    /* 初始 configure 由 wlroots 用「延迟定时器 / idle」发出（一个帧节拍），比
     * sync 的 done 晚一拍。新 toplevel / popup 创建时置位，dispatch 里据此做一次
     * 有界 drain，把 configure 与 done 凑成同一批 flush，避免客户端
     * "never been configured"。详见 wlroots_proto_dispatch。 */
    bool configure_drain_pending;
    int drain_stall_count;
    struct wl_list toplevels;
    struct wl_list surfaces;       /* surface_registry_node */
    struct wl_list xdg_surfaces;   /* xdg_surface_node */
    struct wl_list plain_surfaces; /* plain_surface_node */
    struct wl_list popups;         /* popup_node */
    struct wl_list subsurfaces;    /* subsurface_node */

    /* 当前 wl_pointer 焦点 surface。协议上 enter 必须成对出现（enter 之后
     * 只能是 leave），对已聚焦的 surface 重复 enter 会让客户端把交叉事件
     * 重新走一遍 —— GTK 会据此重置按钮的按下状态，表现为「第一次点击按钮
     * 没反应」。这里记录焦点，重复 enter 降级为「只更新坐标」。 */
    void* pointer_focus;
    uint32_t last_pointer_time;

    struct wlroots_proto_callbacks cb;
    char socket[64];
};

static struct toplevel_node* node_for_surface(struct wlroots_proto* p, struct wlr_surface* surface)
{
    struct toplevel_node* node;
    wl_list_for_each(node, &p->toplevels, link)
    {
        if (node->surface == surface)
            return node;
    }
    return NULL;
}

static struct plain_surface_node* plain_node_for_surface(struct wlroots_proto* p,
    struct wlr_surface* surface)
{
    struct plain_surface_node* node;
    wl_list_for_each(node, &p->plain_surfaces, link)
    {
        if (node->surface == surface)
            return node;
    }
    return NULL;
}

/*
 * 裸 surface 的整块 buffer 就是窗口内容（Xwayland 不画 CSD 阴影、没有
 * window_geometry），所以这里不需要 xdg 那份 geometry 裁剪 / configured
 * 检查 / set_size 钳制，直接把 CPU 像素交给上层。
 */
static void emit_plain_frame(struct wlroots_proto* p, struct plain_surface_node* node)
{
    struct wlr_surface* surface = node->surface;
    struct wlr_surface_state* state = &surface->current;
    if (!state->buffer) {
        if (!node->logged_no_buffer) {
            node->logged_no_buffer = true;
            fprintf(stderr, "ws-proto: plain surface #%u has no buffer yet\n",
                wl_resource_get_id(surface->resource));
        }
        return;
    }

    void* data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(state->buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        fprintf(stderr, "ws-proto: plain surface #%u buffer not CPU-readable\n",
            wl_resource_get_id(surface->resource));
        return;
    }
    if (!node->logged_first_frame) {
        node->logged_first_frame = true;
        fprintf(stderr, "ws-proto: plain surface #%u first frame %dx%d stride=%zu fmt=0x%08x\n",
            wl_resource_get_id(surface->resource), state->buffer_width,
            state->buffer_height, stride, format);
    }

    int dx = 0, dy = 0, dw = 0, dh = 0;
    if (pixman_region32_not_empty(&state->buffer_damage)) {
        pixman_box32_t* extents = pixman_region32_extents(&state->buffer_damage);
        dx = extents->x1;
        dy = extents->y1;
        dw = extents->x2 - extents->x1;
        dh = extents->y2 - extents->y1;
    }

    int w = state->buffer_width;
    int h = state->buffer_height;
    if (w <= 0 || h <= 0) {
        wlr_buffer_end_data_ptr_access(state->buffer);
        return;
    }
    /* X11 客户端偶尔请求远大于屏幕的窗口；这里没有 xdg configure 可回灌，
     * 只把交给上层的位图尺寸钳住，避免为整块画面分配几百 MB。 */
    int max_w = p->output_width - 40;
    int max_h = p->output_height - 80;
    if (max_w < 320)
        max_w = 320;
    if (max_h < 240)
        max_h = 240;
    if (w > max_w)
        w = max_w;
    if (h > max_h)
        h = max_h;

    if (p->cb.toplevel_committed) {
        p->cb.toplevel_committed(p->cb.user, surface, data,
            w, h, (int)stride, format, dx, dy, dw, dh);
    }
    wlr_buffer_end_data_ptr_access(state->buffer);
}

static void on_plain_surface_commit(struct wl_listener* listener, void*)
{
    struct plain_surface_node* node = wl_container_of(listener, node, commit);
    emit_plain_frame(node->proto, node);
}

static void on_plain_surface_destroy(struct wl_listener* listener, void*)
{
    struct plain_surface_node* node = wl_container_of(listener, node, destroy);
    struct wlroots_proto* p = node->proto;
    wl_list_remove(&node->commit.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->link);
    if (p->cb.toplevel_destroyed)
        p->cb.toplevel_destroyed(p->cb.user, node->surface);
    free(node);
}

static void on_registry_surface_destroy(struct wl_listener* listener, void*)
{
    struct surface_registry_node* node = wl_container_of(listener, node, destroy);
    struct wlroots_proto* p = node->proto;
    /* 摘掉 new_subsurface listener（挂在 surface->events.new_subsurface）。
     * 否则 surface 销毁时 wlroots 断言 wl_list_empty(&...->listener_list)
     * 直接 abort（实测 xdg_subsurface 测试一退出就崩）。仅在连接过时才摘。 */
    if (node->new_subsurface.notify)
        wl_list_remove(&node->new_subsurface.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->link);
    if (p->cb.surface_destroyed)
        p->cb.surface_destroyed(p->cb.user, node->surface);
    free(node);
}

static void on_new_surface(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_surface);
    struct wlr_surface* surface = data;
    struct wl_resource* resource = surface->resource;
    if (!resource)
        return;

    struct surface_registry_node* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->proto = p;
    node->surface = surface;
    node->id = wl_resource_get_id(resource);
    node->pid = -1;
    struct wl_client* client = wl_resource_get_client(resource);
    if (client) {
        pid_t pid = 0;
        uid_t uid = 0;
        gid_t gid = 0;
        wl_client_get_credentials(client, &pid, &uid, &gid);
        node->pid = (int32_t)pid;
    }
    node->destroy.notify = on_registry_surface_destroy;
    wl_signal_add(&surface->events.destroy, &node->destroy);
    /* 每个 surface 都可能挂子 subsurface（GTK 菜单）；监听 new_subsurface。 */
    node->new_subsurface.notify = on_new_subsurface;
    wl_signal_add(&surface->events.new_subsurface, &node->new_subsurface);
    wl_list_insert(&p->surfaces, &node->link);

    fprintf(stderr, "ws-proto: new wl_surface #%u pid=%d\n", node->id, node->pid);

    /* 旧桥 compositor_create_surface 里的 send_surface_output_enter：
     * Xwayland 需要知道自己落在哪块 wl_output 上才会开始 present。 */
    struct output_bind* bind;
    wl_list_for_each(bind, &p->output_resources, link)
    {
        if (wl_resource_get_client(bind->resource) == client)
            wl_surface_send_enter(resource, bind->resource);
    }

    if (p->cb.surface_created)
        p->cb.surface_created(p->cb.user, surface, node->id, node->pid);
}

/*
 * 客户端「复制」：wlroots 把请求交给我们，只有调用 wlr_seat_set_selection()
 * 才算接受。缺失这一步时 wl_data_device 的 selection 恒为空。
 */
/* Xwayland 转发的 selection 用的是 X 原子名（STRING / UTF8_STRING / TEXT）。
 * 这类 source 由 Xwayland 自己在 X 内部管理；我们若把它塞进 Wayland seat，
 * 会在客户端销毁 source 时踩到堆破坏（实测 free(): invalid / invalid size，
 * host signal 6）。先只接受 Wayland 原生客户端的 selection。 */
static bool source_is_x11(struct wlr_data_source* source)
{
    if (!source || source->mime_types.size == 0)
        return false;
    char** types = (char**)source->mime_types.data;
    size_t const count = source->mime_types.size / sizeof(char*);
    bool has_wayland_mime = false;
    bool has_x11_mime = false;
    for (size_t i = 0; i < count; ++i) {
        if (!types[i])
            continue;
        /* Wayland 客户端一律用 MIME 类型（text/plain;charset=utf-8、image/png…）。
         * 只要出现任何一个 text/plain*，就当它是 Wayland 原生客户端 —— 不能误伤
         * wl-copy（它同时也会挂几个自定义类型）。 */
        if (strncmp(types[i], "text/plain", 10) == 0 || strcmp(types[i], "image/png") == 0)
            has_wayland_mime = true;
        if (strcmp(types[i], "STRING") == 0 || strcmp(types[i], "UTF8_STRING") == 0
            || strcmp(types[i], "TEXT") == 0 || strcmp(types[i], "COMPOUND_TEXT") == 0)
            has_x11_mime = true;
    }
    return has_x11_mime && !has_wayland_mime;
}

static void on_request_set_selection(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, request_set_selection);
    struct wlr_seat_request_set_selection_event* event = data;
    if (!p->seat || !event)
        return;
    if (source_is_x11(event->source))
        return;
    wlr_seat_set_selection(p->seat, event->source, event->serial);
}

static void on_surface_commit(struct wl_listener* listener, void*)
{
    struct toplevel_node* node = wl_container_of(listener, node, commit);
    struct wlroots_proto* p = node->proto;
    struct wlr_surface* surface = node->surface;
    struct wlr_surface_state* state = &surface->current;

    /* 客户端首次 commit 之后必须有一次 configure，否则它带 buffer 提交会被
     * wlroots 以 "xdg_surface has never been configured" 拒绝并断开。
     * 旧实现只在 set_activated（窗口被 WS 激活）时才间接触发 configure，
     * 于是**任何没被激活的 toplevel** 都会卡死 —— 实测 wl-copy / wl-paste
     * 这类不上屏的客户端必踩（"error 3: xdg_surface has never been configured"）。
     * 这里在首次 commit（initialized 已为真）时主动补一次。 */
    if (node->toplevel && node->toplevel->base
        && !node->toplevel->base->configured && !node->configure_sent) {
        node->configure_sent = true;
        wlr_xdg_surface_schedule_configure(node->toplevel->base);
        /* 这里**不要**动 configure_drain_pending：那是给「新建窗口的首次
         * configure」用的抑制窗口，在 commit 回调里再置位会让 flush 的 drain
         * 与 wlroots 的配置定时器互相等待，实测把进程带进堆破坏
         * （free(): invalid size）。正常 flush 即可。 */
    }

    if (!state->buffer)
        return;
    if (!node->toplevel || !node->toplevel->base)
        return;
    /* 客户端还没 configure 过：先尺寸，不发帧数据。 */
    if (!node->toplevel->base->configured)
        return;

    void* data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(state->buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
            &data, &format, &stride)) {
        return;
    }

    int dx = 0, dy = 0, dw = 0, dh = 0;
    if (pixman_region32_not_empty(&state->buffer_damage)) {
        pixman_box32_t* extents = pixman_region32_extents(&state->buffer_damage);
        dx = extents->x1;
        dy = extents->y1;
        dw = extents->x2 - extents->x1;
        dh = extents->y2 - extents->y1;
    }

    /*
     * 关键：buffer 是整块 CSD 画面（GTK 的阴影/圆角），window_geometry 才是
     * 窗口内容。必须按 geometry 裁掉外圈再交给 GUI::Window，否则：
     *   ① 窗口带一圈 26px 透明阴影；② 上层若把 buffer 尺寸回灌成
     *   configure 尺寸，GTK 每轮都会把阴影再加一遍，窗口无限扩大
     *   （实测 configure 每轮 +52px）。回调里给的 width/height 从此等于
     *   window geometry，绝不能再拿它去 wlr_xdg_toplevel_set_size。
     */
    struct wlr_xdg_surface* xdg = node->toplevel->base;
    struct wlr_box geom = xdg->current.geometry;
    int gx = geom.x;
    int gy = geom.y;
    int gw = geom.width;
    int gh = geom.height;
    int bw = state->buffer_width;
    int bh = state->buffer_height;
    if (gx < 0 || gy < 0 || gw <= 0 || gh <= 0 || gx >= bw || gy >= bh) {
        gx = 0;
        gy = 0;
        gw = bw;
        gh = bh;
    }
    if (gx + gw > bw)
        gw = bw - gx;
    if (gy + gh > bh)
        gh = bh - gy;
    if (gw <= 0 || gh <= 0) {
        wlr_buffer_end_data_ptr_access(state->buffer);
        return;
    }

    /*
     * 超大窗口保护：客户端可能恢复一个远大于屏幕的窗口（实测 gedit
     * 8596x8443）。只发一次固定尺寸的 configure —— 绝不能按后续 buffer
     * 尺寸反复 set_size，那正是之前每轮 +52px 无限扩大的原因。
     * 同时把交给上层的尺寸也钳住，避免为整块画面分配 ~290MB 位图。
     */
    int max_w = p->output_width - 40;
    int max_h = p->output_height - 80;
    if (max_w < 320)
        max_w = 320;
    if (max_h < 240)
        max_h = 240;
    if (!node->size_clamped) {
        node->size_clamped = true;
        if (gw > max_w || gh > max_h) {
            int target_w = p->output_width * 3 / 4;
            int target_h = p->output_height * 3 / 4;
            wlr_log(WLR_INFO, "ws-proto: clamp oversized toplevel %dx%d -> %dx%d",
                gw, gh, target_w, target_h);
            wlr_xdg_toplevel_set_size(node->toplevel, target_w, target_h);
        }
    }
    if (gw > max_w)
        gw = max_w;
    if (gh > max_h)
        gh = max_h;

    /* ARGB8888 / XRGB8888 都是 4 字节像素；指针偏移到 geometry 起点。 */
    uint8_t* visible = (uint8_t*)data + (size_t)gy * stride + (size_t)gx * 4;
    if (p->cb.toplevel_committed) {
        p->cb.toplevel_committed(p->cb.user, surface, visible,
            gw, gh, (int)stride, format,
            dx - gx, dy - gy, dw, dh);
    }

    wlr_buffer_end_data_ptr_access(state->buffer);
}

static void on_toplevel_destroy(struct wl_listener* listener, void*)
{
    struct toplevel_node* node = wl_container_of(listener, node, destroy);
    struct wlroots_proto* p = node->proto;

    if (p->cb.toplevel_destroyed)
        p->cb.toplevel_destroyed(p->cb.user, node->surface);

    wl_list_remove(&node->commit.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->set_title.link);
    wl_list_remove(&node->map.link);
    wl_list_remove(&node->unmap.link);
    wl_list_remove(&node->configure.link);
    wl_list_remove(&node->link);
    free(node);
}

static void on_toplevel_set_title(struct wl_listener* listener, void*)
{
    struct toplevel_node* node = wl_container_of(listener, node, set_title);
    struct wlroots_proto* p = node->proto;
    if (p->cb.toplevel_title_changed)
        p->cb.toplevel_title_changed(p->cb.user, node->surface, node->toplevel->title);
}

static void on_toplevel_map(struct wl_listener* listener, void*)
{
    struct toplevel_node* node = wl_container_of(listener, node, map);
    struct wlroots_proto* p = node->proto;
    if (p->cb.toplevel_mapped)
        p->cb.toplevel_mapped(p->cb.user, node->surface);
}

static void on_toplevel_unmap(struct wl_listener* listener, void*)
{
    struct toplevel_node* node = wl_container_of(listener, node, unmap);
    struct wlroots_proto* p = node->proto;
    if (p->cb.toplevel_unmapped)
        p->cb.toplevel_unmapped(p->cb.user, node->surface);
}

/* ── xdg-decoration ────────────────────────────────────────────────────────
 * 窗口边框/标题栏是 WS 的原生能力（GUI::Window 自动套复古框）。按「新桥只做
 * 转译」的原则，这里唯一要做的是告诉客户端「别自己画 CSD」：无条件回
 * SERVER_SIDE，并在客户端后续请求 CLIENT_SIDE 时再次拒绝。若不这么做，
 * GTK/Firefox 会自绘一条蓝标题栏叠在复古框里，出现双标题栏 + 缝隙。
 */
struct decoration_state {
    struct wl_listener request_mode;
    struct wl_listener destroy;
};

static void on_decoration_request_mode(struct wl_listener*, void* data)
{
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    wlr_xdg_toplevel_decoration_v1_set_mode(deco,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void on_decoration_destroy(struct wl_listener* listener, void*)
{
    struct decoration_state* state = wl_container_of(listener, state, destroy);
    wl_list_remove(&state->request_mode.link);
    wl_list_remove(&state->destroy.link);
    free(state);
}

static void on_new_decoration(struct wl_listener*, void* data)
{
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    wlr_xdg_toplevel_decoration_v1_set_mode(deco,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    struct decoration_state* state = calloc(1, sizeof(*state));
    if (!state)
        return;
    state->request_mode.notify = on_decoration_request_mode;
    wl_signal_add(&deco->events.request_mode, &state->request_mode);
    state->destroy.notify = on_decoration_destroy;
    wl_signal_add(&deco->events.destroy, &state->destroy);
}

static void on_new_toplevel(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_toplevel);
    struct wlr_xdg_toplevel* toplevel = data;
    struct wlr_xdg_surface* xdg_surface = toplevel->base;
    if (!xdg_surface || xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        return;

    struct toplevel_node* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    fprintf(stderr, "ws-proto: on_new_toplevel entered toplevel=%p base=%p role=%d\n",
        (void*)toplevel, (void*)xdg_surface, (int)xdg_surface->role);
    node->proto = p;
    node->toplevel = toplevel;
    node->surface = xdg_surface->surface;

    node->commit.notify = on_surface_commit;
    wl_signal_add(&node->surface->events.commit, &node->commit);
    node->destroy.notify = on_toplevel_destroy;
    wl_signal_add(&toplevel->events.destroy, &node->destroy);
    node->set_title.notify = on_toplevel_set_title;
    wl_signal_add(&toplevel->events.set_title, &node->set_title);
    /* wlroots 0.20 把 map/unmap 挂在 wlr_surface 上，不在 xdg_surface 上。 */
    node->map.notify = on_toplevel_map;
    wl_signal_add(&node->surface->events.map, &node->map);
    node->unmap.notify = on_toplevel_unmap;
    wl_signal_add(&node->surface->events.unmap, &node->unmap);
    node->configure.notify = on_toplevel_configure;
    wl_signal_add(&xdg_surface->events.configure, &node->configure);
    wl_list_insert(&p->toplevels, &node->link);

    if (p->cb.toplevel_created)
        p->cb.toplevel_created(p->cb.user, node->surface,
            toplevel->title ? toplevel->title : "", toplevel->app_id ? toplevel->app_id : "");

    /* 新 toplevel 需要首帧 configure；在 configure 真正发出前抑制 flush，
     * 让 sync 的 done 与 configure 同批到达（见 wlroots_proto_dispatch）。 */
    p->configure_drain_pending = true;
    p->drain_stall_count = 0;
}

/*
 * xdg_surface 的 configure 必须由合成器转发给客户端（wlroots 不会自动发）。
 * 不接这条，toplevel 永远收不到 configure（base->configured 一直 false），
 * 而 popup 也拿不到带几何的 xdg_popup.configure。两个角色共用同一个转发逻辑。
 */
static void on_toplevel_configure(struct wl_listener* listener, void* data)
{
    /* wlroots 在内部已经把 xdg_surface.configure 事件发给客户端；这里的
     * signal 只是通知合成层「配置已发出」，用于记录 serial / 调试。
     * 切勿再手动调 send_configure —— 0.20 没有 wlr_xdg_surface_configure，
     * 重复发送会让客户端 configure/ack 计数错乱。 */
    struct wlr_xdg_surface_configure* configure = data;
    struct toplevel_node* node = wl_container_of(listener, node, configure);
    (void)node;
    fprintf(stderr, "ws-proto: xdg_surface configure serial=%u\n", configure->serial);
    /* configure 已入写出缓冲：解除「done 与 configure 同批 flush」的抑制，
     * 下一次任意 dispatcher 的 flush 就会把两者一起发走（见 wlroots_proto_dispatch /
     * wlroots_proto_flush 对 configure_drain_pending 的判别）。 */
    if (node->proto)
        node->proto->configure_drain_pending = false;
}

/* ── xdg_popup ────────────────────────────────────────────────────────────
 * wlroots 已经把 positioner 的 anchor/gravity/constraint_adjustment 算好
 * （FLIP/SLIDE 避让），并把 grab 也管起来了。这里的工作只有三件：
 *   1. 给 popup 的 wl_surface 挂帧转发（整块 buffer 即菜单内容）；
 *   2. 把可用约束框交给 wlroots 做避让（unconstrain_from_box）；
 *   3. 把生命周期 / reposition 转给上层，由 Serenity 决定窗口位置与关闭。
 */

static void on_popup_configure(struct wl_listener* listener, void* data)
{
    /* 同上：wlroots 已自动发出 xdg_popup.configure（含位置），这里只观察。 */
    struct wlr_xdg_surface_configure* configure = data;
    struct popup_node* node = wl_container_of(listener, node, configure);
    fprintf(stderr, "ws-proto: xdg_popup configure serial=%u\n", configure->serial);
    if (node->proto)
        node->proto->configure_drain_pending = false;

    /* The configure carries wlroots' final, constraint-adjusted geometry in
     * popup->scheduled.geometry. Reposition the Serenity window NOW: GTK acks
     * this configure but never re-attaches a buffer just because the position
     * changed, so waiting for another commit leaves the popup pinned at (0,0)
     * on the parent window's top-left decoration. */
    if (node->proto && node->proto->cb.popup_configured && node->popup) {
        struct wlr_box const* geom = &node->popup->scheduled.geometry;
        fprintf(stderr, "ws-proto: xdg_popup configure geometry=%d,%d %dx%d\n",
            geom->x, geom->y, geom->width, geom->height);
        node->proto->cb.popup_configured(node->proto->cb.user, node->popup);
    }
}

static void on_popup_reposition(struct wl_listener* listener, void*)
{
    struct popup_node* node = wl_container_of(listener, node, reposition);
    if (node->proto->cb.popup_repositioned)
        node->proto->cb.popup_repositioned(node->proto->cb.user, node->popup);
}

static void on_popup_destroy(struct wl_listener* listener, void*)
{
    struct popup_node* node = wl_container_of(listener, node, destroy);
    struct wlroots_proto* p = node->proto;
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->reposition.link);
    wl_list_remove(&node->configure.link);
    wl_list_remove(&node->commit.link);
    wl_list_remove(&node->link);
    if (p->cb.popup_destroyed)
        p->cb.popup_destroyed(p->cb.user, node->popup);
    free(node);
}

/*
 * xdg_shell->events.new_surface：每个 wlr_xdg_surface 创建时触发（toplevel /
 * popup 各自的 xdg_surface 都会触发）。这是拿 wlr_xdg_surface 的正确时机 ——
 * 早了（on_new_surface，wl_surface 创建）xdg_surface 还没诞生，try_from 必返 NULL。
 * 在这里给「父 xdg_surface」挂 new_popup：popup 创建时它的父 xdg_surface 会发
 * new_popup，我们据此建 popup_node 并主动 schedule_configure（否则客户端报
 * "never been configured"）。popup 自身也是 xdg_surface，所以这里连 popup 的
 * xdg_surface 也挂（支持嵌套菜单）。
 */
static void on_xdg_surface_destroy(struct wl_listener* listener, void*)
{
    struct xdg_surface_node* node = wl_container_of(listener, node, destroy);
    /* new_popup 挂在 xdg_surface->events.new_popup 上，xdg_surface 销毁时
     * wlroots 要求它的 listener_list 为空，否则断言 abort。这里随 xdg_surface
     * 销毁一并摘掉。 */
    if (node->new_popup.notify)
        wl_list_remove(&node->new_popup.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->link);
    free(node);
}

static void on_new_xdg_surface(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_xdg_surface);
    struct wlr_xdg_surface* xdg = data;
    if (!xdg)
        return;
    struct xdg_surface_node* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->proto = p;
    node->xdg_surface = xdg;
    node->new_popup.notify = on_new_popup;
    wl_signal_add(&xdg->events.new_popup, &node->new_popup);
    node->destroy.notify = on_xdg_surface_destroy;
    wl_signal_add(&xdg->events.destroy, &node->destroy);
    wl_list_insert(&p->xdg_surfaces, &node->link);
    fprintf(stderr, "ws-proto: new xdg_surface role=%d surf=#%u\n",
        (int)xdg->role, wl_resource_get_id(xdg->surface->resource));
}

/*
 * popup 的首帧 configure 必须由合成器触发。但 wlroots 的
 * wlr_xdg_surface_schedule_configure 断言 surface->initialized，而该标志只在
 * popup 的 wl_surface 首次 commit 时（wlroots 用 wlr_surface_role 机制：
 * xdg_surface_role_commit 在 surface->events.commit 派发前把 initialized 置真）
 * 才为真。因此：
 *   - 不能在 on_new_popup 里直接调（那时还没 commit，断言必崩）；
 *   - 也不能在 new_popup 里挂 idle（下一轮 dispatch 客户端仍未 commit，
 *     initialized 仍假，idle 里一样崩）。
 * 正确做法：挂在 popup surface 的 commit 上。客户端先做一次无 buffer 的实化
 * commit（GTK/Firefox 的标准做法），首次 commit 之后 initialized 已真，这里
 * 再 schedule_configure；并把 done 与 configure 凑同批 flush（见 dispatch/flush）。
 */
static void popup_on_commit(struct wl_listener* listener, void*)
{
    struct popup_node* node = wl_container_of(listener, node, commit);
    if (!node->popup || !node->popup->base)
        return;
    if (node->configure_sent)
        return;
    /* initialized 由 wlroots 在 surface->events.commit 派发前的角色回调里置真，
     * 普通 commit listener 跑在它之后，这里一定已真；极端情况仍假则跳过本次。 */
    if (!node->popup->base->initialized)
        return;

    node->configure_sent = true;
    /* 两条路都恰好调度一次 configure：
     *  - 有约束框：unconstrain_from_box 内部末尾就会 schedule_configure；
     *  - 无约束框：直接 schedule_configure。
     * 两个都调会排出两个 configure（客户端收两次），所以必须二选一。 */
    if (node->has_constraint_box)
        wlr_xdg_popup_unconstrain_from_box(node->popup, &node->constraint_box);
    else
        wlr_xdg_surface_schedule_configure(node->popup->base);
    fprintf(stderr, "ws-proto: popup schedule_configure (on first commit) surf=#%u box=%s\n",
        wl_resource_get_id(node->surface->resource),
        node->has_constraint_box ? "yes" : "no");
    /* configure 走 wlroots 延迟定时器（下一轮 dispatch 才真正写出），抑制 flush
     * 让 sync 的 done 与 configure 同批到达（见 wlroots_proto_dispatch/flush）。 */
    struct wlroots_proto* p = node->proto;
    p->configure_drain_pending = true;
    p->drain_stall_count = 0;
}

static void on_new_popup(struct wl_listener* listener, void* data)
{
    /* listener 嵌在 xdg_surface_node 里（new_popup 挂在父 xdg_surface 的
     * events.new_popup 上），不是嵌在 wlroots_proto，别用错了 container_of。 */
    struct xdg_surface_node* carrier = wl_container_of(listener, carrier, new_popup);
    struct wlroots_proto* p = carrier->proto;
    struct wlr_xdg_popup* popup = data;
    fprintf(stderr, "ws-proto: on_new_popup entered popup=%p base=%p base_surf=%p seat=%p\n",
        (void*)popup, popup ? (void*)popup->base : NULL,
        (popup && popup->base) ? (void*)popup->base->surface : NULL,
        (popup && popup->base) ? (void*)popup->seat : NULL);
    if (!popup || !popup->base || !popup->base->surface)
        return;

    /* 幂等：一个 popup 只会触发一次父 xdg_surface 的 new_popup；这里再保险一次，
     * 防止重复建节点（重复建会让两条 popup 帧转发都挂上）。 */
    struct popup_node* existing;
    wl_list_for_each(existing, &p->popups, link)
        if (existing->popup == popup)
            return;

    struct wlr_surface* surface = popup->base->surface;

    /* 给 popup 的 wl_surface 挂帧转发：整块 buffer 即菜单内容，无 CSD、无
     * window_geometry，复用 plain_surface 的整块拷贝路径。已有 xdg toplevel
     * role 的 surface 会被 watch_surface 拒绝（这里不应发生）。 */
    wlroots_proto_watch_surface(p, surface);

    struct popup_node* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->proto = p;
    node->popup = popup;
    node->surface = surface;
    node->destroy.notify = on_popup_destroy;
    wl_signal_add(&popup->events.destroy, &node->destroy);
    node->reposition.notify = on_popup_reposition;
    wl_signal_add(&popup->events.reposition, &node->reposition);
    node->configure.notify = on_popup_configure;
    wl_signal_add(&popup->base->events.configure, &node->configure);
    node->commit.notify = popup_on_commit;
    wl_signal_add(&surface->events.commit, &node->commit);
    wl_list_insert(&p->popups, &node->link);

    /* 把 popup 的可用约束框（屏幕，在顶层 toplevel 的 surface 坐标系里）记下来，
     * 交给 wlroots 按客户端 positioner 的 constraint_adjustment 做 FLIP/SLIDE
     * 避让，结果经 xdg_popup.configure 回给客户端。上层 on_popup_created 里会用
     * 更精确的父窗口框覆盖一次。
     *
     * 关键：这里**不能**直接调 wlr_xdg_popup_unconstrain_from_box。它的实现末尾
     * 就是 wlr_xdg_surface_schedule_configure(popup->base)（反汇编 0x9f3d7 确认），
     * 而此刻 popup surface 还没被客户端 commit 过、initialized 仍是假，调用必触发
     * wlr_xdg_surface.c:168 的断言 abort。改为只记框，在 popup_on_commit 里生效。 */
    node->constraint_box = (struct wlr_box) { 0, 0, p->output_width, p->output_height };
    node->has_constraint_box = true;

    /* 合成器必须主动给 popup 发一次 configure。wlroots 不像 toplevel 那样在
     * set_activated 时自动 schedule_configure，popup 没有任何角色事件会触发它；
     * 不调，客户端在首帧提交时会报 "xdg_surface has never been configured"。
     * 但 wlr_xdg_surface_schedule_configure 断言 surface->initialized，而该标志
     * 只在 popup surface 首次 commit 后才置真，故不能在此处调。改为在
     * popup_on_commit（surface->events.commit）里，客户端首次 commit 之后触发，
     * 见 popup_on_commit。这里只挂好 listener，不做 schedule_configure。 */

    void* toplevel_surface = wlroots_proto_popup_toplevel_surface(p, popup);
    fprintf(stderr, "ws-proto: new xdg_popup surf=#%u parent_toplevel=#%u grab=%s\n",
        wl_resource_get_id(surface->resource),
        toplevel_surface ? wl_resource_get_id(((struct wlr_surface*)toplevel_surface)->resource) : 0,
        popup->seat ? "yes" : "no");

    if (p->cb.popup_created)
        p->cb.popup_created(p->cb.user, popup, surface, toplevel_surface);
}

/* ── wl_subsurface ─────────────────────────────────────────────────────────
 * GTK/gedit 的菜单走这条：子 surface 的整块 buffer 是菜单内容，没有 xdg role。
 * 旧桥（wayland_host.cpp:901）把这个当成独立 Popup 窗口处理，新桥同样如此。
 */

static void on_subsurface_parent_commit(struct wl_listener* listener, void*)
{
    struct subsurface_node* node = wl_container_of(listener, node, parent_commit);
    if (node->proto->cb.subsurface_repositioned)
        node->proto->cb.subsurface_repositioned(node->proto->cb.user, node->subsurface);
}

static void on_subsurface_surface_destroy(struct wl_listener* listener, void*)
{
    struct subsurface_node* node = wl_container_of(listener, node, surface_destroy);
    wl_list_remove(&node->surface_destroy.link);
    wl_list_remove(&node->parent_commit.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->link);
    if (node->proto->cb.subsurface_destroyed)
        node->proto->cb.subsurface_destroyed(node->proto->cb.user, node->subsurface);
    free(node);
}

static void on_subsurface_destroy(struct wl_listener* listener, void*)
{
    struct subsurface_node* node = wl_container_of(listener, node, destroy);
    struct wlroots_proto* p = node->proto;
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->surface_destroy.link);
    wl_list_remove(&node->parent_commit.link);
    wl_list_remove(&node->link);
    if (p->cb.subsurface_destroyed)
        p->cb.subsurface_destroyed(p->cb.user, node->subsurface);
    free(node);
}

static void on_new_subsurface(struct wl_listener* listener, void* data)
{
    struct surface_registry_node* parent_node = wl_container_of(listener, parent_node, new_subsurface);
    struct wlroots_proto* p = parent_node->proto;
    struct wlr_subsurface* sub = data;
    if (!sub || !sub->surface)
        return;
    struct wlr_surface* child = sub->surface;

    /* 给子 surface 挂帧转发（整块 buffer 即菜单内容）。 */
    wlroots_proto_watch_surface(p, child);

    struct subsurface_node* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->proto = p;
    node->subsurface = sub;
    node->surface = child;
    node->parent = parent_node->surface;
    node->destroy.notify = on_subsurface_destroy;
    wl_signal_add(&sub->events.destroy, &node->destroy);
    node->surface_destroy.notify = on_subsurface_surface_destroy;
    wl_signal_add(&child->events.destroy, &node->surface_destroy);
    node->parent_commit.notify = on_subsurface_parent_commit;
    wl_signal_add(&parent_node->surface->events.commit, &node->parent_commit);
    wl_list_insert(&p->subsurfaces, &node->link);

    fprintf(stderr, "ws-proto: new subsurface surf=#%u parent=#%u at %d,%d\n",
        wl_resource_get_id(child->resource),
        wl_resource_get_id(parent_node->surface->resource),
        sub->current.x, sub->current.y);

    if (p->cb.subsurface_created)
        p->cb.subsurface_created(p->cb.user, sub, child, parent_node->surface,
            sub->current.x, sub->current.y);
}

/* ── wl_output ─────────────────────────────────────────────────────────────
 * wlroots 的 wlr_output 绑定 backend/renderer，本路径刻意不创建它们，所以
 * 这里手写一个纯协议的 wl_output global：只发 geometry/mode/scale/done，
 * 不产生任何像素或帧调度。没有它 GTK 拿不到 GdkMonitor，
 * gdk_monitor_get_workarea() 会断言 GDK_IS_MONITOR 失败。
 */
static void output_release(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void on_output_bind_destroy(struct wl_listener* listener, void*)
{
    struct output_bind* bind = wl_container_of(listener, bind, destroy);
    wl_list_remove(&bind->link);
    free(bind);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void bind_output(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    struct wlroots_proto* p = data;
    struct wl_resource* resource = wl_resource_create(client, &wl_output_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, p, NULL);

    /* 96 DPI 的物理尺寸，只用于 GTK 计算 mm/缩放，不参与合成。 */
    int phys_w = p->output_width * 254 / 960;
    int phys_h = p->output_height * 254 / 960;
    wl_output_send_geometry(resource, 0, 0, phys_w, phys_h,
        WL_OUTPUT_SUBPIXEL_UNKNOWN, "Serenity", "Serenity Display",
        WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        p->output_width, p->output_height, 60000);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(resource, 1);
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(resource);

    struct output_bind* bind = calloc(1, sizeof(*bind));
    if (bind) {
        bind->resource = resource;
        bind->destroy.notify = on_output_bind_destroy;
        wl_resource_add_destroy_listener(resource, &bind->destroy);
        wl_list_insert(&p->output_resources, &bind->link);
    }

    /* wl_output 可能比 surface 晚绑定（Xwayland 启动顺序不固定），给这个
     * 客户端已经存在的 surface 补发 enter。 */
    struct surface_registry_node* snode;
    wl_list_for_each(snode, &p->surfaces, link)
    {
        if (snode->surface->resource
            && wl_resource_get_client(snode->surface->resource) == client)
            wl_surface_send_enter(snode->surface->resource, resource);
    }
}

/*
 * X11/Wine 兼容不在 wlroots 的 wlr_xwayland 上重做：旧 WaylandHost 的
 * XwaylandBridge + XWM 已验证可用，新桥直接复用那两个编译单元。这里只提供
 * 两个通用能力 —— 所有 wl_surface 的 (pid, id) 登记，以及给「裸」surface
 * 挂帧转发 —— 剩下「WindowInfo → GUI::Window」的翻译在上层 main.cpp。
 * 窗口框、标题栏、焦点、层叠、光标、合成全部由 WindowServer 原生负责。
 */
wlroots_proto* wlroots_proto_create(char const* socket_name,
    int output_width, int output_height,
    struct wlroots_proto_callbacks const* callbacks)
{
    struct wlroots_proto* p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    if (callbacks)
        p->cb = *callbacks;
    wl_list_init(&p->toplevels);
    wl_list_init(&p->surfaces);
    wl_list_init(&p->xdg_surfaces);
    wl_list_init(&p->plain_surfaces);
    wl_list_init(&p->popups);
    wl_list_init(&p->subsurfaces);
    wl_list_init(&p->output_resources);

    wlr_log_init(getenv("WSWLR_VERBOSE") ? WLR_DEBUG : WLR_ERROR, NULL);

    p->display = wl_display_create();
    if (!p->display)
        goto fail;

    /*
     * 关键：renderer 传 NULL。
     * 头文件明确 "The renderer may be NULL, in which case no textures are
     * created" —— 这样 wlroots 不会为任何客户端 buffer 建 GL 对象，
     * 也就没有纹理缓存/上传这一整条路径。
     */
    p->compositor = wlr_compositor_create(p->display, 6, NULL);
    if (!p->compositor)
        goto fail;

    /*
     * wl_shm 同样不需要 renderer。只声明我们真正能按 CPU 读的两种格式。
     * 注意 wlr_shm_create() 收的是 DRM_FORMAT_*（wlroots 内部统一用 DRM 格式码），
     * 不是 WL_SHM_FORMAT_* 枚举；传错会直接命中
     * "wlr_shm_create: Assertion `has_argb8888 && has_xrgb8888' failed"。
     */
    static uint32_t const shm_formats[] = {
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_XRGB8888,
    };
    p->shm = wlr_shm_create(p->display, 1, shm_formats,
        sizeof(shm_formats) / sizeof(shm_formats[0]));
    if (!p->shm)
        goto fail;

    p->output_width = output_width > 0 ? output_width : 1920;
    p->output_height = output_height > 0 ? output_height : 1080;
    p->output_global = wl_global_create(p->display, &wl_output_interface, 3, p, bind_output);
    if (!p->output_global)
        goto fail;

    p->xdg_shell = wlr_xdg_shell_create(p->display, 6);
    if (!p->xdg_shell)
        goto fail;

    p->seat = wlr_seat_create(p->display, "seat0");
    if (!p->seat)
        goto fail;

    /* 接受客户端的复制请求：wlroots 只把它当"请求"，不调 set_selection 就等于拒绝，
     * seat 的 selection 会恒为空（所有应用的复制粘贴都失效）。 */
    p->request_set_selection.notify = on_request_set_selection;
    wl_signal_add(&p->seat->events.request_set_selection, &p->request_set_selection);

    /*
     * 虚拟键盘：物理键鼠仍由 WindowServer 独占 libinput，这里只借
     * wlr_keyboard 生成 us xkb keymap 并交给 seat。keymap 是客户端把
     * evdev keycode 翻译成字符的唯一依据，缺了它 wl_keyboard.key 全无效。
     */
    static const struct wlr_keyboard_impl virtual_keyboard_impl = {
        .name = "serenity-virtual",
        .led_update = NULL,
    };
    p->keyboard = calloc(1, sizeof(*p->keyboard));
    if (!p->keyboard)
        goto fail;
    wlr_keyboard_init(p->keyboard, &virtual_keyboard_impl, "serenity-virtual");

    struct xkb_context* xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap* keymap = xkb_ctx
        ? xkb_keymap_new_from_names(xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS)
        : NULL;
    bool keymap_ok = keymap && wlr_keyboard_set_keymap(p->keyboard, keymap);
    if (keymap)
        xkb_keymap_unref(keymap);
    if (xkb_ctx)
        xkb_context_unref(xkb_ctx);
    if (!keymap_ok) {
        wlr_log(WLR_ERROR, "ws-proto: failed to build xkb keymap");
        goto fail;
    }
    wlr_keyboard_set_repeat_info(p->keyboard, 25, 600);
    wlr_seat_set_keyboard(p->seat, p->keyboard);
    wlr_seat_set_capabilities(p->seat,
        WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    wlr_data_device_manager_create(p->display);
    p->decoration = wlr_xdg_decoration_manager_v1_create(p->display);
    if (p->decoration) {
        p->new_decoration.notify = on_new_decoration;
        wl_signal_add(&p->decoration->events.new_toplevel_decoration, &p->new_decoration);
    }

    /* wl_subcompositor 必须显式创建，否则客户端拿不到 wl_subcompositor global，
     * GTK 的 subsurface 菜单路径走不通（见 struct 注释）。 */
    p->subcompositor = wlr_subcompositor_create(p->display);
    if (!p->subcompositor)
        goto fail;

    p->new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&p->xdg_shell->events.new_toplevel, &p->new_toplevel);

    /* xdg_popup（Firefox 菜单）信号挂在「父 xdg_surface->events.new_popup」上，
     * 不是 xdg_shell->events.new_popup（后者本 build 不触发）。我们用
     * xdg_shell->events.new_surface 钩子拿到每个 wlr_xdg_surface，再在
     * on_new_xdg_surface 里给它的 new_popup 接 on_new_popup。 */
    p->new_xdg_surface.notify = on_new_xdg_surface;
    wl_signal_add(&p->xdg_shell->events.new_surface, &p->new_xdg_surface);

    /* 所有 surface（含 Xwayland 的裸 wl_surface）登记后交给上层配对。 */
    p->new_surface.notify = on_new_surface;
    wl_signal_add(&p->compositor->events.new_surface, &p->new_surface);

    char const* requested = (socket_name && *socket_name) ? socket_name : "wayland-serenity-0";
    snprintf(p->socket, sizeof(p->socket), "%s", requested);
    if (wl_display_add_socket(p->display, p->socket) != 0) {
        char const* generated = wl_display_add_socket_auto(p->display);
        if (!generated)
            goto fail;
        snprintf(p->socket, sizeof(p->socket), "%s", generated);
    }
    setenv("WAYLAND_DISPLAY", p->socket, 1);

    wlr_log(WLR_INFO, "ws-proto: listening on %s (no renderer, no output)", p->socket);
    return p;

fail:
    wlroots_proto_destroy(p);
    return NULL;
}

void wlroots_proto_destroy(wlroots_proto* p)
{
    if (!p)
        return;
    if (p->display) {
        /* display 销毁会连带触发所有 surface 的 destroy 事件；此时上层可能
         * 已经退出，先把回调表清空，别再把事件投递回去。 */
        memset(&p->cb, 0, sizeof(p->cb));
        /* 让客户端先收到 disconnect，再销毁 display（否则它们会卡在 roundtrip）。 */
        wl_display_destroy_clients(p->display);
        wl_display_destroy(p->display);
    }
    if (p->keyboard) {
        wlr_keyboard_finish(p->keyboard);
        free(p->keyboard);
    }
    free(p);
}

int wlroots_proto_event_fd(wlroots_proto* p)
{
    if (!p || !p->display)
        return -1;
    return wl_event_loop_get_fd(wl_display_get_event_loop(p->display));
}

void wlroots_proto_dispatch(wlroots_proto* p, int timeout_ms)
{
    if (!p || !p->display)
        return;
    struct wl_event_loop* loop = wl_display_get_event_loop(p->display);
    /*
     * 关键竞态（实测抓包确认）：
     *   - libwayland-server 在收到 wl_display.sync 时【同步】回 wl_callback.done，
     *     一旦 flush 就写回客户端；
     *   - 但 xdg_surface / xdg_popup 的 configure 由 wlroots 用一个【延迟定时器】
     *     （一个帧节拍，本机实测约 18ms）发出，比 done 晚一拍；
     *   - 客户端 wl_display_roundtrip() 只在看到 done 时就返回，随后立刻提交带
     *     buffer 的 surface；此时 configure 还没到、客户端也没 ack，wlroots 于是
     *     报 "xdg_surface has never been configured"（toplevel / popup 同源）。
     *
     * 修复（不阻塞、不动 main.cpp 事件循环）：新 toplevel / popup 创建时把
     * configure_drain_pending 置位。本函数与 wlroots_proto_flush 在置位期间
     * 【一律不 flush 客户端输出】；等 wlroots 真正把 configure 入写出缓冲时，
     * on_*_configure 回调清掉该标志，下一次（pump 或 Notifier 的）flush 就会把
     * done 与 configure 作为【同一批】写回。客户端在 roundtrip 里同时拿到二者、
     * 先 ack configure 再提交 buffer，竞态消除。
     */
    wl_event_loop_dispatch(loop, timeout_ms);
    if (p->configure_drain_pending) {
        /* 安全网：若连续多轮仍未发出 configure（极少见），强制解除抑制并 flush，
         * 避免永久挂起；极端情况下可能退回原始竞态，但远比卡死好。 */
        if (++p->drain_stall_count > 8)
            p->configure_drain_pending = false;
        return; /* 暂不 flush，等 configure 就绪 */
    }
    p->drain_stall_count = 0;
    wl_display_flush_clients(p->display);
}

static uint32_t monotonic_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void wlroots_proto_flush(wlroots_proto* p)
{
    if (!p || !p->display)
        return;
    /*
     * 与 wlroots_proto_dispatch 一致：configure 尚未就绪时完全抑制 flush。
     * 8ms pump 会独立调用本函数，若在此处把 wl_display.sync 的 done 先
     * flush 出去（比 wlroots 的 configure 延迟定时器早一拍），客户端
     * roundtrip 看到 done 即返回并提交 buffer，configure 还没到就触发
     * "xdg_surface has never been configured"。置位期间一律不 flush，
     * 等 on_*_configure 清标志后由下一次 dispatch/flush 把 done 与
     * configure 同批发走。stall 安全网由 dispatch 负责（>8 轮强制清标志）。
     */
    if (p->configure_drain_pending)
        return;
    wl_display_flush_clients(p->display);

    /*
     * 帧回调必须由节拍器无条件驱动（旧桥 WAYLAND_ROADMAP §3.5 的
     * flush_frame_callbacks），绝不能只在「收到 buffer 之后」才发 done。
     * Xwayland map 窗口时是先 wl_surface.frame 再 attach/commit 的：如果
     * done 依赖首帧 buffer，就会互相等待 —— 客户端永远不提交，窗口永远
     * 空白。8ms pump 按 ~16ms 节拍对所有 surface 批量发 done。
     */
    uint32_t const now_ms = monotonic_msec();
    if (now_ms - p->last_frame_tick < 16)
        return;
    p->last_frame_tick = now_ms;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct surface_registry_node* node;
    wl_list_for_each(node, &p->surfaces, link)
        wlr_surface_send_frame_done(node->surface, &now);
}

char const* wlroots_proto_socket(wlroots_proto* p)
{
    return p ? p->socket : NULL;
}

/* ── 输入映射（WindowServer → wl_seat）──────────────────────────────────── */

void wlroots_proto_pointer_enter(wlroots_proto* p, void* surface, double sx, double sy)
{
    if (!p || !p->seat || !surface)
        return;
    /* 已聚焦同一个 surface：不再重申 enter（协议上必须 enter/leave 成对），
     * 只把坐标同步过去。 */
    if (p->pointer_focus == surface) {
        wlr_seat_pointer_notify_motion(p->seat, p->last_pointer_time, sx, sy);
        wlr_seat_pointer_notify_frame(p->seat);
        return;
    }
    p->pointer_focus = surface;
    wlr_seat_pointer_notify_enter(p->seat, surface, sx, sy);
    /* notify_enter 自带一次 frame；不要再补，否则一个 enter 两个 frame。 */
}

void wlroots_proto_pointer_motion(wlroots_proto* p, double sx, double sy, uint32_t time_msec)
{
    if (!p || !p->seat)
        return;
    p->last_pointer_time = time_msec;
    wlr_seat_pointer_notify_motion(p->seat, time_msec, sx, sy);
    wlr_seat_pointer_notify_frame(p->seat);
}

void wlroots_proto_pointer_button(wlroots_proto* p, uint32_t button, bool pressed, uint32_t time_msec)
{
    if (!p || !p->seat)
        return;
    wlr_seat_pointer_notify_button(p->seat, time_msec, button,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    /* 帧边界：wlr_seat 的 notify_motion/button/axis 都**不会**自动发 frame，
     * 必须显式补（头文件原文："Motion, button and axis events should all be
     * followed by a frame event"）。缺了 frame，客户端会把事件攒在批次里：
     * 实测 GTK3 表现为「按钮按下后不弹起、第一次点击不生效，第二次才响应」。 */
    wlr_seat_pointer_notify_frame(p->seat);
}

void wlroots_proto_pointer_axis(wlroots_proto* p, double vertical, double horizontal, uint32_t time_msec)
{
    if (!p || !p->seat)
        return;
    if (vertical != 0.0) {
        wlr_seat_pointer_notify_axis(p->seat, time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL,
            vertical, (int32_t)vertical, WL_POINTER_AXIS_SOURCE_WHEEL,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        wlr_seat_pointer_notify_frame(p->seat);
    }
    if (horizontal != 0.0) {
        wlr_seat_pointer_notify_axis(p->seat, time_msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            horizontal, (int32_t)horizontal, WL_POINTER_AXIS_SOURCE_WHEEL,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        wlr_seat_pointer_notify_frame(p->seat);
    }
}

void wlroots_proto_pointer_leave(wlroots_proto* p)
{
    if (!p || !p->seat)
        return;
    p->pointer_focus = NULL;
    wlr_seat_pointer_clear_focus(p->seat);
}

/*
 * Serenity 的修饰符位（LibGUI/Event.h）：Alt=1, Ctrl=2, Shift=4, Super=8。
 * xkbcommon 标准 modifier index：Shift=0, Lock=1, Control=2, Alt=3, Super=6
 * —— 与 wlr_keyboard_set_keymap() 里那份 us keymap 一致，也是旧
 * wayland_host.cpp 的 serenity_mods_to_xkb() 使用的位定义。
 */
static uint32_t serenity_mods_to_xkb_mask(unsigned mods)
{
    uint32_t mask = 0;
    if (mods & 0x04)
        mask |= 1u << 0; /* Shift */
    if (mods & 0x02)
        mask |= 1u << 2; /* Control */
    if (mods & 0x01)
        mask |= 1u << 3; /* Mod1 / Alt */
    if (mods & 0x08)
        mask |= 1u << 6; /* Mod4 / Super */
    return mask;
}

void wlroots_proto_keyboard_enter(wlroots_proto* p, void* surface)
{
    if (!p || !p->seat || !surface)
        return;
    struct wlr_keyboard_modifiers mods = {
        .depressed = p->last_keyboard_mods,
        .latched = 0,
        .locked = 0,
        .group = 0,
    };
    wlr_seat_keyboard_notify_enter(p->seat, surface, NULL, 0, &mods);
}

void wlroots_proto_keyboard_leave(wlroots_proto* p)
{
    if (p && p->seat)
        wlr_seat_keyboard_notify_clear_focus(p->seat);
}

void wlroots_proto_keyboard_key(wlroots_proto* p, uint32_t evdev_code, bool pressed,
    uint32_t time_msec, unsigned serenity_mods)
{
    if (!p || !p->seat || !p->keyboard)
        return;

    uint32_t mods = serenity_mods_to_xkb_mask(serenity_mods);
    if (mods != p->last_keyboard_mods) {
        struct wlr_keyboard_modifiers modifiers = {
            .depressed = mods,
            .latched = 0,
            .locked = 0,
            .group = 0,
        };
        wlr_seat_keyboard_notify_modifiers(p->seat, &modifiers);
        p->last_keyboard_mods = mods;
        p->keyboard->modifiers.depressed = mods;
    }

    wlr_seat_keyboard_notify_key(p->seat, time_msec, evdev_code,
        pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void wlroots_proto_toplevel_set_size(wlroots_proto* p, void* surface, int width, int height)
{
    if (!p || !surface || width <= 0 || height <= 0)
        return;
    struct toplevel_node* node = node_for_surface(p, surface);
    if (node && node->toplevel)
        wlr_xdg_toplevel_set_size(node->toplevel, (uint32_t)width, (uint32_t)height);
}

void wlroots_proto_toplevel_set_activated(wlroots_proto* p, void* surface, bool activated)
{
    if (!p || !surface)
        return;
    struct toplevel_node* node = node_for_surface(p, surface);
    if (node && node->toplevel)
        wlr_xdg_toplevel_set_activated(node->toplevel, activated);
}

void wlroots_proto_toplevel_close(wlroots_proto* p, void* surface)
{
    if (!p || !surface)
        return;
    struct toplevel_node* node = node_for_surface(p, surface);
    if (node && node->toplevel)
        wlr_xdg_toplevel_send_close(node->toplevel);
}

void wlroots_proto_surface_frame_done(wlroots_proto* p, void* surface)
{
    if (!p || !surface)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_send_frame_done(surface, &now);
}

void* wlroots_proto_watch_surface(wlroots_proto* p, void* surface_ptr)
{
    if (!p || !surface_ptr)
        return NULL;
    struct wlr_surface* surface = surface_ptr;

    /* 已有 xdg toplevel role → 不是 Xwayland 的裸 surface，拒绝错配。
     * 这也挡住 per-client id 撞号时把 GTK 窗口当成 X11 窗口。 */
    if (node_for_surface(p, surface))
        return NULL;

    if (plain_node_for_surface(p, surface))
        return surface; /* 幂等：重复配对不挂第二个 commit listener */

    struct plain_surface_node* node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    node->proto = p;
    node->surface = surface;
    node->commit.notify = on_plain_surface_commit;
    wl_signal_add(&surface->events.commit, &node->commit);
    node->destroy.notify = on_plain_surface_destroy;
    wl_signal_add(&surface->events.destroy, &node->destroy);
    wl_list_insert(&p->plain_surfaces, &node->link);
    return surface;
}

/* ── popup / subsurface 转译 API 实现 ─────────────────────────────────────── */

static void* popup_toplevel_surface(struct wlr_xdg_popup* popup)
{
    if (!popup)
        return NULL;
    struct wlr_surface* s = popup->parent;
    while (s) {
        struct wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(s);
        if (!xdg)
            break;
        if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
            return s;
        if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP)
            s = xdg->popup->parent;
        else
            break;
    }
    return NULL;
}

void* wlroots_proto_popup_toplevel_surface(wlroots_proto* p, void* popup_ptr)
{
    (void)p;
    return popup_toplevel_surface(popup_ptr);
}

void wlroots_proto_popup_geometry(wlroots_proto* p, void* popup_ptr, int* lx, int* ly, int* w, int* h)
{
    (void)p;
    struct wlr_xdg_popup* popup = popup_ptr;
    if (!popup || !lx || !ly || !w || !h) {
        if (lx) *lx = 0;
        if (ly) *ly = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    /* 位置必须取「相对父 surface window geometry 左上角」的坐标，正好等于
     * 我们要的「父窗口内容区坐标」。不能再用
     * wlr_xdg_popup_get_toplevel_coords()：它沿 base->current.geometry 累加，
     * 而那是 xdg_surface 的 window geometry（对 popup 恒为 0,0），实测只返回
     * 父窗口的 CSD 偏移 (26,23)，菜单被甩到窗口左上角。
     *
     * 关键修正：优先读 scheduled.geometry，而不是 current.geometry。
     * wlroots 在发 xdg_popup.configure 时把 FLIP/SLIDE 避让后的最终位置写进
     * scheduled；current 要等客户端 ack_configure 并且**再次 commit**才更新。
     * GTK 收到纯位置变化的 configure 只 ack、不重贴 buffer，于是 current 永远
     * 停在 (0,0)，候选窗就被钉在父窗口左上角装饰条上。 */
    struct wlr_box geom = popup->scheduled.geometry;
    if (geom.width <= 0 && geom.height <= 0)
        geom = popup->current.geometry;
    *lx = geom.x;
    *ly = geom.y;
    *w = geom.width;
    *h = geom.height;
}

void wlroots_proto_popup_unconstrain(wlroots_proto* p, void* popup_ptr, int x, int y, int w, int h)
{
    struct wlr_xdg_popup* popup = popup_ptr;
    if (!popup || !popup->base)
        return;
    struct wlr_box box = { x, y, w, h };

    /* 首个 configure 之前的调用一律只记框（同 on_new_popup 的理由：
     * unconstrain_from_box 末尾自己 schedule_configure，initialized 假就崩），
     * 由 popup_on_commit 在首次 commit 后统一消费。 */
    struct popup_node* node;
    wl_list_for_each(node, &p->popups, link)
    {
        if (node->popup != popup)
            continue;
        if (!node->configure_sent) {
            node->constraint_box = box;
            node->has_constraint_box = true;
            return;
        }
        break;
    }
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

void wlroots_proto_popup_dismiss(wlroots_proto* p, void* popup_ptr)
{
    (void)p;
    struct wlr_xdg_popup* popup = popup_ptr;
    if (!popup)
        return;
    /* wlroots 0.20 没有公开的 popup_done 发送函数；destroy 等价于发 popup_done
     * 并置 inert（旧桥 xdg_popup_resource_destroy 的同款语义）。 */
    wlr_xdg_popup_destroy(popup);
}

int wlroots_proto_popup_is_grabbing(wlroots_proto* p, void* popup_ptr)
{
    (void)p;
    struct wlr_xdg_popup* popup = popup_ptr;
    return popup && popup->seat ? 1 : 0;
}

void wlroots_proto_subsurface_offset(wlroots_proto* p, void* subsurface_ptr, int* x, int* y)
{
    (void)p;
    struct wlr_subsurface* sub = subsurface_ptr;
    if (!sub) {
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    if (x) *x = sub->current.x;
    if (y) *y = sub->current.y;
}

void wlroots_proto_toplevel_geometry_offset(wlroots_proto* p, void* surface_ptr, int* gx, int* gy)
{
    (void)p;
    if (gx) *gx = 0;
    if (gy) *gy = 0;
    struct wlr_surface* surface = surface_ptr;
    if (!surface)
        return;
    struct wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
    if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        return;
    struct wlr_box const geom = xdg->current.geometry;
    if (gx) *gx = geom.x;
    if (gy) *gy = geom.y;
}

void wlroots_proto_surface_repaint(wlroots_proto* p, void* surface_ptr)
{
    if (!p || !surface_ptr)
        return;
    struct plain_surface_node* node = plain_node_for_surface(p, surface_ptr);
    if (node)
        emit_plain_frame(p, node);
}
