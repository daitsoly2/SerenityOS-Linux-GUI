/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Notifications.h"

#include <LibCore/Timer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <WindowServer/Overlays.h>
#include <WindowServer/Screen.h>

#include <string.h>

namespace WindowServer {

// Compact by design: these sit in the corner of a 1080p desktop, so they only
// need to be readable at a glance, not to be a full notification centre.
static constexpr int card_width = 260;
static constexpr int card_margin = 12;
static constexpr int card_gap = 8;
static constexpr int card_padding = 9;

static Gfx::Color card_background() { return Gfx::Color::from_rgb(0x1b1e24); }
static Gfx::Color card_title() { return Gfx::Color::from_rgb(0xf4f6fa); }
static Gfx::Color card_body() { return Gfx::Color::from_rgb(0xc3c9d4); }
static Gfx::Color card_app() { return Gfx::Color::from_rgb(0x8e97a8); }
static Gfx::Color card_accent_normal() { return Gfx::Color::from_rgb(0x5aa9ff); }
static Gfx::Color card_accent_critical() { return Gfx::Color::from_rgb(0xff4d4f); }

// ─────────────────────────────────────────────────────────────────────────────
// Card overlay
// ─────────────────────────────────────────────────────────────────────────────

class NotificationCardOverlay final : public Overlay {
public:
    explicit NotificationCardOverlay(NotificationCenter::Card& card)
        : m_card(card)
    {
    }

    virtual ZOrder zorder() const override { return ZOrder::ScreenNumber; }

    void set_frame(Gfx::IntRect const& rect)
    {
        set_rect(rect);
        invalidate();
    }

    virtual void render(Gfx::Painter& painter, Screen const&) override
    {
        auto bounds = Gfx::IntRect { {}, rect().size() };
        painter.fill_rect_with_rounded_corners(bounds, card_background(), 8);

        auto accent = m_card.urgency >= 2 ? card_accent_critical() : card_accent_normal();
        painter.fill_rect_with_rounded_corners(Gfx::IntRect { 0, 6, 3, bounds.height() - 12 }, accent, 1);

        auto& title_font = title_font_ref();
        auto& body_font = body_font_ref();

        int y = card_padding;
        auto content_width = bounds.width() - card_padding - 8;

        if (!m_card.app_name.is_empty()) {
            painter.draw_text(Gfx::IntRect { card_padding + 2, y, content_width, 11 }, m_card.app_name, body_font, Gfx::TextAlignment::CenterLeft, card_app());
            y += 11;
        }

        if (!m_card.summary.is_empty()) {
            painter.draw_text(Gfx::IntRect { card_padding + 2, y, content_width, 14 }, m_card.summary, title_font, Gfx::TextAlignment::CenterLeft, card_title());
            y += 14;
        }

        if (!m_card.body.is_empty()) {
            auto remaining = bounds.height() - y - card_padding;
            painter.draw_text(Gfx::IntRect { card_padding + 2, y, content_width, remaining }, m_card.body, body_font, Gfx::TextAlignment::TopLeft, card_body(), Gfx::TextElision::Right, Gfx::TextWrapping::Wrap);
        }
    }

    static Gfx::Font const& body_font_ref()
    {
        static RefPtr<Gfx::Font> font = Gfx::FontDatabase::default_font().with_size(8.0f);
        return *font;
    }

    static Gfx::Font const& title_font_ref()
    {
        static RefPtr<Gfx::Font> font = Gfx::FontDatabase::default_font().with_size(8.5f);
        return *font;
    }

