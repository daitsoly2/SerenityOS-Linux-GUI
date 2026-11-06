/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
// ─────────────────────────────────────────────────────────────────────────────
// serenity-ws-wlr-bridge-host —— 让桥「先跑起来」的最小真实载体。
//
// 定位（重要）：这是「labwc 托管外来 Linux 应用」路线里的**策略/任务栏桥**，
// 不是合成器。labwc/wlroots 负责所有 Wayland 协议；本进程只是它的一个普通
// Wayland 客户端，绑定 zwlr_foreign_toplevel_manager_v1，把顶层窗口事件翻译
// 成 Bridge::on_wlr_*，再把 Serenity 的决定翻译回 wlr_* 调用。
//
// 因此它完全绕开了 serenity-wayland-host 的 xdg-shell 实现：外来 GTK/Qt/GTK4
// 应用不再经过那条 bug 多、协议缺的链路。
//
//   labwc (wlroots)  ──foreign-toplevel──>  host ──> Bridge ──> 状态文件 / FIFO
//        ^                                  │
//        └──── activate/close <─────────────┘  (Serenity 的决定，桥只转发)
//
// 先期约定（等之后有更优秀方案再替换）：
//   * ws 侧用文件 + FIFO，而不是新 IPC：不改 WindowServer 核心（规则 5）。
//   * foreign-toplevel 协议不给窗口尺寸，map 用 0x0，位置用哨兵交 Serenity，
//     桥不编造坐标（规则 3）。
//   * 路径可用 WSWLR_STATE_PATH / WSWLR_CMD_PATH / WSWLR_EVENT_PATH 覆盖；
//     生产环境应放到 $XDG_RUNTIME_DIR 或 /run/user/<uid>/。
//
// 用法：
//   WAYLAND_DISPLAY=wayland-0 ./serenity-ws-wlr-bridge-host
//   echo "activate 1" > /tmp/serenity-ws-wlr-cmd
// ─────────────────────────────────────────────────────────────────────────────

#include "WsWlrBridge/Bridge.h"
#include "WsWlrBridge/Classification.h"

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <wayland-client.h>

#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace WsWlrBridge;

namespace {

char const* state_path()
{
    char const* value = getenv("WSWLR_STATE_PATH");
    return value ? value : "/tmp/serenity-ws-wlr-windows";
}
char const* command_path()
{
    char const* value = getenv("WSWLR_CMD_PATH");
    return value ? value : "/tmp/serenity-ws-wlr-cmd";
}
char const* event_path()
{
    char const* value = getenv("WSWLR_EVENT_PATH");
    return value ? value : "/tmp/serenity-ws-wlr-events";
}

struct HostState {
    Bridge* bridge { nullptr };
    wl_display* display { nullptr };
    wl_seat* seat { nullptr };
    zwlr_foreign_toplevel_manager_v1* manager { nullptr };

    WsWindowId next_ws_id { 1 };
    WlrSerial next_serial { 1 };

    // Bridge 的调用链是同步的：进入 on_wlr_* 前设置，出来后清空。
    // ws_window_create 用它把「刚建立的 ws 窗口」绑回 wlr handle。
    WlrSurfaceHandle current_surface { nullptr };

    std::unordered_map<WlrSurfaceHandle, WsWindowId> handle_to_ws;
    std::unordered_map<WsWindowId, WlrSurfaceHandle> ws_to_handle;
    std::unordered_map<WlrSurfaceHandle, std::string> app_ids;
    std::unordered_map<WsWindowId, std::string> titles;

