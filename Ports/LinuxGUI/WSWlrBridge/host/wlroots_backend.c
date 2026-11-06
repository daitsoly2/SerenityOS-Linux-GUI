/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
// wlroots 交互层 —— 必须用 C 编译器编译（wlroots 0.20 头含 C99
// `float color[static 4]` 数组参数语法，C++ 不接受）。
//
// 架构（一层合成）：WindowServer 不再把整屏合成到一张位图，而是
//   * 桌面背景/图标 = 一个全屏 layer；
//   * 每个原生窗口 = 一个 layer（各自的 buffer + scene 节点）；
//   * 外来 Wayland/X11 客户端 = xdg_surface / xwayland surface 节点；
// 它们都是 wlr_scene 里平等的节点，由 wlroots 直接合成到 DRM 输出。
// 这与 labwc 的模型一致：没有「整屏 CPU framebuffer 反复上传」这一步。

#define _GNU_SOURCE // memfd_create
#include "wlroots_backend.h"

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/memfd.h>
#include <time.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <libinput.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#define BORDER 4
#define TITLE_HEIGHT 22
#define INITIAL_WIDTH 640
#define INITIAL_HEIGHT 480

static float const k_title_color[4] = { 0.039f, 0.141f, 0.416f, 1.0f }; // #0a246a
static float const k_frame_color[4] = { 0.831f, 0.816f, 0.784f, 1.0f }; // #d4d0c8

// ── 可被 CPU 写的 buffer ────────────────────────────────────────────────────
// 必须自建（malloc + wlr_buffer_impl）：allocator/swapchain 给的 GBM buffer
// 在 tiled modifier 下不支持 begin_data_ptr_access(WRITE)，写入会直接失败。
struct wswlr_layer;

struct cpu_buffer {
    struct wlr_buffer base;
    int fd; // memfd；-1 表示退化成纯 malloc（此时 GLES 无法上传）
    uint8_t* data;
    size_t size;
    uint32_t w, h, stride;
    // 消费者（wlroots）用完这块内存时置 true，池子才能安全复用。
    // 自己按帧数猜释放时机是错的：GLES 上传是异步的，会读到刚被回收的内存。
    bool idle;
    // 是否交给过 scene。没提交过的 buffer 没有任何 lock，wlr_buffer_drop()
    // 会命中 `n_locks > 0` 的断言，只能自己释放。
    bool submitted;
    // 归属的层：桌面层靠 release 触发一次「补画」，否则被丢掉的脏区会一直
    // 停在旧内容上（表现为「某块一直不刷新，动一下鼠标才出来」）。
    struct wswlr_layer* owner;
    struct wl_listener release;
};

static void cpu_buffer_unmap(struct cpu_buffer* b)
{
    if (b->data && b->size)
        munmap(b->data, b->size);
    b->data = NULL;
    b->size = 0;
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
}

// 用 memfd 承载像素并实现 get_shm：GLES 渲染器需要 shm 属性才能把这块内存
// 变成纹理（只实现 begin_data_ptr_access 时 pixman 能读、GLES 会直接跳过该节点，
// 表现就是「headless 正常、真机黑屏」）。
static bool cpu_buffer_alloc(struct cpu_buffer* b, uint32_t w, uint32_t h)
{
    cpu_buffer_unmap(b);
    b->w = w;
    b->h = h;
    b->stride = w * 4;
    b->size = (size_t)b->stride * h;
    if (b->size == 0)
        return false;

    b->fd = memfd_create("wswlr-layer", MFD_CLOEXEC);
    if (b->fd >= 0) {
        if (ftruncate(b->fd, (off_t)b->size) != 0) {
            cpu_buffer_unmap(b);
            return false;
        }
        void* p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
        if (p != MAP_FAILED) {
            b->data = p;
            memset(b->data, 0, b->size);
            return true;
        }
        cpu_buffer_unmap(b);
    }

    // 没有 memfd：退回匿名内存（pixman 仍可用，GLES 不可用）。
    b->data = calloc((size_t)h, (size_t)b->stride);
    b->size = b->data ? (size_t)b->stride * h : 0;
    return b->data != NULL;
}

static void cpu_buffer_on_release(struct wl_listener* listener, void*)
{
    struct cpu_buffer* b = wl_container_of(listener, b, release);
    b->idle = true;
}

static void cpu_buffer_destroy(struct wlr_buffer* buf)
{
    struct cpu_buffer* b = wl_container_of(buf, b, base);
    cpu_buffer_unmap(b);
    free(b);
}

static bool cpu_buffer_begin_access(struct wlr_buffer* buf, uint32_t flags,
    void** data, uint32_t* format, size_t* stride)
{
    (void)flags;
    struct cpu_buffer* b = wl_container_of(buf, b, base);
    if (!b->data)
        return false;
    *data = b->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = b->stride;
    return true;
}

static void cpu_buffer_end_access(struct wlr_buffer* buf) { (void)buf; }

static bool cpu_buffer_get_shm(struct wlr_buffer* buf, struct wlr_shm_attributes* attribs)
{
    struct cpu_buffer* b = wl_container_of(buf, b, base);
    if (b->fd < 0 || !b->data)
        return false;
    attribs->fd = b->fd;
    attribs->format = DRM_FORMAT_ARGB8888;
    attribs->width = (int)b->w;
    attribs->height = (int)b->h;
    attribs->stride = (int)b->stride;
    attribs->offset = 0;
    return true;
}

static const struct wlr_buffer_impl cpu_buffer_impl = {
    .destroy = cpu_buffer_destroy,
    .begin_data_ptr_access = cpu_buffer_begin_access,
    .end_data_ptr_access = cpu_buffer_end_access,
    .get_shm = cpu_buffer_get_shm,
};

static struct cpu_buffer* cpu_buffer_create(uint32_t w, uint32_t h)
{
    struct cpu_buffer* b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->fd = -1;
    if (!cpu_buffer_alloc(b, w, h)) {
        free(b);
        return NULL;
    }
    wlr_buffer_init(&b->base, &cpu_buffer_impl, (int)w, (int)h);
    b->release.notify = cpu_buffer_on_release;
    wl_signal_add(&b->base.events.release, &b->release);
    // 新块还没交给 wlroots，立刻可写；交给 scene 后置 false，release 再置回 true。
    b->idle = true;
    return b;
}

