/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-toplevel-bridge: expose the compositor's window list to the shell.
//
// In the Xwayland session (labwc owns the display, the Serenity desktop is one
// X client), foreign GTK/Qt windows are labwc toplevels - they never reach our
// WindowServer, so the taskbar cannot see them. labwc exports
// zwlr_foreign_toplevel_manager_v1; this bridge binds it and publishes a plain
// file the shell can poll:
//
//   /run/user/1000/serenity-toplevels      one line per window:
//                                          <index>\t<app_id>\t<title>\t<flags>
//                                          flags: a=activated m=minimized
//                                                 x=maximized f=fullscreen
//   /run/user/1000/serenity-toplevel-cmd   commands, one per line:
//                                          activate <index>
//                                          close <index>
//                                          minimize <index>
//
// A file plus a FIFO instead of new IPC: the shell already polls files (clipboard
// history does the same) and it keeps the two processes independent - either can
// restart without the other noticing.

#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr char const* STATE_PATH = "/run/user/1000/serenity-toplevels";
constexpr char const* STATE_TMP = "/run/user/1000/serenity-toplevels.tmp";
constexpr char const* FIFO_PATH = "/run/user/1000/serenity-toplevel-cmd";

struct Toplevel {
    zwlr_foreign_toplevel_handle_v1* handle { nullptr };
    std::string app_id;
    std::string title;
    bool activated { false };
    bool minimized { false };
    bool maximized { false };
    bool fullscreen { false };
};

wl_seat* g_seat { nullptr };
zwlr_foreign_toplevel_manager_v1* g_manager { nullptr };
std::vector<Toplevel*> g_toplevels;

void rewrite_state()
{
    std::string out;
    for (size_t i = 0; i < g_toplevels.size(); ++i) {
        auto const& toplevel = *g_toplevels[i];
        std::string flags;
        if (toplevel.activated)
            flags += 'a';
        if (toplevel.minimized)
            flags += 'm';
        if (toplevel.maximized)
            flags += 'x';
        if (toplevel.fullscreen)
            flags += 'f';
        out += std::to_string(i);
        out += '\t';
        out += toplevel.app_id;
        out += '\t';
        // Tabs and newlines would break the line format; the shell splits on tabs.
        for (char c : toplevel.title)
            out += (c == '\t' || c == '\n') ? ' ' : c;
        out += '\t';
        out += flags;
        out += '\n';
    }

    // Atomic replace, so the shell never reads a half-written list.
    if (auto* file = fopen(STATE_TMP, "w")) {
        fwrite(out.data(), 1, out.size(), file);
        fclose(file);
        if (rename(STATE_TMP, STATE_PATH) != 0)
            perror("toplevel-bridge: rename");
    }
}

void on_title(void* data, zwlr_foreign_toplevel_handle_v1*, char const* title)
{
    static_cast<Toplevel*>(data)->title = title ? title : "";
}

void on_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, char const* app_id)
{
    static_cast<Toplevel*>(data)->app_id = app_id ? app_id : "";
}

void on_output_enter(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }
void on_output_leave(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }
void on_parent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) { }

void on_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states)
{
    auto* toplevel = static_cast<Toplevel*>(data);
    toplevel->activated = false;
    toplevel->minimized = false;
    toplevel->maximized = false;
    toplevel->fullscreen = false;

    auto* begin = static_cast<uint32_t*>(states->data);
    size_t count = states->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        switch (begin[i]) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            toplevel->activated = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            toplevel->minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            toplevel->maximized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            toplevel->fullscreen = true;
            break;
        default:
            break;
        }
    }
}

void on_done(void*, zwlr_foreign_toplevel_handle_v1*)
{
    rewrite_state();
}

void on_closed(void* data, zwlr_foreign_toplevel_handle_v1* handle)
{
    auto* toplevel = static_cast<Toplevel*>(data);
    for (auto it = g_toplevels.begin(); it != g_toplevels.end(); ++it) {
        if (*it == toplevel) {
            g_toplevels.erase(it);
            break;
        }
    }
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    delete toplevel;
    rewrite_state();
}

