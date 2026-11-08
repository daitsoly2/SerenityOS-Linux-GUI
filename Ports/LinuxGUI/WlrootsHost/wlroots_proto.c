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
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
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

/* 输入法（IME）子系统，实现在文件后半段（见「输入法 text-input-v3」一节）。 */
static void on_new_text_input(struct wl_listener* listener, void* data);
static void on_new_input_method(struct wl_listener* listener, void* data);
static void on_new_virtual_keyboard(struct wl_listener* listener, void* data);

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

    /*
     * ── 输入法（text-input-v3 + input-method-v2）──────────────────────────
     * 系统里有两条互不相通的输入法路径：
     *   A) 原生 Serenity 应用：GUI::InputMethod → /tmp/serenity-ime.sock → fcitx5
     *   B) 外部 Linux 客户端（本桥）：Wayland 协议 → 桥 → IM
     * 这条桥只服务 B：客户端用 zwp_text_input_v3 报「我要输文本」，
     * fcitx5 用 zwp_input_method_v2 接住按键、组字，再把结果 commit 回客户端。
     * 与 A 不冲突（A 走 socket，B 走协议）。
     */
    struct wlr_text_input_manager_v3* text_input_manager;
    struct wlr_input_method_manager_v2* im_manager;
    /* fcitx5 在 IM grab 生效时会用 zwp_virtual_keyboard_v1 把「它不消费的键」
     * 回灌给合成器。没有这个 global，IM 激活期间 Ctrl+C / 方向键会全部丢失。 */
    struct wlr_virtual_keyboard_manager_v1* virtual_keyboard_manager;
    struct wl_list text_inputs;      /* text_input_node */
    struct wl_list input_methods;    /* input_method_node */
    struct wl_list virtual_keyboards;/* virtual_keyboard_node */
    struct wl_listener new_text_input;
    struct wl_listener new_input_method;
    struct wl_listener new_virtual_keyboard;
    /* 当前 wl_keyboard 焦点 surface（text-input enter 要发给它所属 client）。 */
    struct wlr_surface* keyboard_focus_surface;
    struct wlr_input_method_v2* input_method;        /* 当前接入的 IM（fcitx5） */
    struct wlr_text_input_v3* focused_text_input;    /* 已 enter 的客户端 text_input */

    struct wlroots_proto_callbacks cb;
    char socket[64];
};

/*
 * text_input 节点：一个客户端（一个 wl_surface 的所有者）可以有多个
 * zwp_text_input_v3，生命周期完全跟着协议对象走。
 * 用独立节点而不是给 wlroots_proto 塞零散 listener，是为了避免
 * container_of 挂错基准字段（见 HANDOFF §6.2 的铁律）。
 */
struct text_input_node {
    struct wlroots_proto* proto;
    struct wlr_text_input_v3* ti;
    struct wl_listener enable;
    struct wl_listener commit;
    struct wl_listener disable;
    struct wl_listener destroy;
    struct wl_list link; /* wlroots_proto.text_inputs */
};

/* 当前接入的输入法（fcitx5）及其监听器。 */
struct input_method_node {
    struct wlroots_proto* proto;
    struct wlr_input_method_v2* im;
    struct wl_listener commit;            /* im->events.commit */
    struct wl_listener grab_keyboard;     /* im->events.grab_keyboard */
    struct wl_listener new_popup_surface; /* im->events.new_popup_surface（候选窗） */
    struct wl_listener destroy;           /* im->events.destroy */
    struct wl_list link; /* wlroots_proto.input_methods */
};

/* zwp_virtual_keyboard_v1（fcitx5 回灌未消费按键用）。 */
struct virtual_keyboard_node {
    struct wlroots_proto* proto;
    struct wlr_virtual_keyboard_v1* vk;
    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
    struct wl_list link; /* wlroots_proto.virtual_keyboards */
};

