/* Minimal Wayland client: verifies the bridge event loop is dispatching.
 *
 * A cold-start deadlock (bridge alive but never answering get_registry) makes
 * wl_display_roundtrip() hang here with zero globals. Any healthy bridge
 * answers immediately with >= 10 globals. See WS_MENU_MAPPING.md §6.
 *
 * Build (host):
 *   gcc -I<WlrootsHost> tests/wl-roundtrip.c -lwayland-client -o wl-roundtrip
 */
#include <wayland-client.h>
#include <stdio.h>
#include <unistd.h>

static int s_globals = 0;

static void global_cb(void*, struct wl_registry*, uint32_t, const char*, uint32_t)
{
    ++s_globals;
}

static void global_remove_cb(void*, struct wl_registry*, uint32_t)
{
}

static const struct wl_registry_listener s_listener = {
    .global = global_cb,
    .global_remove = global_remove_cb,
};

int main(void)
{
    alarm(6);
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        printf("CONNECT_FAILED\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_listener, NULL);
    wl_display_roundtrip(display);
    printf("ROUNDTRIP_OK globals=%d\n", s_globals);
    wl_display_disconnect(display);
    return s_globals >= 10 ? 0 : 1;
}
