/* Regression probe for serenity_menu_v1 (P0 + P1).
 *
 * Build (host):
 *   gcc -I../../LinuxGUI/WlrootsHost \
 *       tests/menu-probe.c \
 *       ../../LinuxGUI/WlrootsHost/protocol/serenity-menu-v1-client.c \
 *       -lwayland-client -o build/menu-probe
 * Run in VM with WAYLAND_DISPLAY=wayland-serenity-0.
 *
 * Verifies:
 *   P0  the global is advertised, bind succeeds, create_menu / add_item /
 *       add_separator / popup / dismiss / destroy cross the C <-> C++ boundary
 *       (check "ws-menu:" lines in /tmp/serenity-session.log).
 *   P1  the native WindowServer menu really appears (screenshot it), and
 *       protocol events come back: click an item -> ACTIVATED, dismiss ->
 *       DISMISSED.  The probe keeps the connection alive for HOLD seconds so
 *       the menu stays on screen and events can be dispatched.
 *
 * The menu content deliberately mirrors the native Notepad "Edit" menu so the
 * screenshot can be compared pixel-for-pixel with WindowServer/Menu.cpp output.
 * Icons are P3 and therefore absent here.
 *
 * Env: SERENITY_MENU_HOLD_SECONDS (default 20) controls how long the menu
 * stays up; SERENITY_MENU_X / _Y override the popup anchor (screen coords
 * because this probe has no wl_surface).
 */
#include <wayland-client.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "protocol/serenity-menu-v1-client.h"

static struct serenity_menu_manager_v1* s_manager;

static void global_cb(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    (void) data;
    if (strcmp(interface, "serenity_menu_manager_v1") == 0) {
        s_manager = (struct serenity_menu_manager_v1*)
            wl_registry_bind(registry, name, &serenity_menu_manager_v1_interface, 1);
        printf("BOUND name=%u version=%u\n", name, version);
        fflush(stdout);
    }
}

static void global_remove_cb(void* data, struct wl_registry* registry, uint32_t name)
{
    (void) data; (void) registry; (void) name;
}

static const struct wl_registry_listener s_registry_listener = {
    .global = global_cb,
    .global_remove = global_remove_cb,
};

static void activated_cb(void* data, struct serenity_menu_v1* menu, uint32_t identifier)
{
    (void) data; (void) menu;
    printf("ACTIVATED id=%u\n", identifier);
    fflush(stdout);
}

static void dismissed_cb(void* data, struct serenity_menu_v1* menu)
{
    (void) data; (void) menu;
    printf("DISMISSED\n");
    fflush(stdout);
}

static void hovered_cb(void* data, struct serenity_menu_v1* menu,
    uint32_t identifier, uint32_t entered)
{
    (void) data; (void) menu;
    printf("HOVERED id=%u entered=%u\n", identifier, entered);
    fflush(stdout);
}

static const struct serenity_menu_v1_listener s_menu_listener = {
    .activated = activated_cb,
    .dismissed = dismissed_cb,
    .hovered = hovered_cb,
};

/* Standard Wayland event loop: prepare_read -> poll -> read_events ->
 * dispatch_pending. A bare wl_display_dispatch() can swallow events that were
 * already buffered by an earlier roundtrip, which would make ACTIVATED look
 * missing even when the bridge sent it. */
static void pump(struct wl_display* display, int duration_ms)
{
    struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
    int left = duration_ms;
    while (left > 0) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        int step = left < 100 ? left : 100;
        int rc = poll(&pfd, 1, step);
        left -= step;
        if (rc < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
    }
}

static int env_int(const char* name, int fallback)
{
    const char* v = getenv(name);
    return (v && *v) ? atoi(v) : fallback;
}

int main(void)
{
    int hold_seconds = env_int("SERENITY_MENU_HOLD_SECONDS", 20);
    int anchor_x = env_int("SERENITY_MENU_X", 300);
    int anchor_y = env_int("SERENITY_MENU_Y", 200);

    alarm((unsigned) (hold_seconds + 15));
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        printf("CONNECT_FAILED\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!s_manager) {
        printf("NO_MANAGER\n");
        wl_display_disconnect(display);
        return 1;
    }

    /* surface=NULL: the probe has no wl_surface, so x/y are screen coords.
     * A real GTK4 shim passes its anchor wl_surface instead (P1b). */
    struct serenity_menu_v1* menu = serenity_menu_manager_v1_create_menu(
        s_manager, NULL, anchor_x, anchor_y, 40, 24);
    serenity_menu_v1_add_listener(menu, &s_menu_listener, NULL);

    serenity_menu_v1_add_item(menu, 1, "Undo", "Ctrl+Z", 1, 0, 0, NULL);
    serenity_menu_v1_add_item(menu, 2, "Redo", "Ctrl+Y", 0, 0, 0, NULL);
    serenity_menu_v1_add_separator(menu);
    serenity_menu_v1_add_item(menu, 3, "Cut", "Ctrl+X", 1, 0, 0, NULL);
    serenity_menu_v1_add_item(menu, 4, "Copy", "Ctrl+C", 1, 0, 0, NULL);
    serenity_menu_v1_add_item(menu, 5, "Paste", "Ctrl+V", 1, 0, 0, NULL);
    serenity_menu_v1_add_separator(menu);
    serenity_menu_v1_add_item(menu, 6, "Select All", "Ctrl+A", 1, 0, 0, NULL);

    serenity_menu_v1_popup(menu, 42);
    wl_display_roundtrip(display);
    printf("REQUESTS_SENT\n");
    printf("MENU_VISIBLE_HOLD=%ds (click an item to test ACTIVATED)\n", hold_seconds);
    fflush(stdout);

    pump(display, hold_seconds * 1000);

    /* Dismissing from the client also exercises the send_dismissed path. */
    serenity_menu_v1_dismiss(menu);
    wl_display_roundtrip(display);
    pump(display, 400);

    printf("DONE\n");
    fflush(stdout);
    serenity_menu_v1_destroy(menu);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