    size_t commits { 0 };
    size_t damages { 0 };
    int forbidden_calls { 0 };
};

HostState g;

void log_event(char const* format, ...)
{
    auto* file = fopen(event_path(), "a");
    if (!file)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

// [适配] ① 参数翻译：把桥内的窗口状态写成 shell 能轮询的纯文本。
// 原子替换（.tmp + rename），shell 永远不会读到半截。
void rewrite_state()
{
    std::string tmp = std::string(state_path()) + ".tmp";
    auto* file = fopen(tmp.c_str(), "w");
    if (!file) {
        perror("ws-wlr-host: open state tmp");
        return;
    }
    fprintf(file, "# ws_id\tapp_id\ttitle\twlr_handle\n");
    for (auto const& entry : g.ws_to_handle) {
        auto id = entry.first;
        auto* handle = entry.second;
        auto app = g.app_ids.find(handle);
        auto title = g.titles.find(id);
        std::string safe_title;
        if (title != g.titles.end()) {
            for (char c : title->second)
                safe_title += (c == '\t' || c == '\n') ? ' ' : c;
        }
        fprintf(file, "%u\t%s\t%s\t%p\n", id,
            app == g.app_ids.end() ? "" : app->second.c_str(),
            safe_title.c_str(), handle);
    }
    fprintf(file, "# commits=%zu damages=%zu forbidden=%d\n",
        g.commits, g.damages, g.forbidden_calls);
    fclose(file);
    if (rename(tmp.c_str(), state_path()) != 0)
        perror("ws-wlr-host: rename state");
}

// ── WsApi backend：桥 → Serenity（先期用文件，之后换真 IPC）─────────────────
WsWindowId ws_create(Rect const* geometry, char const*)
{
    auto id = g.next_ws_id++;
    g.ws_to_handle[id] = g.current_surface;
    g.handle_to_ws[g.current_surface] = id;
    g.titles[id] = "";
    log_event("create ws=%u handle=%p origin=%s", id, g.current_surface,
        geometry && geometry->x == kOriginUnspecified ? "serenity-decides" : "BRIDGE-DECIDED");
    rewrite_state();
    return id;
}

void ws_destroy(WsWindowId id)
{
    auto it = g.ws_to_handle.find(id);
    if (it != g.ws_to_handle.end()) {
        g.handle_to_ws.erase(it->second);
        g.app_ids.erase(it->second);
        g.ws_to_handle.erase(it);
    }
    g.titles.erase(id);
    log_event("destroy ws=%u", id);
    rewrite_state();
}

void ws_set_title(WsWindowId id, char const* title)
{
    g.titles[id] = title ? title : "";
    log_event("title ws=%u %s", id, g.titles[id].c_str());
    rewrite_state();
}

void ws_damage(WsWindowId, Rect const*, size_t) { ++g.damages; }

WsCookie ws_commit(WsWindowId id, BufferRef const*)
{
    ++g.commits;
    log_event("commit ws=%u", id);
    // 先期：WS 同步消费，没有后续回调，桥当场释放两端。
    return kInvalidCookie;
}

void ws_release(uintptr_t) { }
void ws_note_focus(WsWindowId id, WlrSerial serial) { log_event("focus-request ws=%u serial=%u", id, serial); }
void ws_note_resize(WsWindowId id, Size const* size, WlrSerial serial)
{
    log_event("resize-request ws=%u %dx%d serial=%u", id, size ? size->width : 0, size ? size->height : 0, serial);
}
void ws_forbidden_focus(WsWindowId, bool) { ++g.forbidden_calls; log_event("VIOLATION ws_window_set_focus"); }
void ws_forbidden_geometry(WsWindowId, Rect const*) { ++g.forbidden_calls; log_event("VIOLATION ws_window_set_geometry"); }

// ── WlrApi backend：桥 → labwc/wlroots（Serenity 的决定在这里落地）──────────
void wlr_lock(WlrBufferHandle) { }
void wlr_unlock(WlrBufferHandle) { }
void wlr_configure(WlrSurfaceHandle, int width, int height, WlrSerial)
{
    // foreign-toplevel 协议没有 configure；尺寸由 labwc 自己管。
    log_event("configure-decision %dx%d (no protocol counterpart)", width, height);
}
void wlr_close(WlrSurfaceHandle surface)
{
    auto* handle = static_cast<zwlr_foreign_toplevel_handle_v1*>(surface);
    zwlr_foreign_toplevel_handle_v1_close(handle);
    wl_display_flush(g.display);
    log_event("close-decision handle=%p", surface);
}
void wlr_enter(WlrSurfaceHandle surface, WlrSerial serial)
{
    auto* handle = static_cast<zwlr_foreign_toplevel_handle_v1*>(surface);
    if (g.seat)
        zwlr_foreign_toplevel_handle_v1_activate(handle, g.seat);
    wl_display_flush(g.display);
    log_event("focus-decision activate handle=%p serial=%u", surface, serial);
}
void wlr_clear_focus(WlrSerial serial) { log_event("focus-decision deactivate serial=%u", serial); }
void wlr_frame_done(WlrSurfaceHandle, uint32_t) { }

WsApi make_ws_api()
{
    return WsApi { &ws_create, &ws_destroy, &ws_set_title, &ws_damage, &ws_commit,
        &ws_release, &ws_note_focus, &ws_note_resize, &ws_forbidden_focus, &ws_forbidden_geometry };
}

WlrApi make_wlr_api()
{
    return WlrApi { &wlr_lock, &wlr_unlock, &wlr_configure, &wlr_close,
        &wlr_enter, &wlr_clear_focus, &wlr_frame_done };
}

// ── foreign-toplevel 事件 → Bridge::on_wlr_* ────────────────────────────────
// [适配] 语义层级差异：协议给的是「toplevel 级」事件，Bridge 期望
// 「surface 级」生命周期。先期做最小对齐，完整状态机留到后面再优化。
void on_title(void* data, zwlr_foreign_toplevel_handle_v1*, char const* title)
{
    auto* handle = static_cast<WlrSurfaceHandle>(data);
    g.current_surface = handle;
    g.bridge->on_wlr_title_changed(handle, title ? title : "");
    g.current_surface = nullptr;
}

void on_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, char const* app_id)
{
    g.app_ids[data] = app_id ? app_id : "";
    rewrite_state();
}

void on_output_enter(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }
void on_output_leave(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }
void on_parent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) { }