static struct toplevel_node* node_for_surface(struct wlroots_proto* p, struct wlr_surface* surface){
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

/*
 * 首帧 configure 的延迟发送：wlroots 0.20 的 wlr_xdg_surface_schedule_configure
 * 断言 surface->initialized，而 initialized 只在客户端首次 commit 走完 xdg_surface
 * 的 role commit（wlroots 在 surface->role->commit 里置真）之后才为真。
 *
 * 旧实现在 on_surface_commit 里「同步」调 schedule_configure：多数构建里
 * role commit 先于 events.commit 的 listener，于是 initialized 已真、没问题；
 * 但部分 libwlroots 构建把顺序翻了（role commit 晚于本 listener），于是顶着
 * initialized=false 调 schedule_configure，触发
 * "Assertion surface->initialized failed"，合成器直接 abort、客户端拿 EPIPE。
 * 实测 winit 0.30 的首帧协商正好踩这个时序（GTK/Firefox 的时序恰好绕开）。
 *
 * 修法：沿用 popup_on_commit 的同一思路 —— 把 schedule_configure 推迟到下一次
 * dispatch 的 idle 里做，那时首次 commit 的 role commit 必然已跑完、initialized
 * 必为真，既绕开断言、又不会漏发（configure_sent 只置一次，靠 configure_drain
 * 现有的 flush 逻辑与 done 同批发走）。 */
static void schedule_toplevel_configure_idle(void* data)
{
    struct toplevel_node* node = data;
    if (!node || !node->toplevel || !node->toplevel->base)
        return;
    struct wlr_xdg_surface* xdg = node->toplevel->base;
    if (!xdg->initialized) {
        /* 不应发生（idle 在 role commit 之后才跑）；保险再排一次，避免漏发
         * 首帧 configure 把客户端卡死在 roundtrip。 */
        wl_event_loop_add_idle(wl_display_get_event_loop(node->proto->display),
            schedule_toplevel_configure_idle, node);
        return;
    }
    wlr_xdg_surface_schedule_configure(xdg);
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
     * 这里在首次 commit 时主动补一次，但必须推迟到 idle 里发（见
     * schedule_toplevel_configure_idle 的注释），否则会顶着 initialized=false
     * 触发 wlroots 的断言使合成器 abort。 */
    if (node->toplevel && node->toplevel->base
        && !node->toplevel->base->configured && !node->configure_sent) {
        node->configure_sent = true;
        wl_event_loop_add_idle(wl_display_get_event_loop(p->display),
            schedule_toplevel_configure_idle, node);
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
    struct wlroots_proto* proto;
};

/*
 * 把「强制 SERVER_SIDE」的 set_mode 推迟到 surface->initialized 为真之后再发。
 *
 * 客户端（winit 0.30 / GTK / Firefox）在首个 wl_surface.commit 之前就创建
 * xdg_toplevel_decoration 对象并请求 CLIENT_SIDE，而
 * wlr_xdg_toplevel_decoration_v1_set_mode 内部会调
 * wlr_xdg_surface_schedule_configure（decoration_v1.c:57），后者断言
 * surface->initialized —— 在首 commit 之前该标志为假，同步调必崩
 * （"Assertion surface->initialized failed"，合成器 abort、客户端 EPIPE）。
 *
 * 旧实现在 on_new_decoration / on_decoration_request_mode 里同步 set_mode，
 * 正好踩这个时序。修法沿用 popup_on_commit 的 idle 推迟思路：排到下一次
 * dispatch，若仍 !initialized 就再排一轮，直到客户端首次 commit 把
 * initialized 置真；deco 销毁时置 dead，避免悬垂指针。 */
struct deco_defer {
    struct wlr_xdg_toplevel_decoration_v1* deco;
    struct wlroots_proto* proto;
    struct wl_listener deco_destroy;
    bool dead;
};

static void deco_defer_on_destroy(struct wl_listener* l, void*)
{
    struct deco_defer* d = wl_container_of(l, d, deco_destroy);
    d->dead = true;
}

static void schedule_decoration_mode_idle(void* data)
{
    struct deco_defer* d = data;
    if (d->dead) {
        free(d);
        return;
    }
    struct wlr_xdg_surface* xdg = d->deco->toplevel->base;
    if (!xdg->initialized) {
        /* 首 commit 还没到，再排一轮（idle 成本低，会随客户端首次
         * commit 自然结束，不会无限循环）。 */
        wl_event_loop_add_idle(wl_display_get_event_loop(d->proto->display),
            schedule_decoration_mode_idle, d);
        return;
    }
    wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_list_remove(&d->deco_destroy.link);
    free(d);
}

static void request_server_side_decoration(struct wlroots_proto* p,
        struct wlr_xdg_toplevel_decoration_v1* deco)
{
    struct deco_defer* d = calloc(1, sizeof(*d));
    if (!d)
        return;
    d->deco = deco;
    d->proto = p;
    d->dead = false;
    d->deco_destroy.notify = deco_defer_on_destroy;
    wl_signal_add(&deco->events.destroy, &d->deco_destroy);
    wl_event_loop_add_idle(wl_display_get_event_loop(p->display),
        schedule_decoration_mode_idle, d);
}

static void on_decoration_request_mode(struct wl_listener* listener, void* data)
{
    struct decoration_state* state = wl_container_of(listener, state, request_mode);
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    /* 客户端请求 CLIENT_SIDE：推迟回 SERVER_SIDE（见 deco_defer 注释）。 */
    request_server_side_decoration(state->proto, deco);
}

static void on_decoration_destroy(struct wl_listener* listener, void*)
{
    struct decoration_state* state = wl_container_of(listener, state, destroy);
    wl_list_remove(&state->request_mode.link);
    wl_list_remove(&state->destroy.link);
    free(state);
}

static void on_new_decoration(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_decoration);
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    /* 新 decoration 必然在首 commit 之前诞生，同步 set_mode 必崩；推迟。 */
    request_server_side_decoration(p, deco);

    struct decoration_state* state = calloc(1, sizeof(*state));
    if (!state)
        return;
    state->proto = p;
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
    wl_list_init(&p->text_inputs);
    wl_list_init(&p->input_methods);
    wl_list_init(&p->virtual_keyboards);

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

    /*
     * 输入法通路。默认配置：
     *   zwp_text_input_v3  ← 客户端声明「这里要输文本」（桥自己当 IM 前端）
     * 桥拿到键后走 serenityime socket 交给引擎，候选窗自己用 LibGUI 画。
     *
     * SERENITY_IM_V2=1 时才额外暴露 input-method-v2 + virtual-keyboard，
     * 让 fcitx5 自己当 IM（自带候选窗，外观不受控），仅用于对照实验。
     */
    p->text_input_manager = wlr_text_input_manager_v3_create(p->display);
    if (p->text_input_manager) {
        p->new_text_input.notify = on_new_text_input;
        wl_signal_add(&p->text_input_manager->events.new_text_input, &p->new_text_input);
    }
    if (getenv("SERENITY_IM_V2")) {
        p->im_manager = wlr_input_method_manager_v2_create(p->display);
        if (p->im_manager) {
            p->new_input_method.notify = on_new_input_method;
            wl_signal_add(&p->im_manager->events.new_input_method, &p->new_input_method);
        }
        p->virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(p->display);
        if (p->virtual_keyboard_manager) {
            p->new_virtual_keyboard.notify = on_new_virtual_keyboard;
            wl_signal_add(&p->virtual_keyboard_manager->events.new_virtual_keyboard,
                &p->new_virtual_keyboard);
        }
        fprintf(stderr, "ws-proto: input-method-v2 enabled (fcitx5 draws its own candidate window)\n");
    }

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
        /*
         * 这几个 wlroots manager 在 display_destroy 时会断言
         * 「events.*.listener_list 为空」（wlr_text_input_v3.c /
         * wlr_input_method_v2.c 里都是硬 assert）。必须先摘掉自己的监听器，
         * 否则桥退出时会被自己的断言打死（表现为退出码 134）。
         */
        if (p->text_input_manager)
            wl_list_remove(&p->new_text_input.link);
        if (p->im_manager)
            wl_list_remove(&p->new_input_method.link);
        if (p->virtual_keyboard_manager)
            wl_list_remove(&p->new_virtual_keyboard.link);
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

/* ── 输入法 text-input-v3 / input-method-v2 ────────────────────────────────
 *
 * 数据流（与 wlroots 的 sway 参考实现一致）：
 *
 *   客户端           桥                        fcitx5(IM)
 *   ─────────────────────────────────────────────────────────────
 *   enable + commit → 若该 client 的 surface 正有键盘焦点：
 *                       text_input.enter(surface)
 *                       im.activate + surrounding/content_type + im.done
 *   （用户按键）    ←  grab 生效时 key 只发 grab，否则照旧发 seat
 *   IM 组字完成      → im.commit → text_input.preedit/commit_string/delete
 *                                  + text_input.done
 *
 * 三条硬约束（写错就 abort，不是「不生效」而是「进程死」）：
 *   1. wlr_text_input_v3_send_enter 断言 focused_surface == NULL，
 *      send_leave 断言 focused_surface != NULL —— 必须自己配对。
 *   2. send_enter 还断言 text_input 与 surface 属于同一个 wl_client。
 *   3. wlr_input_method_v2_send_done 才会让 IM 认为 activate 生效
 *      （它内部把 active 拷贝进 client_active），不 done 则 IM 一直不组字。
 */

/* 把客户端当前的 surrounding text / content type / 变更原因 送给 IM，并以 done 收尾。 */
static void im_send_text_input_state(struct wlroots_proto* p, struct wlr_text_input_v3* ti)
{
    if (!p->input_method)
        return;
    wlr_input_method_v2_send_surrounding_text(p->input_method,
        ti->current.surrounding.text, ti->current.surrounding.cursor,
        ti->current.surrounding.anchor);
    wlr_input_method_v2_send_content_type(p->input_method,
        ti->current.content_type.hint, ti->current.content_type.purpose);
    wlr_input_method_v2_send_text_change_cause(p->input_method,
        ti->current.text_change_cause);
    wlr_input_method_v2_send_done(p->input_method);
}

/* 激活 IM 并把当前状态一起推给它（activate 本身要 done 才生效，合并成一次）。 */
static void im_activate_for_text_input(struct wlroots_proto* p, struct wlr_text_input_v3* ti)
{
    if (!p->input_method)
        return;
    if (!p->input_method->active)
        wlr_input_method_v2_send_activate(p->input_method);
    im_send_text_input_state(p, ti);
}

static void im_deactivate(struct wlroots_proto* p)
{
    if (!p->input_method || !p->input_method->active)
        return;
    wlr_input_method_v2_send_deactivate(p->input_method);
    wlr_input_method_v2_send_done(p->input_method);
}

/* 取消某个 text_input 的焦点（leave + 必要时 deactivate IM）。幂等。 */
static void text_input_drop_focus(struct wlroots_proto* p, struct wlr_text_input_v3* ti)
{
    if (!ti || !ti->focused_surface)
        return;
    wlr_text_input_v3_send_leave(ti);
    if (p->focused_text_input == ti) {
        p->focused_text_input = NULL;
        im_deactivate(p);
    }
}

/* 给 text_input 焦点：仅当它已 enable 且键盘焦点属于同一个客户端。 */
static bool text_input_take_focus(struct wlroots_proto* p, struct wlr_text_input_v3* ti)
{
    struct wlr_surface* focus = p->keyboard_focus_surface;
    /* 协议顺序是「合成器先 enter → 客户端才 enable」。以前这里要求
     * ti->current_enabled 才发 enter，GTK/Qt 严格遵守协议 ⇒ 两边互等死锁，
     * 表现为 text_input 创建了但永远 ti_active=0、输入法完全不生效。
     * titest 自己抢先 enable 才碰巧能用。 */
    if (!focus || ti->focused_surface)
        return false;
    if (p->focused_text_input && p->focused_text_input != ti)
        return false;
    if (wl_resource_get_client(focus->resource) != wl_resource_get_client(ti->resource))
        return false;

    wlr_text_input_v3_send_enter(ti, focus);
    p->focused_text_input = ti;
    im_activate_for_text_input(p, ti);
    if (p->cb.text_input_state_changed)
        p->cb.text_input_state_changed(p->cb.user, focus);
    return true;
}

/*
 * enable / commit 走同一套逻辑：
 *   - 未 enable：把焦点收回来（客户端显式关闭了文本输入）
 *   - 已 enable 但还没焦点：尝试拿焦点（覆盖「先 commit enable、后窗口获焦」）
 *   - 已有焦点：把最新状态同步给 IM（客户端改了 surrounding text 等）
 */
static void text_input_handle_commit(struct wlroots_proto* p, struct wlr_text_input_v3* ti)
{
    fprintf(stderr, "ws-proto: text_input commit enabled=%d focused=%p kb_focus=%p im=%p\n",
        ti->current_enabled, (void*)ti->focused_surface, (void*)p->keyboard_focus_surface,
        (void*)p->input_method);
    if (!ti->current_enabled) {
        /* 客户端显式 disable：保留 enter（焦点还在它的 surface 上），
         * 只让上层收掉引擎 context；下次 enable+commit 就能立刻接着用。 */
        if (p->cb.text_input_state_changed && ti->focused_surface)
            p->cb.text_input_state_changed(p->cb.user, ti->focused_surface);
        return;
    }
    if (ti->focused_surface) {
        im_send_text_input_state(p, ti);
        if (p->cb.text_input_state_changed)
            p->cb.text_input_state_changed(p->cb.user, ti->focused_surface);
        return;
    }
    text_input_take_focus(p, ti);
}

/* GTK/Qt 会为每个 GdkWindow 建一个 text_input，p->focused_text_input 可能被
 * 后建的那个覆盖，而真正 enter 的是先建的那个。一律按「谁有 focused_surface」
 * 来找，别信单一指针。 */
static struct wlr_text_input_v3* focused_text_input(struct wlroots_proto* p)
{
    if (p->focused_text_input && p->focused_text_input->focused_surface)
        return p->focused_text_input;
    struct text_input_node* n;
    wl_list_for_each(n, &p->text_inputs, link)
    {
        if (n->ti && n->ti->focused_surface)
            return n->ti;
    }
    return NULL;
}

/* ── 内置输入法用的公开 API ─────────────────────────────────────────────── */

int wlroots_proto_text_input_active(wlroots_proto* p)
{
    return p && focused_text_input(p) ? 1 : 0;
}

void wlroots_proto_text_input_cursor_rect(wlroots_proto* p, int* x, int* y, int* w, int* h)
{
    int rx = 0, ry = 0, rw = 0, rh = 0;
    struct wlr_text_input_v3* ti = focused_text_input(p);
    if (ti) {
        rx = ti->current.cursor_rectangle.x;
        ry = ti->current.cursor_rectangle.y;
        rw = ti->current.cursor_rectangle.width;
        rh = ti->current.cursor_rectangle.height;
    }
    if (x)
        *x = rx;
    if (y)
        *y = ry;
    if (w)
        *w = rw;
    if (h)
        *h = rh;
}

void wlroots_proto_text_input_commit(wlroots_proto* p, char const* text)
{
    struct wlr_text_input_v3* ti = focused_text_input(p);
    if (!ti || !text)
        return;
    wlr_text_input_v3_send_commit_string(ti, text);
}

void wlroots_proto_text_input_preedit(wlroots_proto* p, char const* text, int cursor_begin, int cursor_end)
{
    struct wlr_text_input_v3* ti = focused_text_input(p);
    if (!ti)
        return;
    wlr_text_input_v3_send_preedit_string(ti, text ? text : "",
        cursor_begin, cursor_end);
}

void wlroots_proto_text_input_delete(wlroots_proto* p, unsigned before_length, unsigned after_length)
{
    struct wlr_text_input_v3* ti = focused_text_input(p);
    if (!ti)
        return;
    wlr_text_input_v3_send_delete_surrounding_text(ti,
        before_length, after_length);
}

void wlroots_proto_text_input_done(wlroots_proto* p)
{
    struct wlr_text_input_v3* ti = focused_text_input(p);
    if (!ti)
        return;
    wlr_text_input_v3_send_done(ti);
}

static void on_text_input_enable(struct wl_listener* listener, void*)
{
    struct text_input_node* n = wl_container_of(listener, n, enable);
    text_input_handle_commit(n->proto, n->ti);
}

static void on_text_input_commit_event(struct wl_listener* listener, void*)
{
    struct text_input_node* n = wl_container_of(listener, n, commit);
    text_input_handle_commit(n->proto, n->ti);
}

static void on_text_input_disable(struct wl_listener* listener, void*)
{
    struct text_input_node* n = wl_container_of(listener, n, disable);
    text_input_drop_focus(n->proto, n->ti);
}

static void on_text_input_destroy(struct wl_listener* listener, void*)
{
    struct text_input_node* n = wl_container_of(listener, n, destroy);
    struct wlroots_proto* p = n->proto;
    if (p->focused_text_input == n->ti) {
        p->focused_text_input = NULL;
        im_deactivate(p);
    }
    wl_list_remove(&n->enable.link);
    wl_list_remove(&n->commit.link);
    wl_list_remove(&n->disable.link);
    wl_list_remove(&n->destroy.link);
    wl_list_remove(&n->link);
    free(n);
}

static void on_new_text_input(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_text_input);
    struct wlr_text_input_v3* ti = data;

    struct text_input_node* n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->proto = p;
    n->ti = ti;
    n->enable.notify = on_text_input_enable;
    wl_signal_add(&ti->events.enable, &n->enable);
    n->commit.notify = on_text_input_commit_event;
    wl_signal_add(&ti->events.commit, &n->commit);
    n->disable.notify = on_text_input_disable;
    wl_signal_add(&ti->events.disable, &n->disable);
    n->destroy.notify = on_text_input_destroy;
    wl_signal_add(&ti->events.destroy, &n->destroy);
    wl_list_insert(&p->text_inputs, &n->link);
    fprintf(stderr, "ws-proto: new text_input %p client=%p\n", (void*)ti,
        (void*)wl_resource_get_client(ti->resource));
}

/* IM 组好字 → 回灌给客户端。 */
static void on_input_method_commit(struct wl_listener* listener, void*)
{
    struct input_method_node* n = wl_container_of(listener, n, commit);
    struct wlroots_proto* p = n->proto;
    struct wlr_input_method_v2* im = n->im;
    struct wlr_text_input_v3* ti = p->focused_text_input;
    if (!ti || !ti->focused_surface)
        return;

    struct wlr_input_method_v2_state* st = &im->current;
    /* preedit 必须无条件发（空串＝清掉上一次的预编辑）。 */
    wlr_text_input_v3_send_preedit_string(ti,
        st->preedit.text ? st->preedit.text : "",
        st->preedit.cursor_begin, st->preedit.cursor_end);
    if (st->commit_text)
        wlr_text_input_v3_send_commit_string(ti, st->commit_text);
    if (st->delete.before_length || st->delete.after_length)
        wlr_text_input_v3_send_delete_surrounding_text(ti,
            st->delete.before_length, st->delete.after_length);
    wlr_text_input_v3_send_done(ti);
}

static void on_input_method_grab_keyboard(struct wl_listener* listener, void* data)
{
    struct input_method_node* n = wl_container_of(listener, n, grab_keyboard);
    struct wlr_input_method_keyboard_grab_v2* grab = data;
    /* 把组成员的虚拟键盘交给 grab：wlroots 会据此给 IM 发 keymap/repeat/modifiers。
     * 没有这一步，fcitx5 拿到的是 NO_KEYMAP，一个字符都翻译不出来。 */
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, n->proto->keyboard);
    fprintf(stderr, "ws-proto: input-method grabbed keyboard\n");
}