static void cpu_buffer_put(struct cpu_buffer** b)
{
    if (!*b)
        return;
    if ((*b)->submitted) {
        wlr_buffer_drop(&(*b)->base);
    } else {
        // 还没交给 scene：没有任何 lock，走 wlr_buffer_drop 会命中
        // `wlr_buffer_unlock: n_locks > 0` 断言，直接自己回收。
        cpu_buffer_unmap(*b);
        free(*b);
    }
    *b = NULL;
}

// ── 单层合成中的一个「层」：桌面、每个原生窗口各一个 ────────────────────────
#define LAYER_ACCUM 16

// 关键约束（本轮实测出来的根因）：wlroots 的 GLES 渲染器把「像素 → 纹理」的
// 结果挂在 wlr_buffer 的 addon 上。**复用同一个 wlr_buffer 对象、只改里面的
// 像素，纹理永远不会重新上传**，画面就冻结在第一次提交的那张快照上 ——
// 表现就是一帧好一帧黑地使劲闪。pixman 渲染器没有纹理，所以它完全正常。
//
// 因此每个层都按 Wayland 客户端的规矩来：每次提交用一个**新的** wlr_buffer
// 对象，旧对象在 release 之后连同纹理一起销毁。
struct wswlr_layer {
    struct wlr_scene_buffer* node;
    uint32_t width, height;
    struct wl_list link;

    // 窗口层正在绘制的 buffer（begin_paint 里新建，commit 后交出去）。
    struct cpu_buffer* painting;

    // 桌面层的源快照（WindowServer 的 framebuffer）。
    uint32_t const* src_pixels;
    uint32_t src_pitch;
};

// ── 外来窗口 ───────────────────────────────────────────────────────────────
struct FrameWindow {
    void* surface;
    bool xwayland;
    bool decorated;
    bool mapped;
    bool configured;
    struct wlr_xdg_toplevel* toplevel;
    struct wlr_xwayland_surface* xsurface;
    struct wlr_scene_tree* container;
    struct wlr_scene_tree* content;
    struct wlr_scene_rect* title_bar;
    struct wlr_scene_rect* border[4];
    int frame_w, frame_h;
    struct wl_listener destroy;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_configure;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_list link;
};

struct KeyboardNode {
    struct wlr_keyboard* keyboard;
    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
    struct wl_list link;
};

struct PopupNode {
    struct wlr_scene_tree* tree;
    struct wl_listener destroy;
};

struct wswlr_desktop_surface {
    struct wswlr_layer* layer;
};

struct wswlr_compositor {
    struct wl_display* display;
    struct wlr_backend* backend;
    struct wlr_renderer* renderer;
    struct wlr_allocator* allocator;
    struct wlr_scene* scene;
    struct wlr_scene_output* scene_output;
    struct wlr_output_layout* layout;
    struct wlr_output* output;
    struct wlr_compositor* compositor;
    struct wlr_linux_dmabuf_v1* linux_dmabuf;
    struct wlr_xdg_shell* xdg_shell;
    struct wlr_xdg_decoration_manager_v1* decoration;
    struct wlr_seat* seat;
    struct wlr_xwayland* xwayland;

    struct wlr_cursor* cursor;
    struct wlr_xcursor_manager* cursor_mgr;
    struct wl_list keyboards;
    struct wlr_keyboard* last_keyboard;
    struct wlr_surface* pointer_surface;
    struct wlr_surface* focused_foreign;

    struct wl_listener new_output;
    struct wl_listener new_xdg_surface;
    struct wl_listener new_toplevel;
    struct wl_listener new_decoration;
    struct wl_listener new_xwayland_surface;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener output_frame;

    struct wl_list windows;
    struct wl_list layers;
    // 原生窗口层统一挂在这个子树下：它建得最早，因此永远在外来窗口
    // （xdg/xwayland，直接挂在 scene 根下）之下。真正的交错堆叠要等
    // 外来窗口进 WindowServer 的窗口列表之后再做。
    struct wlr_scene_tree* native_tree;
    struct wswlr_desktop_surface* desktop;
    wswlr_callbacks callbacks;
    char const* socket;

    struct wl_event_source* shot_timer;
    char const* shot_path;
    struct wl_event_source* fake_timer;
};

static struct wswlr_compositor* g = NULL;

static void request_frame(void)
{
    if (g && g->output)
        wlr_output_schedule_frame(g->output);
}

// ── 复古装饰 ───────────────────────────────────────────────────────────────
static void build_frame(struct FrameWindow* window, int width, int height)
{
    struct wlr_scene_tree* tree = window->container;
    window->title_bar = wlr_scene_rect_create(tree, width + 2 * BORDER, TITLE_HEIGHT, k_title_color);
    wlr_scene_node_set_position(&window->title_bar->node, 0, 0);
    window->border[0] = wlr_scene_rect_create(tree, width + 2 * BORDER, BORDER, k_frame_color);
    window->border[1] = wlr_scene_rect_create(tree, width + 2 * BORDER, BORDER, k_frame_color);
    window->border[2] = wlr_scene_rect_create(tree, BORDER, height, k_frame_color);
    window->border[3] = wlr_scene_rect_create(tree, BORDER, height, k_frame_color);
    wlr_scene_node_set_position(&window->border[0]->node, 0, TITLE_HEIGHT);
    wlr_scene_node_set_position(&window->border[1]->node, 0, TITLE_HEIGHT + height + BORDER);
    wlr_scene_node_set_position(&window->border[2]->node, 0, TITLE_HEIGHT + BORDER);
    wlr_scene_node_set_position(&window->border[3]->node, width + BORDER, TITLE_HEIGHT + BORDER);
    window->frame_w = width;
    window->frame_h = height;
}

static void update_frame_geometry(struct FrameWindow* window, int width, int height)
{
    if (!window->decorated || !window->title_bar || width <= 0 || height <= 0)
        return;
    if (window->frame_w == width && window->frame_h == height)
        return;
    window->frame_w = width;
    window->frame_h = height;
    wlr_scene_rect_set_size(window->title_bar, width + 2 * BORDER, TITLE_HEIGHT);
    wlr_scene_rect_set_size(window->border[0], width + 2 * BORDER, BORDER);
    wlr_scene_rect_set_size(window->border[1], width + 2 * BORDER, BORDER);
    wlr_scene_rect_set_size(window->border[2], BORDER, height);
    wlr_scene_rect_set_size(window->border[3], BORDER, height);
    wlr_scene_node_set_position(&window->border[1]->node, 0, TITLE_HEIGHT + height + BORDER);
    wlr_scene_node_set_position(&window->border[3]->node, width + BORDER, TITLE_HEIGHT + BORDER);
}

