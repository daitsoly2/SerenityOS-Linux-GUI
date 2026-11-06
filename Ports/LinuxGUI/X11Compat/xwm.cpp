/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// XWM implementation. See xwm.h for the design notes.
//
// This is the same role Weston's xwayland module plays for Weston: an X
// window manager living inside the compositor that pairs X toplevels with
// the wl_surfaces Xwayland creates for them. All Xlib usage is confined to
// this file; the compositor front-end only sees plain C++ callbacks.

#include "xwm.h"

#include <AK/Vector.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace XWM {

std::function<void(WindowInfo const&)> on_window_mapped;
std::function<void(unsigned long, std::string const&)> on_title_changed;
std::function<void(unsigned long)> on_window_destroyed;

namespace {

int on_x_error(Display* display, XErrorEvent* event)
{
    char text[256] { 0 };
    XGetErrorText(display, event->error_code, text, sizeof(text));
    fprintf(stderr, "xwm: X error: %s (request %d.%d, resource 0x%lx)\n",
        text, event->request_code, event->minor_code, event->resourceid);
    return 0;
}

Display* g_display { nullptr };
Window g_root { 0 };
int g_screen { 0 };
Window g_manager_window { 0 };
Atom g_wl_surface_id_atom { 0 };
Atom g_wl_surface_id_property_atom { 0 };
Atom g_wm_protocols { 0 };
Atom g_wm_delete_window { 0 };
Atom g_wm_take_focus { 0 };
Atom g_wm_state { 0 };
Atom g_net_wm_window_type { 0 };
Atom g_net_wm_window_type_popup_menu { 0 };
Atom g_net_wm_window_type_utility { 0 };
Atom g_net_wm_window_type_tooltip { 0 };
Atom g_net_wm_window_type_combo { 0 };
Atom g_net_wm_window_type_dropdown_menu { 0 };
Atom g_net_wm_window_type_menu { 0 };
Atom g_net_wm_window_type_notification { 0 };
Atom g_net_wm_name { 0 };
Atom g_utf8_string { 0 };
bool g_selection_owned { false };
// Last X server timestamp we observed (from PropertyNotify on our helper
// window, which we toggle to fish for one). WM_TAKE_FOCUS must carry a real
// timestamp - CurrentTime makes clients (xterm!) treat the message as bogus.
Time g_last_timestamp { CurrentTime };
// Windows we already Composite-redirected (redirecting twice = BadAccess).
Vector<unsigned long> g_redirected_windows;

// ICCCM: a managed toplevel carries WM_STATE [state, icon-window]. Xwayland
// (and X clients generally) expect the window manager to maintain it - without
// it the window looks unmanaged, and Xwayland will not create a wl_surface
// for it in rootless mode.
void set_wm_state(Window id, long state)
{
    long data[2] = { state, None };
    XChangeProperty(g_display, id, g_wm_state, g_wm_state, 32, PropModeReplace,
        reinterpret_cast<unsigned char const*>(data), 2);
}

// Same, but tolerate the window dying underneath us: Xlib reports errors
// asynchronously, so an UnmapNotify for a window that just destroyed (Firefox
// tears down like this) would otherwise log a BadWindow per teardown. Install
// a sink handler and XSync inside the scope to force error delivery while it
// is active - the pattern weston's xwm uses for best-effort property writes.
void set_wm_state_tolerating_death(Window id, long state)
{
    XErrorHandler previous = XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
    set_wm_state(id, state);
    XSync(g_display, False);
    XSetErrorHandler(previous);
}

// THE pairing gate: rootless Xwayland only creates a wl_surface for a window
// whose Composite redirect state is RedirectDrawManual (checked in
// ensure_surface_for_window). Every real XWM (weston included) redirects its
// toplevels with the Composite extension before they map; without this, no
// surface, no pairing, ever.
void redirect_window(Window id)
{
    if (g_redirected_windows.contains_slow(id))
        return;
    XCompositeRedirectWindow(g_display, id, CompositeRedirectManual);
    g_redirected_windows.append(id);
}

WindowInfo describe(Window id)
{
    WindowInfo info;
    info.xid = id;
    auto* display = g_display;

    XWindowAttributes attrs {};
    if (XGetWindowAttributes(display, id, &attrs)) {
        info.x = attrs.x;
        info.y = attrs.y;
        info.width = attrs.width;
        info.height = attrs.height;
        info.override_redirect = attrs.override_redirect;
    }

    // Prefer _NET_WM_NAME (UTF-8): Wine / GTK / Qt set Chinese and other
    // non-Latin titles only there (WM_NAME is COMPOUND_TEXT or empty), which is
    // why the Serenity title bar and taskbar used to show a blank name.
    bool got_title = false;
    if (g_net_wm_name && g_utf8_string) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, bytes_after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, id, g_net_wm_name, 0, 1024, False, g_utf8_string,
                &type, &format, &count, &bytes_after, &data)
            == Success
            && data) {
            if (type == g_utf8_string && count > 0)
                info.title.assign(reinterpret_cast<char*>(data), count);
            XFree(data);
            got_title = count > 0;
        }
    }
    if (!got_title) {
        char* name = nullptr;
        if (XFetchName(display, id, &name) && name) {
            info.title = name;
            XFree(name);
        }
    }

    Window transient_for = None;
    if (XGetTransientForHint(display, id, &transient_for))
        info.transient_for = transient_for;

    // A managed window that is semantically a popup (IME candidate window,
    // menu, tooltip, ...) must stay frameless even though it is not
    // override-redirect. Look at _NET_WM_WINDOW_TYPE for those types.
    if (g_net_wm_window_type) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, bytes_left = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, id, g_net_wm_window_type, 0, 16, False, XA_ATOM,
                &type, &format, &count, &bytes_left, &data)
                == Success
            && data) {
            if (format == 32) {
                auto* atoms = reinterpret_cast<Atom*>(data);
                for (unsigned long i = 0; i < count; ++i) {
                    auto atom = atoms[i];
                    if (atom == g_net_wm_window_type_popup_menu || atom == g_net_wm_window_type_utility
                        || atom == g_net_wm_window_type_tooltip || atom == g_net_wm_window_type_combo
                        || atom == g_net_wm_window_type_dropdown_menu || atom == g_net_wm_window_type_menu
                        || atom == g_net_wm_window_type_notification) {
                        info.is_popup = true;
                        break;
                    }
                }
            }
            XFree(data);
        }
    }

    // The whole point of this module: which wl_surface did Xwayland park on
    // this window? Modern Xwayland announces it via a root ClientMessage
    // (handled in handle_event); very old Xwayland set it as a property.
    if (g_wl_surface_id_property_atom) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, bytes_left = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, id, g_wl_surface_id_property_atom, 0, 1, False, XA_CARDINAL,
                &type, &format, &count, &bytes_left, &data)
                == Success
            && data) {
            if (count >= 1 && format == 32)
                info.wl_surface_id = *reinterpret_cast<unsigned long const*>(data);
            XFree(data);
        }
    }
    return info;
}

