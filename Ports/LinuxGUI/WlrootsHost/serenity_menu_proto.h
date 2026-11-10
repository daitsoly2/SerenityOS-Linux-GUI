/* serenity_menu_v1 — protocol module API (pure C, shared with main.cpp).
 *
 * The protocol module owns the Wayland objects; main.cpp owns the native
 * GUI::Menu objects.  They are linked through the opaque `menu` handle and
 * serenity_menu_set_native()/serenity_menu_native().
 */
#ifndef SERENITY_MENU_PROTO_H
#define SERENITY_MENU_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_resource;
struct serenity_menu_proto;

struct serenity_menu_callbacks {
    void* user;

    /* A client created a menu. `surface` is the anchor wl_surface (may be
     * NULL); x/y/anchor_width/anchor_height are surface-local coordinates.
     * The callback should create a GUI::Menu and attach it with
     * serenity_menu_set_native(). */
    void (*create_menu)(void* user, void* menu, struct wl_resource* surface,
        int x, int y, int anchor_width, int anchor_height);

    void (*add_item)(void* user, void* menu, uint32_t identifier,
        const char* text, const char* shortcut, int enabled,
        int checkable, int checked, void* submenu);
    void (*add_separator)(void* user, void* menu);
    void (*add_icon)(void* user, void* menu, uint32_t identifier,
        int fd, uint32_t width, uint32_t height, uint32_t stride);
    void (*update_item)(void* user, void* menu, uint32_t identifier,
        int enabled, int checked);
    void (*popup)(void* user, void* menu, uint32_t serial);
    void (*dismiss)(void* user, void* menu);
    void (*destroy)(void* user, void* menu);
};

struct serenity_menu_proto* serenity_menu_proto_create(struct wl_display* display,
    const struct serenity_menu_callbacks* cb);
void serenity_menu_proto_destroy(struct serenity_menu_proto* p);

void* serenity_menu_native(void* menu);
void serenity_menu_set_native(void* menu, void* native);

void serenity_menu_send_activated(void* menu, uint32_t identifier);
void serenity_menu_send_dismissed(void* menu);
void serenity_menu_send_hovered(void* menu, uint32_t identifier, uint32_t entered);
void serenity_menu_flush(struct serenity_menu_proto* p);

#ifdef __cplusplus
}
#endif

#endif /* SERENITY_MENU_PROTO_H */
