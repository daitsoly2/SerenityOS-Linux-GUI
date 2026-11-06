/*
 * popuptest —— xdg_popup 首帧 configure 契约验证客户端
 *
 * 契约（WAYLAND_POPUP_CONFIGURE_SCHEME.md §4）：popup 的 wl_surface 必须先做一次
 * **无 buffer 的实化 commit**，合成器（wlroots）才会把 xdg_surface 置 initialized
 * 并在首次 commit 之后 schedule_configure；客户端随后收 configure → ack → 才
 * attach buffer 提交。任何「先 attach buffer 再等 configure」的写法在 wlroots 下
 * 都是死锁（host 断言 abort + 客户端 "never been configured"）。
 *
 * 退出码：0 = 三条路径（toplevel / xdg_popup / wl_subsurface）全部按契约通过；
 *         2 = popup 在超时内没收到 configure（方案未生效）。
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>

struct wl_display *d;
struct wl_compositor *compositor;
struct xdg_wm_base *wm_base;
struct wl_subcompositor *subcompositor;
struct wl_shm *shm;

static int top_configure_count;
static int popup_configure_count;
static int popup_done_count;

static void reg_global(void *data, struct wl_registry *reg, uint32_t id,
                      const char *iface, uint32_t ver)
{
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_subcompositor_interface.name))
        subcompositor = wl_registry_bind(reg, id, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wmb_listener = { wm_base_ping };

/* toplevel / popup 共用：收到 xdg_surface.configure 立刻 ack（wlroots 以此置
 * base->configured=true，之后带 buffer 的提交才不被 reject）。 */
static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    top_configure_count++;
}
static const struct xdg_surface_listener xdg_surf_listener = { xdg_surface_configure };

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *s) { (void)w; (void)h; }
static void toplevel_close(void *d, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener top_listener = { toplevel_configure, toplevel_close };

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y,
                            int32_t w, int32_t h)
{
    popup_configure_count++;
    fprintf(stderr, "popup configure #%d: x=%d y=%d w=%d h=%d\n",
        popup_configure_count, x, y, w, h);
}
static void popup_done(void *d, struct xdg_popup *p) { popup_done_count++; }
static const struct xdg_popup_listener popup_listener = { popup_configure, popup_done };

static struct wl_buffer *make_buffer(int w, int h, uint32_t color)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("buf", 0);
    if (fd < 0) return NULL;
    ftruncate(fd, size);
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint32_t *px = m;
    for (int i = 0; i < w * h; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(m, size);
    close(fd);
    return buf;
}

/* 有界等待：poll 到可读就 dispatch，直到 counter >= target 或超时（毫秒）。
 * 注意不能用 wl_display_dispatch_pending 轮询 —— 它只派发「已读入队列」的事件，
 * 不读 socket，单靠它永远等不到 compositor 发来的 configure。 */
static int wait_until_at_least(int *counter, int target, int timeout_ms)
{
    struct pollfd pfd = { wl_display_get_fd(d), POLLIN, 0 };
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (*counter < target) {
        if (wl_display_dispatch_pending(d) < 0)
            return -1;
        if (wl_display_flush(d) < 0 && errno != EAGAIN)
            return -1;
        int r = poll(&pfd, 1, 20);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r > 0 && (pfd.revents & (POLLIN | POLLHUP)))
            if (wl_display_dispatch(d) < 0)
                return -1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms)
            return 0;
    }
    return 1;
}