void select_toplevel_input(Window id)
{
    // StructureNotify for map/destroy, PropertyNotify for the title arriving
    // (possibly after the map) and the surface id if it appears late.
    XSelectInput(g_display, id, StructureNotifyMask | PropertyChangeMask);
}

// Take over toplevels that already exist on the screen. We acquire WM_S<n>
// some time after Xwayland came up, and clients that mapped in that gap never
// generate a MapNotify for us (X11 events are not retroactive). Every real WM
// therefore scans the tree when it takes charge.
void manage_existing_windows()
{
    Window root_return = 0, parent_return = 0;
    Window* children = nullptr;
    unsigned int n_children = 0;
    if (!XQueryTree(g_display, g_root, &root_return, &parent_return, &children, &n_children))
        return;
    for (unsigned int i = 0; i < n_children; ++i) {
        Window id = children[i];
        XWindowAttributes attrs {};
        if (!XGetWindowAttributes(g_display, id, &attrs))
            continue;
        if (attrs.map_state != IsViewable || attrs.override_redirect)
            continue;
        select_toplevel_input(id);
        // We took charge after the client already mapped: retroactively mark
        // it managed so Xwayland treats it like any other toplevel.
        set_wm_state(id, NormalState);
        redirect_window(id);
        auto info = describe(id);
        if (info.width >= 8 && info.height >= 8 && on_window_mapped)
            on_window_mapped(info);
    }
    if (children)
        XFree(children);
}

