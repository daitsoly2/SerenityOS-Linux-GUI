/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>

// libdbus is a C API; this header is only included by the two port files that
// implement and drive the notification center.
#include <dbus/dbus.h>

namespace Core {
class Timer;
}

namespace WindowServer {

class NotificationCardOverlay;

// Compositor-level notification system.
//
// Two jobs:
//   1. Own the desktop's notification surface. Cards are drawn as WindowServer
//      overlays, so they float above every window and need no client process.
//   2. Speak the freedesktop.org Desktop Notifications protocol on the session
//      bus (org.freedesktop.Notifications), so Linux apps running inside the
//      port — GTK/Qt apps under the wlroots host, notify-send, browsers — can
//      post notifications that show up as Serenity desktop notifications.
//
// The recorder posts its "screenshot saved" / "recording started" messages
// through the same path, which is why they look like every other notification.
class NotificationCenter {
public:
    static NotificationCenter& the();
    static void initialize();

    // Posts a notification on behalf of the desktop itself. Returns the id
    // that can be handed to close().
    u32 post(ByteString const& app_name, ByteString const& summary, ByteString const& body, int expire_timeout_ms = 4000, ByteString const& icon = {});

    void close(u32 id);

private:
    NotificationCenter();
    ~NotificationCenter();

    // ── rendering ───────────────────────────────────────────────────────────
    u32 add_card(ByteString app_name, ByteString summary, ByteString body, ByteString icon, u8 urgency, int expire_timeout_ms);
    void remove_card(u32 id, u32 close_reason);
    void relayout();
    void emit_closed(u32 id, u32 reason);

    // ── D-Bus ───────────────────────────────────────────────────────────────
    void try_connect();
    bool connect_to_session_bus();
    void pump();
    static DBusHandlerResult on_message(DBusConnection*, DBusMessage*, void* user_data);
    DBusHandlerResult handle_notify(DBusConnection*, DBusMessage*);

    DBusConnection* m_bus { nullptr };
    RefPtr<Core::Timer> m_pump_timer;
    RefPtr<Core::Timer> m_retry_timer;
    bool m_object_registered { false };
    bool m_name_acquired { false };

    struct Card {
        u32 id { 0 };
        ByteString app_name;
        ByteString summary;
        ByteString body;
        ByteString icon;
        u8 urgency { 1 };
        OwnPtr<NotificationCardOverlay> overlay;
        RefPtr<Core::Timer> expiry;
    };

    u32 m_next_id { 1 };
    Vector<NonnullOwnPtr<Card>> m_cards;

    friend class NotificationCardOverlay;
};

}