    static int height_for(NotificationCenter::Card const& card)
    {
        auto& body_font = body_font_ref();
        auto content_width = card_width - card_padding - 10;
        int height = card_padding;
        if (!card.app_name.is_empty())
            height += 11;
        if (!card.summary.is_empty())
            height += 14;
        if (!card.body.is_empty()) {
            auto average = max(body_font.width("n"sv), 1);
            auto per_line = max(content_width / average, 1);
            auto lines = max(1, (int)((card.body.length() + (size_t)per_line - 1) / (size_t)per_line));
            height += min(lines, 4) * 12;
        }
        height += card_padding;
        return max(height, 40);
    }

private:
    NotificationCenter::Card& m_card;
};

// ─────────────────────────────────────────────────────────────────────────────
// NotificationCenter
// ─────────────────────────────────────────────────────────────────────────────

NotificationCenter& NotificationCenter::the()
{
    static NotificationCenter* s_center = nullptr;
    if (!s_center)
        s_center = new NotificationCenter();
    return *s_center;
}

void NotificationCenter::initialize()
{
    the().try_connect();
}

// The session bus is brought up by the desktop session itself, so on a cold
// boot WindowServer can be up before /run/user/<uid>/bus exists. Retry once a
// second until it appears instead of giving up on the first attempt.
void NotificationCenter::try_connect()
{
    if (connect_to_session_bus())
        return;

    if (!m_retry_timer) {
        m_retry_timer = Core::Timer::create_repeating(1000, [this] {
            if (connect_to_session_bus() && m_retry_timer)
                m_retry_timer->stop();
        });
    }
    m_retry_timer->start();
}

NotificationCenter::NotificationCenter() = default;
NotificationCenter::~NotificationCenter() = default;

// ── cards ───────────────────────────────────────────────────────────────────

u32 NotificationCenter::post(ByteString const& app_name, ByteString const& summary, ByteString const& body, int expire_timeout_ms, ByteString const& icon)
{
    return add_card(app_name, summary, body, icon, 1, expire_timeout_ms);
}

u32 NotificationCenter::add_card(ByteString app_name, ByteString summary, ByteString body, ByteString icon, u8 urgency, int expire_timeout_ms)
{
    auto card = make<Card>();
    card->id = m_next_id++;
    card->app_name = move(app_name);
    card->summary = move(summary);
    card->body = move(body);
    card->icon = move(icon);
    card->urgency = urgency;

    auto* raw = card.ptr();
    raw->overlay = make<NotificationCardOverlay>(*raw);
    m_cards.append(move(card));

    relayout();

    if (expire_timeout_ms > 0) {
        auto id = raw->id;
        raw->expiry = Core::Timer::create_single_shot(expire_timeout_ms, [this, id] {
            remove_card(id, 1 /* expired */);
        });
        raw->expiry->start();
    }

    return raw->id;
}

void NotificationCenter::remove_card(u32 id, u32 close_reason)
{
    for (size_t i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i]->id != id)
            continue;
        auto& card = m_cards[i];
        if (card->expiry)
            card->expiry->stop();
        if (card->overlay) {
            card->overlay->set_enabled(false);
            card->overlay = nullptr;
        }
        m_cards.remove(i);
        emit_closed(id, close_reason);
        relayout();
        return;
    }
}

void NotificationCenter::close(u32 id)
{
    remove_card(id, 2 /* dismissed by user */);
}

void NotificationCenter::relayout()
{
    auto& screen = Screen::main();
    auto rect = screen.rect();
    int y = rect.top() + card_margin;

    for (auto& card : m_cards) {
        if (!card->overlay)
            continue;
        auto height = NotificationCardOverlay::height_for(*card);
        card->overlay->set_frame(Gfx::IntRect { rect.right() - card_width - card_margin, y, card_width, height });
        if (!card->overlay->is_enabled())
            card->overlay->set_enabled(true);
        y += height + card_gap;
    }
}