static void on_input_method_popup_surface(struct wl_listener* listener, void* data)
{
    struct input_method_node* n = wl_container_of(listener, n, new_popup_surface);
    (void)n;
    struct wlr_input_popup_surface_v2* popup_surface = data;
    /* 候选窗：fcitx5 用 zwp_input_popup_surface_v2 画候选词。这里先只登记，
     * 像素转发见 handle_input_popup_commit（候选窗走 wl_surface 的 commit）。 */
    fprintf(stderr, "ws-proto: input-method popup surface %p (candidate window)\n",
        (void*)popup_surface);
}

static void on_input_method_destroy(struct wl_listener* listener, void*)
{
    struct input_method_node* n = wl_container_of(listener, n, destroy);
    struct wlroots_proto* p = n->proto;
    wl_list_remove(&n->commit.link);
    wl_list_remove(&n->grab_keyboard.link);
    wl_list_remove(&n->new_popup_surface.link);
    wl_list_remove(&n->destroy.link);
    wl_list_remove(&n->link);
    if (p->input_method == n->im)
        p->input_method = NULL;
    free(n);
    fprintf(stderr, "ws-proto: input-method gone\n");
}

static void on_new_input_method(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_input_method);
    struct wlr_input_method_v2* im = data;

    /* 只支持一个 IM：新来的顶掉旧的（fcitx5 重启后会重新 bind）。 */
    if (p->input_method && p->input_method != im) {
        wlr_input_method_v2_send_unavailable(p->input_method);
        p->input_method = NULL;
    }

    struct input_method_node* n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->proto = p;
    n->im = im;
    n->commit.notify = on_input_method_commit;
    wl_signal_add(&im->events.commit, &n->commit);
    n->grab_keyboard.notify = on_input_method_grab_keyboard;
    wl_signal_add(&im->events.grab_keyboard, &n->grab_keyboard);
    n->new_popup_surface.notify = on_input_method_popup_surface;
    wl_signal_add(&im->events.new_popup_surface, &n->new_popup_surface);
    n->destroy.notify = on_input_method_destroy;
    wl_signal_add(&im->events.destroy, &n->destroy);
    wl_list_insert(&p->input_methods, &n->link);
    p->input_method = im;

    fprintf(stderr, "ws-proto: input-method attached (fcitx5)\n");

    /* 已经有焦点（用户先点了输入框，fcitx5 后连上）时要补一次激活。 */
    if (p->focused_text_input && p->focused_text_input->focused_surface)
        im_activate_for_text_input(p, p->focused_text_input);
}