static void attach_content(struct FrameWindow* window, struct wlr_surface* surface)
{
    if (!surface || window->content)
        return;
    window->content = wlr_scene_subsurface_tree_create(window->container, surface);
    wlr_scene_node_set_position(&window->content->node,
        window->decorated ? BORDER : 0, window->decorated ? TITLE_HEIGHT + BORDER : 0);
}

static unsigned foreign_flags(struct FrameWindow const* window)
{
    unsigned flags = 0;
    if (window->xwayland && window->xsurface) {
        if (window->xsurface->override_redirect)
            flags |= WSWLR_FOREIGN_OVERRIDE_REDIRECT;
        if (window->xsurface->parent)
            flags |= WSWLR_FOREIGN_DIALOG;
    } else if (window->toplevel && window->toplevel->parent) {
        flags |= WSWLR_FOREIGN_DIALOG;
    }
    return flags;
}

static void destroy_window(struct FrameWindow* window)
{
    wl_list_remove(&window->destroy.link);
    wl_list_remove(&window->set_title.link);
    wl_list_remove(&window->set_app_id.link);
    wl_list_remove(&window->associate.link);
    wl_list_remove(&window->dissociate.link);
    wl_list_remove(&window->request_configure.link);
    wl_list_remove(&window->map.link);
    wl_list_remove(&window->unmap.link);
    wl_list_remove(&window->commit.link);
    wl_list_remove(&window->link);
    if (g && g->callbacks.on_destroy_foreign)
        g->callbacks.on_destroy_foreign(window->surface, g->callbacks.user);
    if (window->container)
        wlr_scene_node_destroy(&window->container->node);
    free(window);
}

// ── xdg-shell ──────────────────────────────────────────────────────────────
static void notify_foreign(struct FrameWindow* window)
{
    if (!g->callbacks.on_new_foreign)
        return;
    g->callbacks.on_new_foreign(window->surface,
        window->xwayland ? "xwayland" : "xdg",
        window->xwayland ? window->xsurface->title : window->toplevel->title,
        window->xwayland ? window->xsurface->class : window->toplevel->app_id,
        foreign_flags(window), g->callbacks.user);
}

static void on_xdg_destroy(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, destroy);
    destroy_window(window);
}

static void on_xdg_set_title(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, set_title);
    if (window->mapped)
        notify_foreign(window);
}

static void on_xdg_set_app_id(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, set_app_id);
    if (window->mapped)
        notify_foreign(window);
}

static void on_xdg_commit(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, commit);
    struct wlr_xdg_surface* xdg_surface = window->toplevel->base;
    if (xdg_surface->geometry.width > 0 && xdg_surface->geometry.height > 0)
        update_frame_geometry(window, xdg_surface->geometry.width, xdg_surface->geometry.height);

    // 初始 configure 必须由合成器发：客户端做完无 buffer 的 initial commit
    // （此后 initialized 为 true）才会 map。大小 0x0 = 客户端自选。
    if (xdg_surface->initialized && !window->configured) {
        window->configured = true;
        wlr_xdg_toplevel_set_activated(window->toplevel, true);
        wlr_xdg_toplevel_set_size(window->toplevel, 0, 0);
    }
}

static void on_xdg_map(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, map);
    struct wlr_xdg_surface* xdg_surface = window->toplevel->base;
    window->mapped = true;
    update_frame_geometry(window, xdg_surface->geometry.width, xdg_surface->geometry.height);
    wlr_scene_node_raise_to_top(&window->container->node);
    notify_foreign(window);
    request_frame();
}

static void on_xdg_unmap(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, unmap);
    window->mapped = false;
}

static int next_cascade_offset(void)
{
    static int counter = 0;
    return 64 + (counter++ % 8) * 28;
}

static void on_new_toplevel(struct wl_listener*, void* data)
{
    struct wlr_xdg_toplevel* toplevel = data;
    struct wlr_xdg_surface* xdg_surface = toplevel->base;
    struct FrameWindow* window = calloc(1, sizeof(*window));
    if (!window)
        return;
    window->surface = toplevel;
    window->toplevel = toplevel;
    window->decorated = true;
    window->container = wlr_scene_tree_create(&g->scene->tree);
    int offset = next_cascade_offset();
    wlr_scene_node_set_position(&window->container->node, offset, offset);
    build_frame(window, INITIAL_WIDTH, INITIAL_HEIGHT);

    window->content = wlr_scene_xdg_surface_create(window->container, xdg_surface);
    wlr_scene_node_set_position(&window->content->node, BORDER, TITLE_HEIGHT + BORDER);
    wlr_scene_node_raise_to_top(&window->container->node);
    wl_list_insert(&g->windows, &window->link);

    window->destroy.notify = on_xdg_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &window->destroy);
    window->set_title.notify = on_xdg_set_title;
    wl_signal_add(&toplevel->events.set_title, &window->set_title);
    window->set_app_id.notify = on_xdg_set_app_id;
    wl_signal_add(&toplevel->events.set_app_id, &window->set_app_id);
    window->map.notify = on_xdg_map;
    wl_signal_add(&xdg_surface->surface->events.map, &window->map);
    window->unmap.notify = on_xdg_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &window->unmap);
    window->commit.notify = on_xdg_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &window->commit);
    wl_list_init(&window->associate.link);
    wl_list_init(&window->dissociate.link);
    wl_list_init(&window->request_configure.link);
}

static void on_popup_destroy(struct wl_listener* listener, void*)
{
    struct PopupNode* popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->destroy.link);
    if (popup->tree)
        wlr_scene_node_destroy(&popup->tree->node);
    free(popup);
}

static struct FrameWindow* window_for_surface(struct wlr_surface* surface)
{
    if (!surface)
        return NULL;
    struct wlr_surface* root = wlr_surface_get_root_surface(surface);
    struct FrameWindow* window;
    wl_list_for_each(window, &g->windows, link)
    {
        if (window->toplevel && window->toplevel->base
            && window->toplevel->base->surface == root)
            return window;
    }
    return NULL;
}

static void on_new_xdg_popup(struct wlr_xdg_surface* xdg_surface)
{
    struct wlr_xdg_popup* popup = xdg_surface->popup;
    if (!popup)
        return;
    struct FrameWindow* parent_window = window_for_surface(popup->parent);
    if (!parent_window || !parent_window->content)
        return;
    struct PopupNode* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->tree = wlr_scene_xdg_surface_create(parent_window->content, xdg_surface);
    wlr_scene_node_set_position(&node->tree->node,
        popup->current.geometry.x, popup->current.geometry.y);
    wlr_scene_node_raise_to_top(&node->tree->node);
    node->destroy.notify = on_popup_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &node->destroy);
}

