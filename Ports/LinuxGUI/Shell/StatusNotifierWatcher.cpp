/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StatusNotifierWatcher.h"
#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <dbus/dbus.h>

// Wire format notes (KDE StatusNotifier spec):
//   org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem(s service)
//   org.kde.StatusNotifierItem.Activate(ii) / SecondaryActivate(ii) / ContextMenu(ii)
//   properties live on org.kde.StatusNotifierItem, read through
//   org.freedesktop.DBus.Properties.GetAll.
// IconPixmap is a(iiay) with ARGB32 in network byte order.

namespace {

constexpr char const* watcher_path = "/StatusNotifierWatcher";
constexpr char const* watcher_interface = "org.kde.StatusNotifierWatcher";
constexpr char const* item_interface = "org.kde.StatusNotifierItem";
constexpr char const* properties_interface = "org.freedesktop.DBus.Properties";

struct PendingRefresh {
    ByteString service;
    ByteString path;
};

// ARGB32 big-endian -> the port's little-endian BGRA8888 bitmaps.
RefPtr<Gfx::Bitmap> bitmap_from_pixmap_array(DBusMessageIter& array)
{
    RefPtr<Gfx::Bitmap> best;
    int best_area = 0;

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
        DBusMessageIter structure;
        dbus_message_iter_recurse(&array, &structure);

        dbus_int32_t width = 0;
        dbus_int32_t height = 0;
        if (dbus_message_iter_get_arg_type(&structure) == DBUS_TYPE_INT32)
            dbus_message_iter_get_basic(&structure, &width);
        dbus_message_iter_next(&structure);
        if (dbus_message_iter_get_arg_type(&structure) == DBUS_TYPE_INT32)
            dbus_message_iter_get_basic(&structure, &height);
        dbus_message_iter_next(&structure);

        if (dbus_message_iter_get_arg_type(&structure) == DBUS_TYPE_ARRAY && width > 0 && height > 0) {
            DBusMessageIter bytes;
            dbus_message_iter_recurse(&structure, &bytes);
            u8 const* data = nullptr;
            int length = 0;
            dbus_message_iter_get_fixed_array(&bytes, &data, &length);
            auto area = width * height;
            if (data && length >= area * 4 && area > best_area) {
                if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height }); !bitmap.is_error()) {
                    auto result = bitmap.release_value();
                    for (int y = 0; y < height; ++y) {
                        auto* scanline = result->scanline(y);
                        for (int x = 0; x < width; ++x) {
                            auto const* pixel = data + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
                            // ARGB32 is a packed u32 here, not a struct.
                            scanline[x] = (static_cast<u32>(pixel[0]) << 24)
                                | (static_cast<u32>(pixel[1]) << 16)
                                | (static_cast<u32>(pixel[2]) << 8)
                                | static_cast<u32>(pixel[3]);
                        }
                    }
                    best = move(result);
                    best_area = area;
                }
            }
        }
        dbus_message_iter_next(&array);
    }
    return best;
}

void append_variant_bool(DBusMessageIter& iter, dbus_bool_t value)
{
    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&iter, &variant);
}

void append_variant_int32(DBusMessageIter& iter, dbus_int32_t value)
{
    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "i", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value);
    dbus_message_iter_close_container(&iter, &variant);
}

void on_properties_reply(DBusPendingCall* pending, void* user_data)
{
    auto* context = static_cast<PendingRefresh*>(user_data);
    auto* reply = dbus_pending_call_steal_reply(pending);
    if (reply) {
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN)
            StatusNotifierWatcher::the().apply_properties(context->service, context->path, reply);
        dbus_message_unref(reply);
    }
    delete context;
    dbus_pending_call_unref(pending);
}

}

StatusNotifierWatcher& StatusNotifierWatcher::the()
{
    static StatusNotifierWatcher watcher;
    return watcher;
}