/* IM grab 生效时，键只发 grab，不再发 seat（协议要求「不得二次处理」）。 */
static struct wlr_input_method_keyboard_grab_v2* active_keyboard_grab(struct wlroots_proto* p)
{
    if (!p->input_method)
        return NULL;
    struct wlr_input_method_keyboard_grab_v2* grab = p->input_method->keyboard_grab;
    if (!grab || !grab->keyboard)
        return NULL;
    return grab;
}

/* ── zwp_virtual_keyboard_v1：fcitx5 回灌「它不消费的按键」───────────────
 * 关键：这里的键**必须**直接进 seat，不能再走 IM grab 路由，否则回灌的键
 * 又回到 fcitx5，形成无限循环。 */

static void on_vk_key(struct wl_listener* listener, void* data)
{
    struct virtual_keyboard_node* n = wl_container_of(listener, n, key);
    struct wlr_keyboard_key_event* ev = data;
    if (!n->proto->seat)
        return;
    wlr_seat_keyboard_notify_key(n->proto->seat, ev->time_msec, ev->keycode, ev->state);
}

static void on_vk_modifiers(struct wl_listener* listener, void* data)
{
    struct virtual_keyboard_node* n = wl_container_of(listener, n, modifiers);
    struct wlr_keyboard_modifiers* mods = data;
    if (!n->proto->seat)
        return;
    wlr_seat_keyboard_notify_modifiers(n->proto->seat, mods);
    n->proto->last_keyboard_mods = mods->depressed;
}