static void on_new_xdg_surface(struct wl_listener*, void* data)
{
    struct wlr_xdg_surface* xdg_surface = data;
    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
        on_new_xdg_popup(xdg_surface);
    // toplevel 交给 on_new_toplevel（role 已确定的时机）。
}

static void on_new_decoration(struct wl_listener*, void* data)
{
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

// ── Xwayland ───────────────────────────────────────────────────────────────
static void on_xs_destroy(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, destroy);
    destroy_window(window);
}
static void on_xs_set_title(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, set_title);
    if (window->mapped)
        notify_foreign(window);
}
static void on_xs_set_class(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, set_app_id);
    if (window->mapped)
        notify_foreign(window);
}
static void on_xs_request_configure(struct wl_listener* listener, void* data)
{
    struct FrameWindow* window = wl_container_of(listener, window, request_configure);
    struct wlr_xwayland_surface_configure_event* event = data;
    wlr_xwayland_surface_configure(window->xsurface,
        event->x, event->y, event->width, event->height);
}
static void on_xs_commit(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, commit);
    if (!window->xsurface->surface)
        return;
    unsigned w = window->xsurface->surface->current.width;
    unsigned h = window->xsurface->surface->current.height;
    if (window->decorated)
        update_frame_geometry(window, (int)w, (int)h);
    wlr_scene_node_set_position(&window->container->node,
        window->xsurface->x, window->xsurface->y);
    request_frame();
}
static void on_xs_associate(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, associate);
    attach_content(window, window->xsurface->surface);
    if (window->xsurface->surface) {
        window->commit.notify = on_xs_commit;
        wl_signal_add(&window->xsurface->surface->events.commit, &window->commit);
        unsigned w = window->xsurface->surface->current.width;
        unsigned h = window->xsurface->surface->current.height;
        if (w > 0 && h > 0)
            update_frame_geometry(window, (int)w, (int)h);
    }
    window->mapped = true;
    notify_foreign(window);
    request_frame();
}
static void on_xs_dissociate(struct wl_listener* listener, void*)
{
    struct FrameWindow* window = wl_container_of(listener, window, dissociate);
    window->mapped = false;
    if (window->content) {
        wlr_scene_node_destroy(&window->content->node);
        window->content = NULL;
    }
}

static void on_new_xwayland_surface(struct wl_listener*, void* data)
{
    struct wlr_xwayland_surface* xsurface = data;
    struct FrameWindow* window = calloc(1, sizeof(*window));
    if (!window)
        return;
    window->surface = xsurface;
    window->xwayland = true;
    window->xsurface = xsurface;
    window->decorated = !xsurface->override_redirect;
    window->container = wlr_scene_tree_create(&g->scene->tree);
    wlr_scene_node_set_position(&window->container->node, 0, 0);
    if (window->decorated)
        build_frame(window, xsurface->width > 0 ? xsurface->width : INITIAL_WIDTH,
            xsurface->height > 0 ? xsurface->height : INITIAL_HEIGHT);
    wl_list_insert(&g->windows, &window->link);

    window->destroy.notify = on_xs_destroy;
    wl_signal_add(&xsurface->events.destroy, &window->destroy);
    window->set_title.notify = on_xs_set_title;
    wl_signal_add(&xsurface->events.set_title, &window->set_title);
    window->set_app_id.notify = on_xs_set_class;
    wl_signal_add(&xsurface->events.set_class, &window->set_app_id);
    window->associate.notify = on_xs_associate;
    wl_signal_add(&xsurface->events.associate, &window->associate);
    window->dissociate.notify = on_xs_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &window->dissociate);
    window->request_configure.notify = on_xs_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &window->request_configure);
    wl_list_init(&window->map.link);
    wl_list_init(&window->unmap.link);
    wl_list_init(&window->commit.link);
}