void on_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states)
{
    // 规则 3：激活状态是 **wlr 侧的通知**，不是 Serenity 的决定。
    // 桥只记录，绝不据此改焦点。
    auto* begin = static_cast<uint32_t*>(states->data);
    size_t count = states->size / sizeof(uint32_t);
    bool activated = false;
    for (size_t i = 0; i < count; ++i) {
        if (begin[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            activated = true;
    }
    log_event("wlr-state handle=%p activated=%d (notification only)", data, activated);
}

void on_done(void*, zwlr_foreign_toplevel_handle_v1*) { rewrite_state(); }

void on_closed(void* data, zwlr_foreign_toplevel_handle_v1* handle)
{
    g.current_surface = data;
    g.bridge->on_wlr_destroy(data);
    g.current_surface = nullptr;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    rewrite_state();
}

zwlr_foreign_toplevel_handle_v1_listener const handle_listener = {
    on_title,
    on_app_id,
    on_output_enter,
    on_output_leave,
    on_state,
    on_done,
    on_closed,
    on_parent,
};

void on_toplevel(void*, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle)
{
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, handle);

    g.current_surface = handle;
    g.bridge->on_wlr_new_surface(handle);
    // foreign-toplevel 不给尺寸：0x0，不编造布局（规则 3）。
    g.bridge->on_wlr_map(handle, { 0, 0 }, 1);
    BufferRef ref {};
    g.bridge->on_wlr_commit(handle, nullptr, ref, nullptr, 0, g.next_serial++);
    g.current_surface = nullptr;
    log_event("wlr-toplevel handle=%p", handle);
    rewrite_state();
}

void on_finished(void*, zwlr_foreign_toplevel_manager_v1* manager)
{
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    g.manager = nullptr;
}

zwlr_foreign_toplevel_manager_v1_listener const manager_listener = { on_toplevel, on_finished };

void on_seat_capabilities(void*, wl_seat*, uint32_t) { }
void on_seat_name(void*, wl_seat*, char const*) { }
wl_seat_listener const seat_listener = { on_seat_capabilities, on_seat_name };

void on_registry_global(void*, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_seat_interface.name) == 0 && !g.seat) {
        g.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(g.seat, &seat_listener, nullptr);
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        g.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3));
        zwlr_foreign_toplevel_manager_v1_add_listener(g.manager, &manager_listener, nullptr);
    }
}
void on_registry_global_remove(void*, wl_registry*, uint32_t) { }
wl_registry_listener const registry_listener = { on_registry_global, on_registry_global_remove };