static void on_vk_destroy(struct wl_listener* listener, void*)
{
    struct virtual_keyboard_node* n = wl_container_of(listener, n, destroy);
    wl_list_remove(&n->key.link);
    wl_list_remove(&n->modifiers.link);
    wl_list_remove(&n->destroy.link);
    wl_list_remove(&n->link);
    free(n);
}

static void on_new_virtual_keyboard(struct wl_listener* listener, void* data)
{
    struct wlroots_proto* p = wl_container_of(listener, p, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1* vk = data;

    struct virtual_keyboard_node* n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->proto = p;
    n->vk = vk;
    n->key.notify = on_vk_key;
    wl_signal_add(&vk->keyboard.events.key, &n->key);
    n->modifiers.notify = on_vk_modifiers;
    wl_signal_add(&vk->keyboard.events.modifiers, &n->modifiers);
    n->destroy.notify = on_vk_destroy;
    wl_signal_add(&vk->keyboard.base.events.destroy, &n->destroy);
    wl_list_insert(&p->virtual_keyboards, &n->link);
    fprintf(stderr, "ws-proto: virtual keyboard attached (key forwarding enabled)\n");
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

    /* text-input 焦点：只给「同一个 wl_client 且已 enable」的 text_input 发 enter
     * （send_enter 内部断言 client 必须一致，且 focused_surface 必须为空）。 */
    struct wlr_surface* wlr_surf = surface;
    p->keyboard_focus_surface = wlr_surf;
    struct wl_client* client = wl_resource_get_client(wlr_surf->resource);
    fprintf(stderr, "ws-proto: kb_enter surf=%p client=%p text_inputs=%d\n", (void*)wlr_surf,
        (void*)client, (int)!wl_list_empty(&p->text_inputs));
    struct text_input_node* n;
    wl_list_for_each(n, &p->text_inputs, link)
    {
        struct wlr_text_input_v3* ti = n->ti;
        if (!ti)
            continue;
        if (wl_resource_get_client(ti->resource) != client) {
            /* 焦点换到别的客户端：旧的要收回，否则它会一直以为自己还在输入。 */
            text_input_drop_focus(p, ti);
            continue;
        }
        if (ti->focused_surface)
            continue;
        text_input_take_focus(p, ti);
    }
    if (p->focused_text_input && p->focused_text_input->focused_surface
        && p->cb.text_input_state_changed)
        p->cb.text_input_state_changed(p->cb.user, p->focused_text_input->focused_surface);
}

void wlroots_proto_keyboard_leave(wlroots_proto* p)
{
    if (!p || !p->seat)
        return;
    p->keyboard_focus_surface = NULL;
    struct text_input_node* n;
    wl_list_for_each(n, &p->text_inputs, link)
        text_input_drop_focus(p, n->ti);
    im_deactivate(p);
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
        /* IM grab 期间客户端收不到 key，但仍需要 modifiers 才能判 Ctrl/Shift。 */
        struct wlr_input_method_keyboard_grab_v2* grab_mods = active_keyboard_grab(p);
        if (grab_mods)
            wlr_input_method_keyboard_grab_v2_send_modifiers(grab_mods, &modifiers);
    }

    /*
     * 键路由：IM 抢了键盘就只发 grab（协议原文「must not further process any
     * event after it has been forwarded」）。fcitx5 会把不消费的键经
     * zwp_virtual_keyboard_v1 回灌，那条路径直连 seat，不会回到这里。
     */
    struct wlr_input_method_keyboard_grab_v2* grab = active_keyboard_grab(p);
    if (grab) {
        wlr_input_method_keyboard_grab_v2_send_key(grab, time_msec, evdev_code,
            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        return;
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