// ── output / frame ─────────────────────────────────────────────────────────
static void on_output_frame(struct wl_listener*, void*)
{
    if (!g->scene_output)
        return;
    // 删除 wlroots 侧的第二套「脏区/局部重绘」：WindowServer 已经有
    // Compositor::flush() 的 m_flush_rects / m_flush_transparent_rects，
    // wlroots 的 damage_ring + swapchain buffer-age 是在它之上又猜一次
    // 「哪些像素变了」。两边判断不一致时，output 换到新的 swapchain buffer
    // 后非 damage 区域是未初始化的 → 黑块/残影，且会随交互累积。
    // 只保留一个决策者（WS）：每次 commit 整幅重绘，wlroots 只负责呈现
    // WS 已经合成好的结果，不再自己做 damage 优化。
    wlr_damage_ring_add_whole(&g->scene_output->damage_ring);
    wlr_scene_output_commit(g->scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(g->scene_output, &now);
    // 不在这里重新 schedule：scene 有 damage 时自己会排帧；
    // 需要时由 request_frame() 在 present 之后踢一次。
}

static void on_new_output(struct wl_listener*, void* data)
{
    struct wlr_output* output = data;
    if (g->output)
        return;
    g->output = output;
    wlr_output_init_render(output, g->allocator, g->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode* mode = wlr_output_preferred_mode(output);
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    else
        wlr_output_state_set_custom_mode(&state, 1600, 900, 60000);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    wlr_output_create_global(output, g->display);
    wlr_output_layout_add_auto(g->layout, output);
    g->scene_output = wlr_scene_output_create(g->scene, output);
    g->output_frame.notify = on_output_frame;
    wl_signal_add(&output->events.frame, &g->output_frame);
    wlr_log(WLR_INFO, "ws-wlr: output %s %dx%d", output->name, output->width, output->height);
}

// ── 输入 ───────────────────────────────────────────────────────────────────
static struct wlr_surface* surface_at(double lx, double ly, double* sx, double* sy)
{
    if (!g || !g->scene)
        return NULL;
    struct wlr_scene_node* node = wlr_scene_node_at(&g->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return NULL;
    struct wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    return scene_surface ? scene_surface->surface : NULL;
}

static void update_pointer_focus(struct wswlr_compositor* c, uint32_t time_msec)
{
    double sx = 0, sy = 0;
    struct wlr_surface* surface = surface_at(c->cursor->x, c->cursor->y, &sx, &sy);

    static double last_x = -1, last_y = -1;
    bool const moved = c->cursor->x != last_x || c->cursor->y != last_y;
    bool const target_changed = surface != c->pointer_surface;
    last_x = c->cursor->x;
    last_y = c->cursor->y;
    c->pointer_surface = surface;
    if (!moved && !target_changed)
        return;

    if (c->callbacks.input.pointer_motion_absolute)
        c->callbacks.input.pointer_motion_absolute(c->cursor->x, c->cursor->y, c->callbacks.user);

    if (surface) {
        wlr_seat_pointer_notify_enter(c->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(c->seat, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(c->seat);
    }
}

static void on_cursor_motion(struct wl_listener*, void* data)
{
    struct wlr_pointer_motion_event* event = data;
    wlr_cursor_move(g->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    update_pointer_focus(g, event->time_msec);
}

static void on_cursor_motion_absolute(struct wl_listener*, void* data)
{
    struct wlr_pointer_motion_absolute_event* event = data;
    wlr_cursor_warp_absolute(g->cursor, &event->pointer->base, event->x, event->y);
    update_pointer_focus(g, event->time_msec);
}

static void on_cursor_button(struct wl_listener*, void* data)
{
    struct wlr_pointer_button_event* event = data;
    bool const pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    double sx = 0, sy = 0;
    struct wlr_surface* surface = surface_at(g->cursor->x, g->cursor->y, &sx, &sy);

    if (surface) {
        struct wlr_surface* root = wlr_surface_get_root_surface(surface);
        if (g->focused_foreign != root) {
            g->focused_foreign = root;
            if (g->last_keyboard)
                wlr_seat_keyboard_notify_enter(g->seat, root,
                    g->last_keyboard->keycodes, g->last_keyboard->num_keycodes,
                    &g->last_keyboard->modifiers);
        }
        wlr_seat_pointer_notify_button(g->seat, event->time_msec, event->button, event->state);
        return;
    }

    g->focused_foreign = NULL;
    wlr_seat_keyboard_clear_focus(g->seat);
    if (g->callbacks.input.pointer_button)
        g->callbacks.input.pointer_button(event->button, pressed, g->callbacks.user);
}

static void on_cursor_axis(struct wl_listener*, void* data)
{
    struct wlr_pointer_axis_event* event = data;
    if (g->pointer_surface) {
        wlr_seat_pointer_notify_axis(g->seat, event->time_msec, event->orientation,
            event->delta, event->delta_discrete, event->source, event->relative_direction);
        return;
    }
    if (!g->callbacks.input.pointer_axis)
        return;
    double vertical = event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL ? event->delta : 0.0;
    double horizontal = event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? event->delta : 0.0;
    g->callbacks.input.pointer_axis(vertical, horizontal, g->callbacks.user);
}

static void on_cursor_frame(struct wl_listener*, void*)
{
    wlr_seat_pointer_notify_frame(g->seat);
}

static void on_keyboard_modifiers(struct wl_listener* listener, void*)
{
    struct KeyboardNode* node = wl_container_of(listener, node, modifiers);
    wlr_seat_set_keyboard(g->seat, node->keyboard);
    wlr_seat_keyboard_notify_modifiers(g->seat, &node->keyboard->modifiers);
}

static void on_keyboard_key(struct wl_listener* listener, void* data)
{
    struct KeyboardNode* node = wl_container_of(listener, node, key);
    struct wlr_keyboard_key_event* event = data;
    bool const pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

    if (g->focused_foreign) {
        wlr_seat_set_keyboard(g->seat, node->keyboard);
        wlr_seat_keyboard_notify_key(g->seat, event->time_msec, event->keycode, event->state);
        return;
    }
    if (g->callbacks.input.keyboard_key)
        g->callbacks.input.keyboard_key(event->keycode, pressed, g->callbacks.user);
}

static void on_keyboard_destroy(struct wl_listener* listener, void*)
{
    struct KeyboardNode* node = wl_container_of(listener, node, destroy);
    wl_list_remove(&node->key.link);
    wl_list_remove(&node->modifiers.link);
    wl_list_remove(&node->destroy.link);
    wl_list_remove(&node->link);
    if (g->last_keyboard == node->keyboard)
        g->last_keyboard = NULL;
    free(node);
}

static void add_keyboard(struct wlr_keyboard* keyboard)
{
    struct KeyboardNode* node = calloc(1, sizeof(*node));
    if (!node)
        return;
    node->keyboard = keyboard;
    node->key.notify = on_keyboard_key;
    wl_signal_add(&keyboard->events.key, &node->key);
    node->modifiers.notify = on_keyboard_modifiers;
    wl_signal_add(&keyboard->events.modifiers, &node->modifiers);
    node->destroy.notify = on_keyboard_destroy;
    wl_signal_add(&keyboard->base.events.destroy, &node->destroy);
    wl_list_insert(&g->keyboards, &node->link);

    char const* layout = getenv("WSWLR_XKB_LAYOUT");
    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context) {
        struct xkb_rule_names names = { 0 };
        names.layout = layout && *layout ? layout : "us";
        struct xkb_keymap* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap) {
            wlr_keyboard_set_keymap(keyboard, keymap);
            xkb_keymap_unref(keymap);
        }
        xkb_context_unref(context);
    }
    wlr_keyboard_set_repeat_info(keyboard, 25, 600);
    g->last_keyboard = keyboard;
}

static void on_new_input(struct wl_listener*, void* data)
{
    struct wlr_input_device* device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_POINTER: {
        // 和 LinuxInput 的 SERENITY_POINTER_FLAT_ACCEL 对齐：libinput 现在归合成器
        // 所有，加速曲线也只能在这里设。VM 里靠它让注入的绝对坐标 1:1 落点。
        if (getenv("SERENITY_POINTER_FLAT_ACCEL")) {
            struct libinput_device* handle = wlr_libinput_get_device_handle(device);
            if (handle && libinput_device_config_accel_is_available(handle)) {
                libinput_device_config_accel_set_profile(handle, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
                libinput_device_config_accel_set_speed(handle, 0.0);
            }
        }
        wlr_cursor_attach_input_device(g->cursor, device);
        break;
    }
    case WLR_INPUT_DEVICE_KEYBOARD:
        add_keyboard(wlr_keyboard_from_input_device(device));
        break;
    default:
        break;
    }
    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&g->keyboards))
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(g->seat, capabilities);
}

// 开发用注入：headless 下没有真实设备，用它验证路由。
// WSWLR_FAKE_CLICK="x,y"  WSWLR_FAKE_KEYS="30 31"  WSWLR_FAKE_DELAY_MS=6000
static int on_fake_timer(void* data)
{
    struct wswlr_compositor* c = data;
    char const* click = getenv("WSWLR_FAKE_CLICK");
    if (click && *click) {
        double x = 0, y = 0;
        if (sscanf(click, "%lf,%lf", &x, &y) == 2) {
            wlr_cursor_warp(c->cursor, NULL, x, y);
            update_pointer_focus(c, 0);
            if (c->pointer_surface) {
                wlr_seat_pointer_notify_button(c->seat, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                wlr_seat_pointer_notify_button(c->seat, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                struct wlr_surface* root = wlr_surface_get_root_surface(c->pointer_surface);
                c->focused_foreign = root;
                if (c->last_keyboard)
                    wlr_seat_keyboard_notify_enter(c->seat, root,
                        c->last_keyboard->keycodes, c->last_keyboard->num_keycodes,
                        &c->last_keyboard->modifiers);
            } else if (c->callbacks.input.pointer_button) {
                c->callbacks.input.pointer_button(BTN_LEFT, true, c->callbacks.user);
                c->callbacks.input.pointer_button(BTN_LEFT, false, c->callbacks.user);
            }
        }
    }
    char const* keys = getenv("WSWLR_FAKE_KEYS");
    if (keys && *keys) {
        char* copy = strdup(keys);
        char* save = NULL;
        for (char* tok = strtok_r(copy, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
            uint32_t code = (uint32_t)atoi(tok);
            if (c->focused_foreign) {
                wlr_seat_keyboard_notify_key(c->seat, 0, code, WL_KEYBOARD_KEY_STATE_PRESSED);
                wlr_seat_keyboard_notify_key(c->seat, 0, code, WL_KEYBOARD_KEY_STATE_RELEASED);
            } else if (c->callbacks.input.keyboard_key) {
                c->callbacks.input.keyboard_key(code, true, c->callbacks.user);
                c->callbacks.input.keyboard_key(code, false, c->callbacks.user);
            }
        }
        free(copy);
    }
    return 0;
}

// ── 调试截图（headless 验证用）─────────────────────────────────────────────
static void write_ppm(struct wlr_buffer* buffer, char const* path)
{
    void* data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
            &data, &format, &stride))
        return;
    int w = buffer->width, h = buffer->height;
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; ++y) {
            uint8_t const* row = (uint8_t const*)data + (size_t)y * stride;
            for (int x = 0; x < w; ++x) {
                uint8_t const* px = row + (size_t)x * 4;
                fputc(px[2], f);
                fputc(px[1], f);
                fputc(px[0], f);
            }
        }
        fclose(f);
    }
    wlr_buffer_end_data_ptr_access(buffer);
}

static int on_shot_timer(void* data)
{
    struct wswlr_compositor* c = data;
    if (!c->scene_output)
        return 0;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (wlr_scene_output_build_state(c->scene_output, &state, NULL) && state.buffer)
        write_ppm(state.buffer, c->shot_path);
    wlr_output_state_finish(&state);
    return 0;
}

// ── 单层 API ───────────────────────────────────────────────────────────────
static struct wswlr_layer* layer_create(struct wswlr_compositor* c, int x, int y,
    uint32_t w, uint32_t h)
{
    struct wswlr_layer* layer = calloc(1, sizeof(*layer));
    if (!layer)
        return NULL;
    layer->width = w;
    layer->height = h;
    (void)w;
    (void)h;
    layer->node = wlr_scene_buffer_create(c->native_tree ? c->native_tree : &c->scene->tree, NULL);
    wlr_scene_node_set_position(&layer->node->node, x, y);
    wl_list_insert(&c->layers, &layer->link);
    return layer;
}

wswlr_layer* wswlr_compositor_create_layer(wswlr_compositor* c, int x, int y,
    uint32_t w, uint32_t h)
{
    if (!c || !c->scene)
        return NULL;
    struct wswlr_layer* layer = layer_create(c, x, y, w, h);
    if (layer && getenv("WSWLR_LAYER_DEBUG"))
        fprintf(stderr, "ws-wlr: layer create %dx%d at %d,%d (%p)\n", w, h, x, y, (void*)layer);
    return layer;
}

// 桌面层 present：把 WindowServer 的整幅快照拷进一个**新建**的 wlr_buffer 并提交。
// rects/buffer_index 都只为兼容 WindowServer 的调用形状；内容始终整幅，
// 因为新对象必须写满（旧对象是空的，且不能复用 —— 见 struct wswlr_layer 的注释）。
static void layer_present_rects(struct wswlr_layer* layer, uint32_t const* pixels,
    uint32_t pitch_bytes, uint32_t w, uint32_t h, wswlr_rect const* rects,
    size_t rect_count)
{
    (void)rects;
    (void)rect_count;
    if (!layer || !layer->node || !pixels || w == 0 || h == 0)
        return;

    struct cpu_buffer* dst = cpu_buffer_create(w, h);
    if (!dst)
        return;
    // 协议契约映射：桌面帧缓冲来自 WindowServer 的 Gfx::BitmapFormat::BGRx8888，
    // 最高字节是无意义的 "x"（Bitmap::allocate_backing_store 用 kcalloc 清零，
    // 所以通常是 0）。窗口层则真的是 BGRA8888（Compositor::render_window_to_layer
    // 会 clear_rect(Transparent)，圆角/阴影靠 alpha 混合）。两种像素共用一个
    // wlr_buffer，而 wlr_buffer 必须声明成 DRM_FORMAT_ARGB8888 —— 因为窗口层要
    // 真 alpha。于是桌面层的 x=0 被 GLES 当成 alpha=0：整块像素被透明混合，
    // 桌面底下没有别的东西，就露出 output 的黑底 = 鼠标一动就冒黑块、越动越多。
    // KMS 路径直接扫描 XRGB、压根不读这个字节，所以完全没有这个症状。
    // 这里把 WS 的「XRGB，x 无意义」语义映射到 wlroots 的 ARGB 契约：x 一律规范
    // 成 opaque。窗口层不走这条路径（begin_paint 由 WS 直接写 BGRA），不受影响。
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* dst_row = dst->data + (size_t)y * dst->stride;
        uint8_t const* src_row = (uint8_t const*)pixels + (size_t)y * pitch_bytes;
        memcpy(dst_row, src_row, (size_t)w * 4);
        uint32_t* p = (uint32_t*)dst_row;
        for (uint32_t x = 0; x < w; ++x)
            p[x] |= 0xff000000u;
    }

    layer->src_pixels = pixels;
    layer->src_pitch = pitch_bytes;
    layer->width = w;
    layer->height = h;

    dst->idle = false;
    dst->submitted = true;
    wlr_scene_buffer_set_buffer_with_damage(layer->node, &dst->base, NULL);
    // 交出去之后就不要这份引用了：scene 释放完，对象和它的 GL 纹理一起销毁。
    cpu_buffer_put(&dst);
    request_frame();
}