void StatusNotifierWatcher::initialize()
{
    if (m_name_acquired)
        return;

    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        warnln("Tray: no session bus, Linux tray icons will not appear");
        return;
    }

    if (!m_bus) {
        DBusError error;
        dbus_error_init(&error);
        m_bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
        if (!m_bus) {
            warnln("Tray: cannot connect to the session bus: {}", error.message ? error.message : "unknown");
            dbus_error_free(&error);
            return;
        }
        dbus_connection_set_exit_on_disconnect(m_bus, false);
    }

    if (!m_object_registered) {
        static DBusObjectPathVTable vtable = {};
        vtable.message_function = [](DBusConnection* connection, DBusMessage* message, void* user_data) -> DBusHandlerResult {
            return static_cast<StatusNotifierWatcher*>(user_data)->on_message(connection, message);
        };
        if (!dbus_connection_register_object_path(m_bus, watcher_path, &vtable, this)) {
            warnln("Tray: cannot register {}", watcher_path);
            return;
        }
        m_object_registered = true;
    }

    if (!m_filter_installed) {
        // Apps that die must lose their icon; NameOwnerChanged is the only
        // notification the bus gives us for that.
        dbus_bus_add_match(m_bus,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", nullptr);
        dbus_connection_add_filter(m_bus, [](DBusConnection*, DBusMessage* message, void* user_data) -> DBusHandlerResult {
            return static_cast<StatusNotifierWatcher*>(user_data)->on_bus_signal(message);
        }, this, nullptr);
        m_filter_installed = true;
    }

    DBusError error;
    dbus_error_init(&error);
    auto reply = dbus_bus_request_name(m_bus, watcher_interface, DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
    if (dbus_error_is_set(&error)) {
        warnln("Tray: cannot own {}: {}", watcher_interface, error.message ? error.message : "unknown");
        dbus_error_free(&error);
    }
    m_name_acquired = (reply == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || reply == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER);
    if (!m_name_acquired) {
        warnln("Tray: {} is owned by someone else", watcher_interface);
        return;
    }
    outln("Tray: serving {} on the session bus", watcher_interface);

    if (!m_pump_timer) {
        // libdbus has no event loop here; poll it like the notification server.
        m_pump_timer = Core::Timer::create_repeating(50, [this] { pump(); });
        m_pump_timer->start();
    }
}

void StatusNotifierWatcher::pump()
{
    if (!m_bus)
        return;
    dbus_connection_read_write(m_bus, 0);
    while (dbus_connection_get_dispatch_status(m_bus) == DBUS_DISPATCH_DATA_REMAINS) {
        if (!dbus_connection_dispatch(m_bus))
            break;
    }

    // NameOwnerChanged is not guaranteed to reach us in time when a client
    // dies (and our match may be installed after an early registration).
    // Re-check ownership every ~2s so a dead app's icon cannot sit in the
    // tray forever and push the native applet area out of place.
    if (++m_pump_ticks % 40 == 0 && !m_items.is_empty()) {
        Vector<ByteString> gone;
        for (auto const& item : m_items) {
            DBusError error;
            dbus_error_init(&error);
            auto has_owner = dbus_bus_name_has_owner(m_bus, item.service.characters(), &error);
            if (dbus_error_is_set(&error)) {
                dbus_error_free(&error);
                continue;
            }
            if (has_owner != TRUE)
                gone.append(item.service);
        }
        for (auto const& service : gone)
            forget_service(service);
    }
}

DBusHandlerResult StatusNotifierWatcher::on_bus_signal(DBusMessage* message)
{
    if (!dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    char const* name = nullptr;
    char const* old_owner = nullptr;
    char const* new_owner = nullptr;
    if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
            DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // An empty new owner means the service vanished.
    if (new_owner && *new_owner == '\0' && name && *name != '\0')
        forget_service(ByteString { name });
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusHandlerResult StatusNotifierWatcher::on_message(DBusConnection* connection, DBusMessage* message)
{
    if (dbus_message_is_method_call(message, watcher_interface, "RegisterStatusNotifierItem")) {
        char const* service = nullptr;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID) || !service)
            return DBUS_HANDLER_RESULT_HANDLED;

        // The argument is either a bus name (":1.42") or an object path. In
        // the latter case the sender's own name is the service.
        ByteString bus_name;
        ByteString object_path;
        if (service[0] == '/') {
            bus_name = ByteString { dbus_message_get_sender(message) };
            object_path = ByteString { service };
        } else {
            bus_name = ByteString { service };
            object_path = ByteString { "/StatusNotifierItem" };
        }
        register_item(move(bus_name), move(object_path));

        auto* reply = dbus_message_new_method_return(message);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, watcher_interface, "RegisterStatusNotifierHost")) {
        auto* reply = dbus_message_new_method_return(message);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, properties_interface, "Get")) {
        char const* interface_name = nullptr;
        char const* property = nullptr;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &interface_name, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID))
            return DBUS_HANDLER_RESULT_HANDLED;
        if (!interface_name || ByteString { interface_name } != watcher_interface)
            return DBUS_HANDLER_RESULT_HANDLED;

        auto* reply = dbus_message_new_method_return(message);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        if (ByteString { property } == "IsStatusNotifierHostRegistered") {
            append_variant_bool(iter, TRUE);
        } else if (ByteString { property } == "ProtocolVersion") {
            append_variant_int32(iter, 0);
        } else {
            DBusMessageIter variant, array;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "as", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
            for (auto const& item : m_items) {
                auto name = ByteString::formatted("{}{}", item.service, item.path);
                char const* text = name.characters();
                dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &text);
            }
            dbus_message_iter_close_container(&variant, &array);
            dbus_message_iter_close_container(&iter, &variant);
        }
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, properties_interface, "GetAll")) {
        auto* reply = dbus_message_new_method_return(message);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        DBusMessageIter dict;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

        auto append_entry = [&dict](char const* key, auto&& append_value) {
            DBusMessageIter entry;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            append_value(entry);
            dbus_message_iter_close_container(&dict, &entry);
        };

        append_entry("IsStatusNotifierHostRegistered", [](DBusMessageIter& entry) { append_variant_bool(entry, TRUE); });
        append_entry("ProtocolVersion", [](DBusMessageIter& entry) { append_variant_int32(entry, 0); });
        append_entry("RegisteredStatusNotifierItems", [this](DBusMessageIter& entry) {
            DBusMessageIter variant, array;
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
            for (auto const& item : m_items) {
                auto name = ByteString::formatted("{}{}", item.service, item.path);
                char const* text = name.characters();
                dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &text);
            }
            dbus_message_iter_close_container(&variant, &array);
            dbus_message_iter_close_container(&entry, &variant);
        });

        dbus_message_iter_close_container(&iter, &dict);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        static char const introspection[] =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node><interface name=\"org.kde.StatusNotifierWatcher\">\n"
            "  <method name=\"RegisterStatusNotifierItem\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
            "  <method name=\"RegisterStatusNotifierHost\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
            "  <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
            "  <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
            "  <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
            "  <signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>\n"
            "  <signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>\n"
            "  <signal name=\"StatusNotifierHostRegistered\"/>\n"
            "</interface></node>\n";
        auto* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_HANDLED;
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        // append_basic wants the ADDRESS OF A POINTER, not the address of the
        // array: passing &introspection (char const (*)[N]) made libdbus read
        // the first characters of the XML as a pointer and segfault.
        char const* text = introspection;
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void StatusNotifierWatcher::register_item(ByteString service, ByteString path)
{
    for (auto& item : m_items) {
        if (item.service == service && item.path == path) {
            request_properties(service, path);
            return;
        }
    }

    Item item;
    item.service = service;
    item.path = path;
    item.id = service;
    m_items.append(move(item));
    outln("Tray: item registered {} {}", service, path);

    emit_signal(watcher_interface, "StatusNotifierItemRegistered", service.view());
    request_properties(service, path);
    notify_changed();
}