// Update our idea of "now" from any timestamped event we see.
void observe_timestamp(Time time)
{
    if (time != CurrentTime && time > g_last_timestamp)
        g_last_timestamp = time;
}

// Fish for a fresh timestamp: toggling a property on our own helper window
// produces a PropertyNotify we observe in pump(). Used before sending
// WM_TAKE_FOCUS so the message carries a valid ICCCM timestamp.
void refresh_timestamp()
{
    if (!g_display)
        return;
    static bool toggle = false;
    long value = toggle ? 1 : 2;
    toggle = !toggle;
    Atom prop = XInternAtom(g_display, "_SERENITY_WM_TS", False);
    XChangeProperty(g_display, g_manager_window, prop, XA_CARDINAL, 32,
        PropModeReplace, reinterpret_cast<unsigned char const*>(&value), 1);
    XFlush(g_display);
    // The PropertyNotify arrives asynchronously; pump() will observe it. For
    // the message we send right now, the previous timestamp is the best we
    // have - a real (if slightly stale) timestamp beats CurrentTime.
}

void handle_event(XEvent& event)
{
    switch (event.type) {
    case CreateNotify: {
        // Redirect EVERY new window before it maps (the Composite-safe
        // timing). Override-redirect popups (menus, tooltips) need the manual
        // redirect too: rootless Xwayland only creates wl_surfaces for
        // RedirectDrawManual windows, and menus are override-redirect. Our
        // own helper windows are excluded.
        if (event.xcreatewindow.window != g_manager_window)
            redirect_window(event.xcreatewindow.window);
        break;
    }
    case MapRequest: {
        // A client wants to map: we are the WM, so we must let it through.
        // ICCCM: mark it managed (WM_STATE) *before* the map so observers
        // (Xwayland!) see a managed window from the first MapNotify. The
        // redirect normally happened at CreateNotify; repeat it here as a
        // safety net (Composite redirect is refcounted).
        set_wm_state(event.xmaprequest.window, NormalState);
        redirect_window(event.xmaprequest.window);
        XMapWindow(g_display, event.xmaprequest.window);
        XFlush(g_display);
        break;
    }
    case ConfigureRequest: {
        auto& request = event.xconfigurerequest;
        XWindowChanges changes {};
        changes.x = request.x;
        changes.y = request.y;
        changes.width = request.width;
        changes.height = request.height;
        changes.border_width = request.border_width;
        changes.sibling = request.above;
        changes.stack_mode = request.detail;
        XConfigureWindow(g_display, request.window, static_cast<unsigned>(request.value_mask), &changes);
        XFlush(g_display);
        break;
    }
    case MapNotify: {
        // Xwayland realizes the wl_surface and parks its id on the window
        // during map, so by MapNotify the property is readable.
        auto id = event.xmap.window;
        select_toplevel_input(id);
        auto info = describe(id);
        if (info.width >= 8 && info.height >= 8 && on_window_mapped)
            on_window_mapped(info);
        break;
    }
    case PropertyNotify: {
        auto id = event.xproperty.window;
        observe_timestamp(event.xproperty.time);
        if (event.xproperty.atom == XA_WM_NAME || event.xproperty.atom == g_net_wm_name) {
            auto info = describe(id);
            if (on_title_changed)
                on_title_changed(id, info.title);
        }
        break;
    }
    case ClientMessage: {
        // Xwayland announces the wl_surface for a newly realized toplevel by
        // sending a ClientMessage to the root window (this is how modern
        // Xwayland does it; the _XWAYLAND_WL_SURFACE_ID *property* is ancient
        // history). format=32, message_type=WL_SURFACE_ID,
        // window = the X toplevel, data.l[0] = the wl_surface resource id.
        auto& message = event.xclient;
        if (g_wl_surface_id_atom && message.message_type == g_wl_surface_id_atom && message.format == 32) {
            auto info = describe(message.window);
            info.wl_surface_id = static_cast<unsigned long>(message.data.l[0]) & 0xffffffffUL;
            if (info.width >= 8 && info.height >= 8 && on_window_mapped)
                on_window_mapped(info);
        }
        break;
    }
    case UnmapNotify:
        // Treated like destruction for the desktop: the Serenity window goes
        // away. If the client remaps, a fresh MapNotify pairs it again.
        set_wm_state_tolerating_death(event.xunmap.window, WithdrawnState);
        if (on_window_destroyed)
            on_window_destroyed(event.xunmap.window);
        break;
    case DestroyNotify:
        if (on_window_destroyed)
            on_window_destroyed(event.xdestroywindow.window);
        // The id will never come back (X recycles ids only much later), so
        // stop tracking the redirect - otherwise this vector grows forever.
        g_redirected_windows.remove_first_matching(
            [&](auto entry) { return entry == event.xdestroywindow.window; });
        break;
    }
}

} // anonymous namespace