int main(void)
{
    d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(d);
    if (!compositor || !wm_base) { fprintf(stderr, "missing globals\n"); return 1; }
    if (!shm) { fprintf(stderr, "no shm\n"); return 1; }
    if (!subcompositor) fprintf(stderr, "WARNING: no wl_subcompositor global\n");
    xdg_wm_base_add_listener(wm_base, &wmb_listener, NULL);

    /* —— toplevel —— */
    struct wl_surface *top = wl_compositor_create_surface(compositor);
    struct xdg_surface *top_xdg = xdg_wm_base_get_xdg_surface(wm_base, top);
    xdg_surface_add_listener(top_xdg, &xdg_surf_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(top_xdg);
    xdg_toplevel_add_listener(tl, &top_listener, NULL);
    xdg_toplevel_set_title(tl, "popuptest");
    wl_surface_commit(top);                       /* 无 buffer 实化 */
    if (wait_until_at_least(&top_configure_count, 1, 2000) != 1) {
        fprintf(stderr, "FAIL: toplevel got no configure\n");
        return 2;
    }
    fprintf(stderr, "toplevel configured\n");
    struct wl_buffer *buf = make_buffer(200, 150, 0xFF3366CC);
    wl_surface_attach(top, buf, 0, 0);
    wl_surface_commit(top);
    wl_display_roundtrip(d);
    fprintf(stderr, "toplevel mapped\n");

    /* —— xdg_popup (Firefox 菜单路径) —— */
    struct wl_surface *ps = wl_compositor_create_surface(compositor);
    struct xdg_surface *p_xdg = xdg_wm_base_get_xdg_surface(wm_base, ps);
    xdg_surface_add_listener(p_xdg, &xdg_surf_listener, NULL);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
    xdg_positioner_set_size(pos, 150, 80);
    xdg_positioner_set_anchor_rect(pos, 0, 0, 10, 10);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, 10, 10);
    struct xdg_popup *pop = xdg_surface_get_popup(p_xdg, top_xdg, pos);
    xdg_popup_add_listener(pop, &popup_listener, NULL);
    xdg_positioner_destroy(pos);

    /* 关键一步（方案 §4）：先做无 buffer 的实化 commit，不要让 configure 先于
     * 任何 commit —— wlroots 只在首次 commit 时置 initialized，host 也只在
     * 首次 commit 之后才 schedule_configure。 */
    wl_surface_commit(ps);
    if (wait_until_at_least(&popup_configure_count, 1, 2000) != 1) {
        fprintf(stderr, "FAIL: xdg_popup got no configure within 2s "
                        "(host did not schedule_configure after first commit)\n");
        return 2;
    }
    fprintf(stderr, "xdg_popup configured\n");

    struct wl_buffer *pbuf = make_buffer(150, 80, 0xFF22CC22);
    wl_surface_attach(ps, pbuf, 0, 0);
    wl_surface_commit(ps);
    wl_display_roundtrip(d);
    fprintf(stderr, "xdg_popup created (mapped)\n");

    /* —— wl_subsurface (GTK/gedit 菜单路径) —— */
    if (subcompositor) {
        struct wl_surface *ss = wl_compositor_create_surface(compositor);
        struct wl_buffer *sbuf = make_buffer(80, 60, 0xFFCCCC22);
        wl_surface_attach(ss, sbuf, 0, 0);
        wl_surface_commit(ss);                       /* 必须先 commit 一次才能 get_subsurface */
        wl_display_roundtrip(d);
        struct wl_subsurface *sub = wl_subcompositor_get_subsurface(subcompositor, ss, top);
        wl_subsurface_set_position(sub, 30, 30);
        wl_surface_commit(top);
        wl_display_roundtrip(d);
        fprintf(stderr, "subsurface created\n");
    }

    /* —— 多轮开/关 popup：验证 configure_sent 的生命周期与 destroy 摘链 —— */
    for (int i = 0; i < 3; i++) {
        int before = popup_configure_count;
        struct wl_surface *ls = wl_compositor_create_surface(compositor);
        struct xdg_surface *l_xdg = xdg_wm_base_get_xdg_surface(wm_base, ls);
        xdg_surface_add_listener(l_xdg, &xdg_surf_listener, NULL);
        struct xdg_positioner *lp = xdg_wm_base_create_positioner(wm_base);
        xdg_positioner_set_size(lp, 120, 60);
        xdg_positioner_set_anchor_rect(lp, 0, 0, 8, 8);
        xdg_positioner_set_anchor(lp, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(lp, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        struct xdg_popup *lpop = xdg_surface_get_popup(l_xdg, top_xdg, lp);
        xdg_popup_add_listener(lpop, &popup_listener, NULL);
        xdg_positioner_destroy(lp);

        wl_surface_commit(ls);                    /* 无 buffer 实化 */
        int wait_target = before + 1;
        if (wait_until_at_least(&popup_configure_count, wait_target, 2000) != 1) {
            fprintf(stderr, "FAIL: loop popup #%d configure=%d (expected %d)\n",
                i, popup_configure_count, wait_target);
            return 2;
        }
        struct wl_buffer *lb = make_buffer(120, 60, 0xFFCC66AA);
        wl_surface_attach(ls, lb, 0, 0);
        wl_surface_commit(ls);
        wl_display_roundtrip(d);

        /* 关掉：destroy popup 等价于发 popup_done，host 应摘掉监听并不崩。
         * 销毁顺序必须 popup → xdg_surface → wl_surface。 */
        xdg_popup_destroy(lpop);
        xdg_surface_destroy(l_xdg);
        wl_surface_destroy(ls);
        wl_display_roundtrip(d);
        usleep(60 * 1000);
    }
    fprintf(stderr, "popup open/close loop x3 done (popup_configure=%d)\n", popup_configure_count);

    for (int i = 0; i < 5; i++) { wl_display_dispatch_pending(d); wl_display_flush(d); usleep(100000); }
    fprintf(stderr, "client done: top_configure=%d popup_configure=%d popup_done=%d; "
                    "leaving surfaces up 3s for host logging\n",
        top_configure_count, popup_configure_count, popup_done_count);
    sleep(3);
    return 0;
}