void wswlr_layer_present(wswlr_layer* layer, uint32_t const* pixels,
    uint32_t pitch_bytes, uint32_t w, uint32_t h)
{
    layer_present_rects(layer, pixels, pitch_bytes, w, h, NULL, 0);
}

void wswlr_layer_set_position(wswlr_layer* layer, int x, int y)
{
    if (layer && layer->node)
        wlr_scene_node_set_position(&layer->node->node, x, y);
}

void wswlr_layer_set_enabled(wswlr_layer* layer, bool enabled)
{
    if (layer && layer->node)
        wlr_scene_node_set_enabled(&layer->node->node, enabled);
    request_frame();
}

// ── 直接绘制：窗口层把自己的像素画进 wlr_buffer ─────────────────────────────
// 两块轮转 + idle 判定，和桌面层一样只写「渲染器当前没在读」的那块。
void* wswlr_layer_begin_paint(wswlr_layer* layer, uint32_t w, uint32_t h,
    uint32_t* stride_bytes)
{
    if (!layer || !layer->node || w == 0 || h == 0)
        return NULL;
    // 上一帧还没 commit：丢掉（正常情况下不会发生）。
    cpu_buffer_put(&layer->painting);

    struct cpu_buffer* b = cpu_buffer_create(w, h);
    if (!b)
        return NULL;
    layer->painting = b;
    layer->width = w;
    layer->height = h;
    if (stride_bytes)
        *stride_bytes = b->stride;
    return b->data;
}