void StatusNotifierWatcher::forget_service(ByteString const& service)
{
    bool changed = false;
    for (size_t i = 0; i < m_items.size();) {
        if (m_items[i].service == service) {
            auto name = ByteString::formatted("{}{}", m_items[i].service, m_items[i].path);
            m_items.remove(i);
            emit_signal(watcher_interface, "StatusNotifierItemUnregistered", name.view());
            changed = true;
        } else {
            ++i;
        }
    }
    if (changed)
        notify_changed();
}

void StatusNotifierWatcher::request_properties(ByteString service, ByteString path)
{
    if (!m_bus)
        return;

    auto* message = dbus_message_new_method_call(service.characters(), path.characters(),
        properties_interface, "GetAll");
    if (!message)
        return;
    char const* interface_name = item_interface;
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface_name, DBUS_TYPE_INVALID);

    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(m_bus, message, &pending, 2000) || !pending) {
        dbus_message_unref(message);
        return;
    }
    auto* context = new PendingRefresh { service, path };
    dbus_pending_call_set_notify(pending, on_properties_reply, context, nullptr);
    dbus_message_unref(message);
}

void StatusNotifierWatcher::apply_properties(ByteString const& service, ByteString const& path, DBusMessage* reply)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return;

    ByteString id;
    ByteString title;
    ByteString icon_name;
    RefPtr<Gfx::Bitmap> icon;

    DBusMessageIter dict;
    dbus_message_iter_recurse(&iter, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);

        char const* key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (!key || dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&dict);
            continue;
        }

        DBusMessageIter variant;
        dbus_message_iter_recurse(&entry, &variant);
        ByteString name { key };
        if (name == "Id" && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            char const* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            if (value)
                id = value;
        } else if (name == "Title" && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            char const* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            if (value)
                title = value;
        } else if (name == "IconName" && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            char const* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            if (value)
                icon_name = value;
        } else if (name == "IconPixmap" && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
            DBusMessageIter array;
            dbus_message_iter_recurse(&variant, &array);
            if (auto bitmap = bitmap_from_pixmap_array(array))
                icon = move(bitmap);
        } else if (name == "Status" && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            char const* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            if (value && ByteString { value } == "NeedsAttention")
                title = title.is_empty() ? ByteString { "needs attention" } : title;
        }
        dbus_message_iter_next(&dict);
    }

    for (auto& item : m_items) {
        if (item.service != service || item.path != path)
            continue;
        bool changed = false;
        if (!id.is_empty() && item.id != id) {
            item.id = id;
            changed = true;
        }
        if (item.title != title) {
            item.title = title;
            changed = true;
        }
        if (item.icon_name != icon_name) {
            item.icon_name = icon_name;
            changed = true;
        }
        if (icon) {
            item.icon = move(icon);
            changed = true;
        }
        if (changed)
            notify_changed();
        break;
    }
}