zwlr_foreign_toplevel_handle_v1_listener const handle_listener = {
    .title = on_title,
    .app_id = on_app_id,
    .output_enter = on_output_enter,
    .output_leave = on_output_leave,
    .state = on_state,
    .done = on_done,
    .closed = on_closed,
    .parent = on_parent,
};

void on_toplevel(void*, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle)
{
    auto* toplevel = new Toplevel;
    toplevel->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, toplevel);
    g_toplevels.push_back(toplevel);
}

void on_finished(void*, zwlr_foreign_toplevel_manager_v1* manager)
{
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    g_manager = nullptr;
}

zwlr_foreign_toplevel_manager_v1_listener const manager_listener = {
    .toplevel = on_toplevel,
    .finished = on_finished,
};

void on_seat_capabilities(void*, wl_seat*, uint32_t) { }
void on_seat_name(void*, wl_seat*, char const*) { }

wl_seat_listener const seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

void on_registry_global(void*, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_seat_interface.name) == 0 && !g_seat) {
        g_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(g_seat, &seat_listener, nullptr);
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        g_manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3));
        zwlr_foreign_toplevel_manager_v1_add_listener(g_manager, &manager_listener, nullptr);
    }
}

void on_registry_global_remove(void*, wl_registry*, uint32_t) { }

wl_registry_listener const registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

// "<command> <index>" -> handle. Returns nullptr for a stale or malformed index.
Toplevel* lookup(char const* argument)
{
    if (!argument)
        return nullptr;
    char* end = nullptr;
    long index = strtol(argument, &end, 10);
    if (end == argument || index < 0 || static_cast<size_t>(index) >= g_toplevels.size())
        return nullptr;
    return g_toplevels[static_cast<size_t>(index)];
}

void handle_command(std::string const& line)
{
    auto space = line.find(' ');
    auto command = line.substr(0, space);
    auto argument = space == std::string::npos ? std::string {} : line.substr(space + 1);

    auto* toplevel = lookup(argument.c_str());
    if (!toplevel) {
        fprintf(stderr, "toplevel-bridge: no such window for '%s'\n", line.c_str());
        return;
    }

    if (command == "activate" && g_seat)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, g_seat);
    else if (command == "close")
        zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
    else if (command == "minimize")
        zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
    else
        fprintf(stderr, "toplevel-bridge: unknown command '%s'\n", command.c_str());
}

}

int main()
{
    auto* display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "toplevel-bridge: cannot connect to the compositor\n");
        return 1;
    }

    auto* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!g_manager) {
        fprintf(stderr, "toplevel-bridge: compositor does not export zwlr_foreign_toplevel_manager_v1\n");
        wl_display_disconnect(display);
        return 1;
    }

    if (mkfifo(FIFO_PATH, 0600) != 0 && errno != EEXIST)
        perror("toplevel-bridge: mkfifo");
    // O_RDWR keeps the FIFO from reporting EOF when no writer is around.
    int command_fd = open(FIFO_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (command_fd < 0)
        perror("toplevel-bridge: open fifo");

    rewrite_state();

    int display_fd = wl_display_get_fd(display);
    std::string pending;

    for (;;) {
        pollfd fds[2] = {
            { .fd = display_fd, .events = POLLIN, .revents = 0 },
            { .fd = command_fd, .events = POLLIN, .revents = 0 },
        };
        if (poll(fds, command_fd >= 0 ? 2 : 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("toplevel-bridge: poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) < 0) {
                fprintf(stderr, "toplevel-bridge: compositor connection lost\n");
                break;
            }
        }

        if (command_fd >= 0 && (fds[1].revents & POLLIN)) {
            char buffer[256];
            auto count = read(command_fd, buffer, sizeof(buffer));
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    handle_command(pending);
                    pending.clear();
                } else {
                    pending += buffer[i];
                }
            }
            wl_display_flush(display);
        }
    }

    if (command_fd >= 0)
        close(command_fd);
    wl_display_disconnect(display);
    return 0;
}
