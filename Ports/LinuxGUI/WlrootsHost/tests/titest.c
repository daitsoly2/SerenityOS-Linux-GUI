#define _GNU_SOURCE
/* Minimal text-input-v3 client: follows the xdg configure/ack flow, maps a
 * window, then enables the text input. Logs every IME event. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wmBase;
static struct wl_seat* seat;
static struct zwp_text_input_manager_v3* tiManager;
static struct zwp_text_input_v3* textInput;
static struct wl_surface* surface;
static struct xdg_surface* xdgSurface;
static struct xdg_toplevel* toplevel;
static struct wl_buffer* buffer;
static int mapped;

static struct wl_buffer*
make_buffer(int w, int h);

static void
wm_base_ping(void* data, struct xdg_wm_base* b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static struct wl_keyboard* keyboard;
static void
kb_keymap(void* d, struct wl_keyboard* k, uint32_t fmt, int32_t fd, uint32_t sz)
{ printf("KB keymap\n"); fflush(stdout); if (fd >= 0) close(fd); }
static void
kb_enter(void* d, struct wl_keyboard* k, uint32_t serial,
	struct wl_surface* sf, struct wl_array* keys)
{ printf("KB enter\n"); fflush(stdout); }
static void
kb_leave(void* d, struct wl_keyboard* k, uint32_t serial,
	struct wl_surface* sf)
{ printf("KB leave\n"); fflush(stdout); }
static void
kb_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time,
	uint32_t key, uint32_t state)
{ printf("KB key %u state=%u\n", key, state); fflush(stdout); }
static void
kb_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t dep,
	uint32_t lat, uint32_t lock, uint32_t grp)
{}
static void
kb_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay)
{}
static const struct wl_keyboard_listener kb_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat
};

static void
ti_enter(void* d, struct zwp_text_input_v3* ti, struct wl_surface* s)
{ printf("TI enter\n"); fflush(stdout); }
static void
ti_leave(void* d, struct zwp_text_input_v3* ti, struct wl_surface* s)
{ printf("TI leave\n"); fflush(stdout); }
static void
ti_preedit(void* d, struct zwp_text_input_v3* ti, const char* text,
	int32_t cb, int32_t ce)
{ printf("TI preedit '%s' [%d,%d]\n", text ? text : "", cb, ce); fflush(stdout); }
static void
ti_commit(void* d, struct zwp_text_input_v3* ti, const char* text)
{
	printf("TI commit '%s'\n", text ? text : "");
	fflush(stdout);
	/* Show the committed text in the window title so it is visible in a
	 * screenshot (the VOS title bar uses the system font, which has CJK). */
	static char title[512] = "";
	if (text != NULL && toplevel != NULL) {
		size_t used = strlen(title);
		if (used + strlen(text) < sizeof(title) - 1)
			strcat(title, text);
		xdg_toplevel_set_title(toplevel, title);
	}
}
static void
ti_delete(void* d, struct zwp_text_input_v3* ti, uint32_t b, uint32_t a)
{ printf("TI delete %u %u\n", b, a); fflush(stdout); }
static void
ti_done(void* d, struct zwp_text_input_v3* ti, uint32_t serial)
{ printf("TI done serial=%u\n", serial); fflush(stdout); }
static const struct zwp_text_input_v3_listener ti_listener = {
	ti_enter, ti_leave, ti_preedit, ti_commit, ti_delete, ti_done
};

static void
xdg_surface_configure(void* data, struct xdg_surface* s, uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);

	if (!mapped) {
		mapped = 1;
		buffer = make_buffer(400, 200);
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, 400, 200);
		wl_surface_commit(surface);

		textInput = zwp_text_input_manager_v3_get_text_input(tiManager, seat);
		zwp_text_input_v3_add_listener(textInput, &ti_listener, NULL);
		zwp_text_input_v3_enable(textInput);
		zwp_text_input_v3_set_content_type(textInput,
			ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
			ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
		zwp_text_input_v3_commit(textInput);
		printf("mapped, text input enabled\n");
		fflush(stdout);
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_configure
};

static void toplevel_configure(void* d, struct xdg_toplevel* t, int32_t w,
	int32_t h, struct wl_array* states) {}
static void toplevel_close(void* d, struct xdg_toplevel* t) { exit(0); }
static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure, toplevel_close
};

static void
registry_global(void* data, struct wl_registry* reg, uint32_t name,
	const char* iface, uint32_t version)
{
	if (strcmp(iface, "wl_compositor") == 0)
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, "wl_shm") == 0)
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, "xdg_wm_base") == 0) {
		wmBase = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wmBase, &wm_base_listener, NULL);
	} else if (strcmp(iface, "wl_seat") == 0)
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
	else if (strcmp(iface, "zwp_text_input_manager_v3") == 0)
		tiManager = wl_registry_bind(reg, name,
			&zwp_text_input_manager_v3_interface, 1);
}
static void registry_remove(void* d, struct wl_registry* r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = {
	registry_global, registry_remove
};

static struct wl_buffer*
make_buffer(int w, int h)
{
	int stride = w * 4;
	int size = stride * h;
	int fd = memfd_create("titest", MFD_CLOEXEC);
	if (fd < 0) return NULL;
	if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
	uint32_t* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { close(fd); return NULL; }
	for (int i = 0; i < w * h; i++) p[i] = 0xff336698;	/* VOS blue */
	munmap(p, size);
	struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

int
main(void)
{
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }

	struct wl_registry* reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !wmBase || !seat || !tiManager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	printf("globals ok\n");
	fflush(stdout);

	keyboard = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(keyboard, &kb_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
	xdg_surface_add_listener(xdgSurface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "IME test");

	/* Empty commit to trigger the initial configure. */
	wl_surface_commit(surface);

	int r;
	while ((r = wl_display_dispatch(display)) != -1) {
	}
	fprintf(stderr, "titest: dispatch returned %d (errno=%d: %s)\n", r, errno, strerror(errno));
	return 0;
}