void StatusNotifierWatcher::emit_signal(char const* interface, char const* name, StringView argument)
{
    if (!m_bus)
        return;
    auto* signal = dbus_message_new_signal(watcher_path, interface, name);
    if (!signal)
        return;
    auto text = ByteString { argument };
    char const* value = text.characters();
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    dbus_connection_send(m_bus, signal, nullptr);
    dbus_message_unref(signal);
}

void StatusNotifierWatcher::notify_changed()
{
    if (on_items_changed)
        on_items_changed();
}

void StatusNotifierWatcher::activate(Item const& item, int screen_x, int screen_y)
{
    send_activation(item, "Activate"sv, screen_x, screen_y);
}

void StatusNotifierWatcher::secondary_activate(Item const& item, int screen_x, int screen_y)
{
    send_activation(item, "SecondaryActivate"sv, screen_x, screen_y);
}

void StatusNotifierWatcher::context_menu(Item const& item, int screen_x, int screen_y)
{
    send_activation(item, "ContextMenu"sv, screen_x, screen_y);
}

void StatusNotifierWatcher::send_activation(Item const& item, StringView method, int screen_x, int screen_y)
{
    if (!m_bus)
        return;
    auto method_name = ByteString { method };
    auto* message = dbus_message_new_method_call(item.service.characters(), item.path.characters(),
        item_interface, method_name.characters());
    if (!message)
        return;
    dbus_int32_t x = screen_x;
    dbus_int32_t y = screen_y;
    dbus_message_append_args(message, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
    dbus_connection_send(m_bus, message, nullptr);
    dbus_message_unref(message);
}
