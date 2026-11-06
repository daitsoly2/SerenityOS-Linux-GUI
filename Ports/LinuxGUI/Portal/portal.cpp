/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-portal - the one portal interface GTK actually needs.

#include <unistd.h>
//
// On Wayland GTK asks org.freedesktop.portal.Settings for
// org.freedesktop.appearance/color-scheme and /accent-color at startup. If
// nothing owns that bus name the call sits on a 25 second D-Bus timeout, which
// is exactly the stall every GTK app had when opened from the start menu.
//
// The real xdg-desktop-portal cannot serve this desktop: it blocks inside
// g_initable_new waiting for a bus call that never returns here (no document
// portal, no session manager it recognises). File choosers and screen sharing
// through a portal are not needed either - GtkFileChooser works in-process and
// the desktop has its own dialogs - so Settings is all we implement.
//
// Answers are static: one theme, one accent colour, no dark-mode switch. If a
// theme switch is wired up later, emit SettingChanged from here.

#include <systemd/sd-bus.h>

#include <cstdio>
#include <cstring>
#include <errno.h>

namespace {

// Serenity's Default theme: light-grey faces, blue selection.
constexpr unsigned APPEARANCE_COLOR_SCHEME = 0; // 0 = no preference, 1 = prefer dark
constexpr unsigned APPEARANCE_CONTRAST = 0;     // 0 = no preference
constexpr double ACCENT_R = 0.129;
constexpr double ACCENT_G = 0.400;
constexpr double ACCENT_B = 0.674;

char const* const PORTAL_PATH = "/org/freedesktop/portal/desktop";
char const* const SETTINGS_INTERFACE = "org.freedesktop.portal.Settings";
char const* const PORTAL_NAME = "org.freedesktop.portal.Desktop";
char const* const APPEARANCE_NAMESPACE = "org.freedesktop.appearance";

char const* signature_for_key(char const* key)
{
    if (strcmp(key, "color-scheme") == 0 || strcmp(key, "contrast") == 0)
        return "u";
    if (strcmp(key, "accent-color") == 0)
        return "(ddd)";
    return nullptr;
}

int append_value(sd_bus_message* message, char const* key, char const* signature)
{
    if (strcmp(signature, "u") == 0) {
        auto value = strcmp(key, "contrast") == 0 ? APPEARANCE_CONTRAST : APPEARANCE_COLOR_SCHEME;
        return sd_bus_message_append(message, "u", value);
    }
    return sd_bus_message_append(message, "(ddd)", ACCENT_R, ACCENT_G, ACCENT_B);
}

int method_read(sd_bus_message* call, void*, sd_bus_error* error)
{
    char const* ns = nullptr;
    char const* key = nullptr;
    if (sd_bus_message_read(call, "ss", &ns, &key) < 0)
        return -EINVAL;

    if (!ns || strcmp(ns, APPEARANCE_NAMESPACE) != 0)
        return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Unknown namespace '%s'", ns ? ns : "");

    auto const* signature = key ? signature_for_key(key) : nullptr;
    if (!signature)
        return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Unknown key '%s'", key ? key : "");

    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    // The reference portal double-wraps: xdg-desktop-portal builds the value as
    // g_variant_new_variant(inner) and then passes it to g_variant_new("(v)", ...),
    // which wraps it again. libadwaita unwraps twice (adw-settings-impl-portal.c:
    // g_variant_get(ret, "(v)", &child); g_variant_get(child, "v", &child2)) and
    // crashes on a single layer with "format 'v' but value has type 'u'".
    ret = sd_bus_message_open_container(reply, 'v', "v");
    if (ret >= 0)
        ret = sd_bus_message_open_container(reply, 'v', signature);
    if (ret >= 0)
        ret = append_value(reply, key, signature);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

// ReadAll answers an empty dict.
//
// libadwaita 1.7 walks the returned a{sa{sv}} assuming the dict value is itself
// a variant and crashes in g_variant_type_equal() when it is not - with a real
// portal it never sees that shape because the portal filters by namespace and
// answers from gsettings. Rather than reproduce gsettings, answer "nothing
// here": libadwaita then keeps its built-in defaults (light scheme), and the
// colour scheme still comes from Read/ReadOne, which GTK3 uses.
int method_read_all(sd_bus_message* call, void*, sd_bus_error*)
{
    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    ret = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

// org.freedesktop.portal.Desktop.GetVersion: cheap, and it makes the name look
// like a real portal to `busctl introspect`.
int method_get_version(sd_bus_message* call, void*, sd_bus_error*)
{
    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret >= 0)
        ret = sd_bus_message_open_container(reply, 'e', "sv");
    if (ret >= 0)
        ret = sd_bus_message_append(reply, "su", "version", 5u);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

sd_bus_vtable const settings_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReadOne", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReadAll", "as", "a{sa{sv}}", method_read_all, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

sd_bus_vtable const desktop_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetVersion", "", "a{sv}", method_get_version, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

}

int main()
{
    sd_bus* bus = nullptr;
    int ret = sd_bus_open_user(&bus);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot connect to the session bus: %s\n", strerror(-ret));
        return 1;
    }

    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, SETTINGS_INTERFACE, settings_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register Settings: %s\n", strerror(-ret));
        return 1;
    }
    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, "org.freedesktop.portal.Desktop", desktop_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register Desktop: %s\n", strerror(-ret));
        return 1;
    }

    // Claim the name before any client asks. xdg-desktop-portal is still
    // dbus-activatable in the distro, and it is not usable here, so whoever gets
    // here first has to win. If the system portal briefly holds the name before
    // our mask takes effect, retry for a bounded time instead of dying, so a
    // transient race does not spin the restart loop. If the name is genuinely
    // taken (mask failed), bow out quietly (exit 0) rather than crash.
    int attempts = 0;
    while ((ret = sd_bus_request_name(bus, PORTAL_NAME, 0)) == -EEXIST && attempts++ < 20) {
        // Another portal owns the name (normally prevented by our user-level
        // D-Bus .service file; otherwise a stale system portal). Retry briefly
        // but log only once so a losing race does not flood the session log.
        if (attempts == 1)
            fprintf(stderr, "serenity-portal: %s already owned, retrying quietly...\n", PORTAL_NAME);
        sleep(1);
    }
    if (ret == -EEXIST) {
        fprintf(stderr, "serenity-portal: %s owned by another portal, exiting\n", PORTAL_NAME);
        return 0;
    }
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot own %s: %s\n", PORTAL_NAME, strerror(-ret));
        return 1;
    }
    fprintf(stderr, "serenity-portal: serving %s on %s\n", SETTINGS_INTERFACE, PORTAL_PATH);

    for (;;) {
        ret = sd_bus_process(bus, nullptr);
        if (ret < 0) {
            fprintf(stderr, "serenity-portal: bus error: %s\n", strerror(-ret));
            break;
        }
        if (ret > 0)
            continue;
        ret = sd_bus_wait(bus, 60000);
        if (ret < 0) {
            fprintf(stderr, "serenity-portal: wait failed: %s\n", strerror(-ret));
            break;
        }
    }

    sd_bus_unref(bus);
    return 0;
}