// ── Serenity 的决定（先期由 FIFO 注入，之后换成真正的 WS IPC）────────────────
void handle_command(std::string const& line)
{
    auto space = line.find(' ');
    auto command = line.substr(0, space);
    auto rest = space == std::string::npos ? std::string {} : line.substr(space + 1);
    WsWindowId id = static_cast<WsWindowId>(strtoul(rest.c_str(), nullptr, 10));
    if (id == kInvalidWindowId || g.ws_to_handle.find(id) == g.ws_to_handle.end()) {
        fprintf(stderr, "ws-wlr-host: unknown window in '%s'\n", line.c_str());
        return;
    }
    auto serial = g.next_serial++;
    if (command == "activate")
        g.bridge->on_ws_focus_decided(id, true, serial);
    else if (command == "deactivate")
        g.bridge->on_ws_focus_decided(id, false, serial);
    else if (command == "close")
        g.bridge->on_ws_close_decided(id);
    else if (command == "configure") {
        int width = 0, height = 0;
        sscanf(rest.c_str(), "%*u %d %d", &width, &height);
        g.bridge->on_ws_configure_decided(id, { 0, 0, width, height }, serial);
    } else {
        fprintf(stderr, "ws-wlr-host: unknown command '%s'\n", command.c_str());
    }
}

}

int main()
{
    auto ws_api = make_ws_api();
    auto wlr_api = make_wlr_api();
    Bridge bridge(ws_api, wlr_api);
    g.bridge = &bridge;

    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        fprintf(stderr, "ws-wlr-host: cannot connect to the compositor\n");
        return 1;
    }
    auto* registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    wl_display_roundtrip(g.display);
    if (!g.manager) {
        fprintf(stderr, "ws-wlr-host: compositor does not export zwlr_foreign_toplevel_manager_v1\n");
        wl_display_disconnect(g.display);
        return 1;
    }

    if (mkfifo(command_path(), 0600) != 0 && errno != EEXIST)
        perror("ws-wlr-host: mkfifo");
    int command_fd = open(command_path(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (command_fd < 0)
        perror("ws-wlr-host: open fifo");

    rewrite_state();
    log_event("start entries=%zu wayland=%s", classification_table_size(), getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");
    fprintf(stderr, "ws-wlr-host: running (state=%s cmd=%s)\n", state_path(), command_path());

    int display_fd = wl_display_get_fd(g.display);
    std::string pending;
    for (;;) {
        pollfd fds[2] = { { display_fd, POLLIN, 0 }, { command_fd, POLLIN, 0 } };
        if (poll(fds, command_fd >= 0 ? 2 : 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("ws-wlr-host: poll");
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(g.display) < 0) {
                fprintf(stderr, "ws-wlr-host: compositor connection lost\n");
                break;
            }
        }
        if (command_fd >= 0 && (fds[1].revents & POLLIN)) {
            char buffer[256];
            auto count = read(command_fd, buffer, sizeof(buffer));
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    if (!pending.empty())
                        handle_command(pending);
                    pending.clear();
                } else {
                    pending += buffer[i];
                }
            }
            wl_display_flush(g.display);
        }
    }

    if (command_fd >= 0)
        close(command_fd);
    wl_display_disconnect(g.display);
    log_event("stop");
    return 0;
}