void wswlr_layer_commit(wswlr_layer* layer, int x, int y)
{
    if (!layer || !layer->node || !layer->painting)
        return;
    struct cpu_buffer* b = layer->painting;
    layer->painting = NULL;
    b->idle = false;
    b->submitted = true;
    wlr_scene_node_set_position(&layer->node->node, x, y);
    wlr_scene_node_set_enabled(&layer->node->node, true);
    wlr_scene_buffer_set_buffer_with_damage(layer->node, &b->base, NULL);
    // 新对象已经交出去，这份引用可以放掉（纹理随对象销毁，下次重新上传）。
    cpu_buffer_put(&b);
    request_frame();
}

void wswlr_layer_raise(wswlr_layer* layer)
{
    if (layer && layer->node)
        wlr_scene_node_raise_to_top(&layer->node->node);
}

void wswlr_layer_destroy(wswlr_layer* layer)
{
    if (!layer)
        return;
    if (getenv("WSWLR_LAYER_DEBUG"))
        fprintf(stderr, "ws-wlr: layer destroy %p (%ux%u)\n", (void*)layer, layer->width, layer->height);
    wl_list_remove(&layer->link);
    if (layer->node)
        wlr_scene_node_destroy(&layer->node->node);
    cpu_buffer_put(&layer->painting);
    free(layer);
}

// ── 桌面层（兼容旧 API：就是最底下那个全屏 layer）──────────────────────────
wswlr_desktop_surface* wswlr_compositor_create_desktop(wswlr_compositor* c,
    uint32_t w, uint32_t h)
{
    if (!c || !c->scene)
        return NULL;
    struct wswlr_desktop_surface* ds = calloc(1, sizeof(*ds));
    if (!ds)
        return NULL;
    ds->layer = layer_create(c, 0, 0, w, h);
    if (!ds->layer) {
        free(ds);
        return NULL;
    }
    wlr_scene_node_lower_to_bottom(&ds->layer->node->node);
    c->desktop = ds;
    return ds;
}

void wswlr_compositor_present_desktop(wswlr_compositor* c,
    wswlr_desktop_surface* ds, uint32_t const* pixels, uint32_t pitch_bytes,
    uint32_t w, uint32_t h)
{
    (void)c;
    if (!ds || !ds->layer)
        return;
    wswlr_layer_present(ds->layer, pixels, pitch_bytes, w, h);
    wlr_scene_node_lower_to_bottom(&ds->layer->node->node);
}

void wswlr_compositor_present_desktop_rects(wswlr_compositor* c,
    wswlr_desktop_surface* ds, int buffer_index, uint32_t const* pixels,
    uint32_t pitch_bytes, uint32_t w, uint32_t h,
    wswlr_rect const* rects, size_t rect_count)
{
    (void)c;
    (void)buffer_index; // 单缓冲：快照永远完整，半区概念已经不存在
    if (!ds || !ds->layer)
        return;
    layer_present_rects(ds->layer, pixels, pitch_bytes, w, h, rects, rect_count);
    wlr_scene_node_lower_to_bottom(&ds->layer->node->node);
}

void wswlr_compositor_resize_desktop(wswlr_compositor* c,
    wswlr_desktop_surface* ds, uint32_t w, uint32_t h)
{
    (void)c;
    if (!ds || !ds->layer)
        return;
    // 每次 present 都是新建 buffer，尺寸变化不需要额外处理；这里只记一下，
    // 下次 present 会按新尺寸来。
    ds->layer->width = w;
    ds->layer->height = h;
}

