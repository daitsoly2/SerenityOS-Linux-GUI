/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>

#include <dbus/dbus.h>

// Host side of the Linux tray protocol (KDE StatusNotifierItem).
//
// Linux apps (Qt, GTK through libappindicator, Electron, ...) publish a tray
// icon as a StatusNotifierItem and register it with the
// org.kde.StatusNotifierWatcher service. Nothing on this port owned that name,
// so their icons simply never appeared. We now own it, remember the items and
// hand them to the taskbar; clicks are forwarded back to the owning app.
//
// Only the icon + activation parts are implemented. Hover menus would need the
// com.canonical.dbusmenu interface on top, which is a separate protocol.
class StatusNotifierWatcher {
public:
    struct Item {
        ByteString service; // bus name, e.g. ":1.42"
        ByteString path;    // object path, usually /StatusNotifierItem
        ByteString id;
        ByteString title;
        ByteString icon_name;
        RefPtr<Gfx::Bitmap> icon;
        bool needs_attention { false };
    };

    static StatusNotifierWatcher& the();

    // Connects to the session bus, claims org.kde.StatusNotifierWatcher and
    // starts polling for item updates. Safe to call more than once.
    void initialize();

    Vector<Item> const& items() const { return m_items; }

    // Fired whenever the item list or one of the icons changed.
    Function<void()> on_items_changed;

    void activate(Item const&, int screen_x, int screen_y);
    void secondary_activate(Item const&, int screen_x, int screen_y);
    void context_menu(Item const&, int screen_x, int screen_y);

    // Called from the D-Bus reply callback; public only so the C callback can
    // reach it without friending a static function.
    void apply_properties(ByteString const& service, ByteString const& path, DBusMessage* reply);

    // Message handlers; public for the same reason (C callbacks).
    DBusHandlerResult on_message(DBusConnection*, DBusMessage*);
    DBusHandlerResult on_bus_signal(DBusMessage*);

private:
    StatusNotifierWatcher() = default;

    void pump();
    void register_item(ByteString service, ByteString path);
    void forget_service(ByteString const& service);
    void request_properties(ByteString service, ByteString path);
    void emit_signal(char const* interface, char const* name, StringView argument);
    void send_activation(Item const&, StringView method, int screen_x, int screen_y);
    void notify_changed();

    DBusConnection* m_bus { nullptr };
    bool m_name_acquired { false };
    bool m_object_registered { false };
    bool m_filter_installed { false };
    Vector<Item> m_items;
    RefPtr<Core::Timer> m_pump_timer;
    unsigned m_pump_ticks { 0 };
};