bool ready(char const* display_name)
{
    // Non-blocking probe on a throwaway connection: has Xwayland booted far
    // enough to answer an X connection setup? This matters because Xwayland's
    // own startup performs blocking Wayland roundtrips against us. If we call
    // the blocking XOpenDisplay while those are still pending, the GUI loop
    // freezes, the roundtrips never complete, and the two processes deadlock.
    // The probe's worst case is a ~10ms poll timeout, never a deadlock.
    int number = -1;
    if (!display_name || display_name[0] != ':' || sscanf(display_name, ":%d", &number) != 1 || number < 0)
        return false;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", number);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    // X connection setup request: byte order 'l', protocol 11.0, no auth.
    static unsigned char const setup[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (write(fd, setup, sizeof(setup)) != static_cast<ssize_t>(sizeof(setup))) {
        close(fd);
        return false;
    }

    auto read_full = [&](unsigned char* buffer, size_t length) {
        size_t offset = 0;
        while (offset < length) {
            pollfd pfd { fd, POLLIN, 0 };
            if (poll(&pfd, 1, 10) <= 0)
                return false;
            auto nread = read(fd, buffer + offset, length - offset);
            if (nread <= 0)
                return false;
            offset += static_cast<size_t>(nread);
        }
        return true;
    };

    // Setup reply: byte 0 == 1 means success, bytes 6-7 hold the length of the
    // rest of the reply in 4-byte units. Consume the whole reply so we only
    // report ready when the server has fully answered a setup.
    unsigned char header[8];
    if (!read_full(header, sizeof(header)) || header[0] != 1) {
        close(fd);
        return false;
    }
    unsigned remaining = (header[6] | (header[7] << 8)) * 4u;
    bool ok = true;
    unsigned char discard[256];
    while (ok && remaining > 0) {
        size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
        if (!read_full(discard, chunk))
            ok = false;
        else
            remaining -= static_cast<unsigned>(chunk);
    }
    close(fd);
    return ok;
}

bool connect(char const* display_name)
{
    auto* display = XOpenDisplay(display_name);
    if (!display)
        return false;
    g_display = display;
    g_screen = DefaultScreen(display);
    g_root = RootWindow(display, g_screen);
    XSetErrorHandler(on_x_error);

    // The atom Xwayland (24.x) sends as a root-window ClientMessage carrying
    // the wl_surface id of a newly realized toplevel. (Older Xwayland set the
    // _XWAYLAND_WL_SURFACE_ID *property* instead; describe() still reads that
    // as a fallback.)
    g_wl_surface_id_atom = XInternAtom(display, "WL_SURFACE_ID", False);
    g_wl_surface_id_property_atom = XInternAtom(display, "_XWAYLAND_WL_SURFACE_ID", False);
    g_wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    g_wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    g_wm_take_focus = XInternAtom(display, "WM_TAKE_FOCUS", False);
    g_wm_state = XInternAtom(display, "WM_STATE", False);
    g_net_wm_window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    g_net_wm_window_type_popup_menu = XInternAtom(display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    g_net_wm_window_type_utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    g_net_wm_window_type_tooltip = XInternAtom(display, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    g_net_wm_window_type_combo = XInternAtom(display, "_NET_WM_WINDOW_TYPE_COMBO", False);
    g_net_wm_window_type_dropdown_menu = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    g_net_wm_window_type_menu = XInternAtom(display, "_NET_WM_WINDOW_TYPE_MENU", False);
    g_net_wm_window_type_notification = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    g_net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    g_utf8_string = XInternAtom(display, "UTF8_STRING", False);

    // Off-screen helper window that owns the WM_S<n> selection.
    XSetWindowAttributes attrs {};
    attrs.event_mask = PropertyChangeMask;
    g_manager_window = XCreateWindow(display, g_root, -100, -100, 1, 1, 0, 0,
        InputOutput, CopyFromParent, CWEventMask, &attrs);

    XSelectInput(display, g_root,
        SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask);

    // Acquire WM_S<n> so Xwayland (and clients) know a window manager is up.
    char selection[32];
    snprintf(selection, sizeof(selection), "WM_S%d", g_screen);
    Atom wm_sn = XInternAtom(display, selection, False);
    XSetSelectionOwner(display, wm_sn, g_manager_window, CurrentTime);
    if (XGetSelectionOwner(display, wm_sn) == g_manager_window)
        g_selection_owned = true;

    XFlush(display);
    if (g_selection_owned)
        manage_existing_windows();
    // Fish for a real timestamp up front, so the first WM_TAKE_FOCUS we send
    // already carries one (PropertyNotify lands in the first pump()).
    refresh_timestamp();
    fprintf(stderr, "xwm: managing display %s (WM_%s %s)\n", display_name, selection,
        g_selection_owned ? "acquired" : "NOT acquired");
    return true;
}

void pump()
{
    if (!g_display)
        return;
    while (XPending(g_display) > 0) {
        XEvent event;
        XNextEvent(g_display, &event);
        handle_event(event);
    }
    XFlush(g_display);
}

// ICCCM: only send a WM_PROTOCOLS message to clients that actually announced
// that protocol. Sending WM_TAKE_FOCUS to a client that did not list it makes
// xterm (whose translation table matches every WM_PROTOCOLS message to
// DeleteWindow) exit - real window managers check first.
bool window_supports_protocol(Window id, Atom protocol)
{
    Atom actual = None;
    int format = 0;
    unsigned long count = 0, left = 0;
    Atom* data = nullptr;
    if (XGetWindowProperty(g_display, id, g_wm_protocols, 0, 64, False, XA_ATOM,
            &actual, &format, &count, &left, reinterpret_cast<unsigned char**>(&data))
            != Success
        || !data)
        return false;
    bool found = false;
    for (unsigned long i = 0; i < count; ++i) {
        if (data[i] == protocol) {
            found = true;
            break;
        }
    }
    XFree(data);
    return found;
}

void focus_window(unsigned long xid)
{
    if (!g_display)
        return;
    // Plain input focus always works; the WM_TAKE_FOCUS protocol message is
    // additionally needed by toolkits that ignore focus-only activation (GTK
    // apps like Firefox among them) - but only if they announced the protocol.
    XSetInputFocus(g_display, xid, RevertToPointerRoot, g_last_timestamp);
    if (window_supports_protocol(xid, g_wm_take_focus)) {
        refresh_timestamp();
        XEvent event {};
        event.xclient.type = ClientMessage;
        event.xclient.window = xid;
        event.xclient.format = 32;
        event.xclient.message_type = g_wm_protocols;
        event.xclient.data.l[0] = g_wm_take_focus;
        event.xclient.data.l[1] = g_last_timestamp;
        XSendEvent(g_display, xid, False, NoEventMask, &event);
    }
    XFlush(g_display);
}

void close_window(unsigned long xid)
{
    if (!g_display)
        return;
    // WM_DELETE_WINDOW only if the client announced it; otherwise the only
    // polite-enough option left is to kill the client outright.
    if (!window_supports_protocol(xid, g_wm_delete_window)) {
        XKillClient(g_display, xid);
        XFlush(g_display);
        return;
    }
    XEvent event {};
    event.xclient.type = ClientMessage;
    event.xclient.window = xid;
    event.xclient.format = 32;
    event.xclient.message_type = g_wm_protocols;
    event.xclient.data.l[0] = g_wm_delete_window;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(g_display, xid, False, NoEventMask, &event);
    XFlush(g_display);
}

void resize_window(unsigned long xid, int width, int height)
{
    if (!g_display)
        return;
    XResizeWindow(g_display, xid, static_cast<unsigned>(width), static_cast<unsigned>(height));
    XFlush(g_display);
}

} // namespace XWM