// ── 纯 C ABI ───────────────────────────────────────────────────────────────
wswlr_compositor* wswlr_compositor_create(uint32_t width, uint32_t height,
    bool headless, bool xwayland, wswlr_callbacks const* cb)
{
    struct wswlr_compositor* c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    g = c;
    if (cb)
        c->callbacks = *cb;
    wl_list_init(&c->windows);
    wl_list_init(&c->layers);
    wl_list_init(&c->keyboards);

    wlr_log_init(getenv("WSWLR_VERBOSE") ? WLR_DEBUG : WLR_ERROR, NULL);
    c->display = wl_display_create();
    if (!c->display)
        goto fail;
    struct wl_event_loop* loop = wl_display_get_event_loop(c->display);

    if (headless) {
        c->backend = wlr_headless_backend_create(loop);
        if (c->backend)
            wlr_headless_add_output(c->backend, (int)width, (int)height);
    } else {
        // 我们是合成器本体，不是嵌套客户端：serenitid 导出的 WAYLAND_DISPLAY
        // 会让 wlr_backend_autocreate 去连「远程合成器」并直接失败。
        bool const nested = getenv("WSWLR_NESTED") && strcmp(getenv("WSWLR_NESTED"), "0");
        if (!nested) {
            unsetenv("WAYLAND_DISPLAY");
            unsetenv("DISPLAY");
        }
        struct wlr_session* session = NULL;
        c->backend = wlr_backend_autocreate(loop, &session);
    }
    if (!c->backend) {
        wlr_log(WLR_ERROR, "ws-wlr: backend init failed");
        goto fail;
    }

    c->renderer = wlr_renderer_autocreate(c->backend);
    if (!c->renderer)
        goto fail;
    wlr_renderer_init_wl_display(c->renderer, c->display);
    c->allocator = wlr_allocator_autocreate(c->backend, c->renderer);
    c->compositor = wlr_compositor_create(c->display, 6, c->renderer);
    // Advertise zwp_linux_dmabuf_v1 so foreign Wayland/Xwayland clients can submit
    // GPU buffers directly. Buffers whose formats/modifiers Mesa can import are
    // used without a CPU copy; SHM remains the fallback for others.
    c->linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(c->display, 4, c->renderer);
    if (!c->linux_dmabuf)
        wlr_log(WLR_ERROR, "ws-wlr: linux-dmabuf global unavailable; foreign clients will use SHM");
    else
        wlr_log(WLR_INFO, "ws-wlr: linux-dmabuf enabled for foreign clients");
    wlr_data_device_manager_create(c->display);
    c->seat = wlr_seat_create(c->display, "seat0");
    wlr_seat_set_capabilities(c->seat, WL_SEAT_CAPABILITY_POINTER);
    c->scene = wlr_scene_create();
    c->native_tree = wlr_scene_tree_create(&c->scene->tree);
    c->layout = wlr_output_layout_create(c->display);
    wlr_scene_attach_output_layout(c->scene, c->layout);
    c->xdg_shell = wlr_xdg_shell_create(c->display, 6);
    c->decoration = wlr_xdg_decoration_manager_v1_create(c->display);

    c->cursor = wlr_cursor_create();
    if (c->cursor) {
        wlr_cursor_attach_output_layout(c->cursor, c->layout);
        c->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
        if (c->cursor_mgr && !getenv("WSWLR_NO_CURSOR_IMAGE"))
            wlr_cursor_set_xcursor(c->cursor, c->cursor_mgr, "default");
        c->cursor_motion.notify = on_cursor_motion;
        wl_signal_add(&c->cursor->events.motion, &c->cursor_motion);
        c->cursor_motion_absolute.notify = on_cursor_motion_absolute;
        wl_signal_add(&c->cursor->events.motion_absolute, &c->cursor_motion_absolute);
        c->cursor_button.notify = on_cursor_button;
        wl_signal_add(&c->cursor->events.button, &c->cursor_button);
        c->cursor_axis.notify = on_cursor_axis;
        wl_signal_add(&c->cursor->events.axis, &c->cursor_axis);
        c->cursor_frame.notify = on_cursor_frame;
        wl_signal_add(&c->cursor->events.frame, &c->cursor_frame);
    }

    c->new_input.notify = on_new_input;
    wl_signal_add(&c->backend->events.new_input, &c->new_input);
    c->new_output.notify = on_new_output;
    wl_signal_add(&c->backend->events.new_output, &c->new_output);
    c->new_xdg_surface.notify = on_new_xdg_surface;
    wl_signal_add(&c->xdg_shell->events.new_surface, &c->new_xdg_surface);
    c->new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&c->xdg_shell->events.new_toplevel, &c->new_toplevel);
    if (c->decoration) {
        c->new_decoration.notify = on_new_decoration;
        wl_signal_add(&c->decoration->events.new_toplevel_decoration, &c->new_decoration);
    }

    if (xwayland) {
        c->xwayland = wlr_xwayland_create(c->display, c->compositor, true);
        if (c->xwayland) {
            wlr_xwayland_set_seat(c->xwayland, c->seat);
            c->new_xwayland_surface.notify = on_new_xwayland_surface;
            wl_signal_add(&c->xwayland->events.new_surface, &c->new_xwayland_surface);
        }
    }

    // socket 名必须和会话里既有消费者一致（serenitid 给应用导出的是
    // wayland-serenity-0）。headless 调试默认 wayland-0，避免撞名。
    char const* socket_name = getenv("WSWLR_SOCKET");
    if (!socket_name || !*socket_name)
        socket_name = headless ? "wayland-0" : "wayland-serenity-0";
    c->socket = socket_name;
    if (wl_display_add_socket(c->display, socket_name) != 0)
        c->socket = wl_display_add_socket_auto(c->display);
    if (!c->socket) {
        wlr_log(WLR_ERROR, "ws-wlr: add_socket failed");
        goto fail;
    }
    setenv("WAYLAND_DISPLAY", c->socket, 1);

    if (!wlr_backend_start(c->backend)) {
        wlr_log(WLR_ERROR, "ws-wlr: backend start failed");
        goto fail;
    }

    char const* shot = getenv("WSWLR_SHOT");
    if (shot && *shot) {
        char const* delay_env = getenv("WSWLR_SHOT_DELAY_MS");
        int delay = delay_env ? atoi(delay_env) : 5000;
        c->shot_path = strdup(shot);
        c->shot_timer = wl_event_loop_add_timer(loop, on_shot_timer, c);
        if (c->shot_timer)
            wl_event_source_timer_update(c->shot_timer, delay > 0 ? delay : 5000);
    }
    if (getenv("WSWLR_FAKE_CLICK") || getenv("WSWLR_FAKE_KEYS")) {
        char const* delay_env = getenv("WSWLR_FAKE_DELAY_MS");
        int delay = delay_env ? atoi(delay_env) : 6000;
        c->fake_timer = wl_event_loop_add_timer(loop, on_fake_timer, c);
        if (c->fake_timer)
            wl_event_source_timer_update(c->fake_timer, delay > 0 ? delay : 6000);
    }
    return c;

fail:
    wswlr_compositor_destroy(c);
    return NULL;
}

void wswlr_compositor_destroy(wswlr_compositor* c)
{
    if (!c)
        return;
    if (c->backend)
        wlr_backend_destroy(c->backend);
    if (c->display)
        wl_display_destroy(c->display);
    free(c);
    if (g == c)
        g = NULL;
}

char const* wswlr_compositor_socket(wswlr_compositor* c)
{
    return c ? c->socket : NULL;
}

int wswlr_compositor_event_fd(wswlr_compositor* c)
{
    return wl_event_loop_get_fd(wl_display_get_event_loop(c->display));
}

void wswlr_compositor_dispatch(wswlr_compositor* c, int timeout_ms)
{
    wl_event_loop_dispatch(wl_display_get_event_loop(c->display), timeout_ms);
    wl_display_flush_clients(c->display);
}