void NotificationCenter::emit_closed(u32 id, u32 reason)
{
    if (!m_bus || !m_name_acquired)
        return;
    DBusMessage* signal = dbus_message_new_signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
    if (!signal)
        return;
    dbus_message_append_args(signal, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(m_bus, signal, nullptr);
    dbus_message_unref(signal);
}

// ── D-Bus ───────────────────────────────────────────────────────────────────

bool NotificationCenter::connect_to_session_bus()
{
    if (m_name_acquired)
        return true;

    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        warnln("Notifications: no session bus, desktop notifications stay local-only");
        return false;
    }

    if (!m_bus) {
        DBusError error;
        dbus_error_init(&error);
        m_bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
        if (!m_bus) {
            warnln("Notifications: cannot connect to the session bus yet: {}", error.message ? error.message : "unknown");
            dbus_error_free(&error);
            return false;
        }
        dbus_connection_set_exit_on_disconnect(m_bus, false);
    }

    if (!m_object_registered) {
        static DBusObjectPathVTable vtable = {};
        vtable.message_function = &NotificationCenter::on_message;
        if (!dbus_connection_register_object_path(m_bus, "/org/freedesktop/Notifications", &vtable, this)) {
            warnln("Notifications: cannot register the notification object path");
            return false;
        }
        m_object_registered = true;
    }

    DBusError error;
    dbus_error_init(&error);
    auto reply = dbus_bus_request_name(m_bus, "org.freedesktop.Notifications", DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
    if (dbus_error_is_set(&error)) {
        warnln("Notifications: cannot own org.freedesktop.Notifications: {}", error.message ? error.message : "unknown");
        dbus_error_free(&error);
    }
    m_name_acquired = (reply == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || reply == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER);
    if (!m_name_acquired) {
        warnln("Notifications: org.freedesktop.Notifications is owned by someone else; Linux app notifications will not reach the desktop");
        return false;
    }
    outln("Notifications: serving org.freedesktop.Notifications on the session bus");

    // libdbus has no event loop of its own here. Polling it at 20 Hz costs two
    // syscalls per tick and keeps the compositor free of a blocking read.
    if (!m_pump_timer) {
        m_pump_timer = Core::Timer::create_repeating(50, [this] { pump(); });
        m_pump_timer->start();
    }
    return true;
}

void NotificationCenter::pump()
{
    if (!m_bus)
        return;

    dbus_connection_read_write(m_bus, 0);
    while (dbus_connection_get_dispatch_status(m_bus) == DBUS_DISPATCH_DATA_REMAINS) {
        if (!dbus_connection_dispatch(m_bus))
            break;
    }
}

DBusHandlerResult NotificationCenter::on_message(DBusConnection* connection, DBusMessage* message, void* user_data)
{
    auto* center = static_cast<NotificationCenter*>(user_data);

    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "Notify"))
        return center->handle_notify(connection, message);

    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "CloseNotification")) {
        dbus_uint32_t id = 0;
        if (dbus_message_get_args(message, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID))
            center->remove_card(id, 3 /* closed by call */);
        auto* reply = dbus_message_new_method_return(message);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "GetCapabilities")) {
        auto* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_HANDLED;
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        DBusMessageIter array;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &array);
        char const* capabilities[] = { "body", "persistence" };
        for (auto const* capability : capabilities)
            dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &capability);
        dbus_message_iter_close_container(&iter, &array);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "GetServerInformation")) {
        auto* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_HANDLED;
        char const* name = "Serenity Notification Server";
        char const* vendor = "SerenityOS Linux GUI port";
        char const* version = "1.0";
        char const* spec_version = "1.2";
        dbus_message_append_args(reply,
            DBUS_TYPE_STRING, &name,
            DBUS_TYPE_STRING, &vendor,
            DBUS_TYPE_STRING, &version,
            DBUS_TYPE_STRING, &spec_version,
            DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        static char const* xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.freedesktop.Notifications\">\n"
            "    <method name=\"Notify\">\n"
            "      <arg name=\"app_name\" type=\"s\" direction=\"in\"/>\n"
            "      <arg name=\"replaces_id\" type=\"u\" direction=\"in\"/>\n"
            "      <arg name=\"app_icon\" type=\"s\" direction=\"in\"/>\n"
            "      <arg name=\"summary\" type=\"s\" direction=\"in\"/>\n"
            "      <arg name=\"body\" type=\"s\" direction=\"in\"/>\n"
            "      <arg name=\"actions\" type=\"as\" direction=\"in\"/>\n"
            "      <arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/>\n"
            "      <arg name=\"expire_timeout\" type=\"i\" direction=\"in\"/>\n"
            "      <arg name=\"id\" type=\"u\" direction=\"out\"/>\n"
            "    </method>\n"
            "    <method name=\"CloseNotification\">\n"
            "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
            "    </method>\n"
            "    <method name=\"GetCapabilities\">\n"
            "      <arg name=\"capabilities\" type=\"as\" direction=\"out\"/>\n"
            "    </method>\n"
            "    <method name=\"GetServerInformation\">\n"
            "      <arg name=\"name\" type=\"s\" direction=\"out\"/>\n"
            "      <arg name=\"vendor\" type=\"s\" direction=\"out\"/>\n"
            "      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
            "      <arg name=\"spec_version\" type=\"s\" direction=\"out\"/>\n"
            "    </method>\n"
            "    <signal name=\"NotificationClosed\">\n"
            "      <arg name=\"id\" type=\"u\"/>\n"
            "      <arg name=\"reason\" type=\"u\"/>\n"
            "    </signal>\n"
            "    <signal name=\"ActionInvoked\">\n"
            "      <arg name=\"id\" type=\"u\"/>\n"
            "      <arg name=\"action_key\" type=\"s\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
            "    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
            "  </interface>\n"
            "</node>\n";
        auto* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_HANDLED;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusHandlerResult NotificationCenter::handle_notify(DBusConnection* connection, DBusMessage* message)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(message, &iter))
        return DBUS_HANDLER_RESULT_HANDLED;

    char const* app_name = "";
    dbus_uint32_t replaces_id = 0;
    char const* app_icon = "";
    char const* summary = "";
    char const* body = "";
    dbus_int32_t expire_timeout = -1;
    u8 urgency = 1;

    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&iter, &app_name);
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
        dbus_message_iter_get_basic(&iter, &replaces_id);
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&iter, &app_icon);
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&iter, &summary);
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&iter, &body);
    dbus_message_iter_next(&iter); // actions: as
    dbus_message_iter_next(&iter); // hints: a{sv}

    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter hints;
        dbus_message_iter_recurse(&iter, &hints);
        while (dbus_message_iter_get_arg_type(&hints) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&hints, &entry);
            char const* key = nullptr;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            if (key && !strcmp(key, "urgency") && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_BYTE) {
                unsigned char value = 1;
                dbus_message_iter_get_basic(&entry, &value);
                urgency = value;
            }
            dbus_message_iter_next(&hints);
        }
    }
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
        dbus_message_iter_get_basic(&iter, &expire_timeout);

    // -1 means "server default"; 0 means "never expire".
    int timeout_ms = 4000;
    if (expire_timeout == 0)
        timeout_ms = 0;
    else if (expire_timeout > 0)
        timeout_ms = min((int)expire_timeout, 30000);

    u32 id = 0;
    if (replaces_id != 0) {
        // Replace in place: drop the old card but keep the caller's id.
        for (auto& card : m_cards) {
            if (card->id != replaces_id)
                continue;
            card->app_name = ByteString { app_name };
            card->summary = ByteString { summary };
            card->body = ByteString { body };
            card->icon = ByteString { app_icon };
            card->urgency = urgency;
            if (card->expiry)
                card->expiry->stop();
            if (timeout_ms > 0) {
                auto existing = card->id;
                card->expiry = Core::Timer::create_single_shot(timeout_ms, [this, existing] { remove_card(existing, 1); });
                card->expiry->start();
            }
            relayout();
            id = replaces_id;
            break;
        }
    }

    if (id == 0)
        id = add_card(ByteString { app_name }, ByteString { summary }, ByteString { body }, ByteString { app_icon }, urgency, timeout_ms);

    auto* reply = dbus_message_new_method_return(message);
    if (reply) {
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

}
