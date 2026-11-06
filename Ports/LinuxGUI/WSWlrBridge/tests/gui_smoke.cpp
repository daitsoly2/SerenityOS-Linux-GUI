// 用 labwc/Xwayland 里「真实 Linux GUI 程序的顶层窗口」数据驱动新桥。
// 只依赖部署出来的 /serenity/include + /serenity/lib/libws-wlr-bridge.a。
#include <WsWlrBridge/Bridge.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace WsWlrBridge;

namespace {

struct RealWindow {
    uintptr_t xid;
    std::string title;
    int width;
    int height;
};

struct Recorder {
    WsWindowId next_id { 200 };
    std::vector<WsWindowId> created;
    std::vector<std::string> titles;
    std::vector<Rect> geometries;
    int damage_calls { 0 };
    int commit_calls { 0 };
    int forbidden_focus { 0 };
    int forbidden_geometry { 0 };
    int enter_calls { 0 };
    WlrSurfaceHandle entered_surface { nullptr };
};

Recorder g;

WsWindowId stub_create(Rect const* rect, char const*)
{
    g.created.push_back(g.next_id);
    g.geometries.push_back(*rect);
    return g.next_id++;
}
void stub_destroy(WsWindowId) { }
void stub_set_title(WsWindowId, char const* title) { g.titles.emplace_back(title ? title : ""); }
void stub_damage(WsWindowId, Rect const*, size_t) { ++g.damage_calls; }
WsCookie stub_commit(WsWindowId, BufferRef const*) { ++g.commit_calls; return 900; }
void stub_release(uintptr_t) { }
void stub_note_focus(WsWindowId, WlrSerial) { }
void stub_note_resize(WsWindowId, Size const*, WlrSerial) { }
void stub_forbidden_focus(WsWindowId, bool) { ++g.forbidden_focus; }
void stub_forbidden_geometry(WsWindowId, Rect const*) { ++g.forbidden_geometry; }
void stub_lock(WlrBufferHandle) { }
void stub_unlock(WlrBufferHandle) { }
void stub_configure(WlrSurfaceHandle, int, int, WlrSerial) { }
void stub_close(WlrSurfaceHandle) { }
void stub_enter(WlrSurfaceHandle surface, WlrSerial) { ++g.enter_calls; g.entered_surface = surface; }
void stub_clear_focus(WlrSerial) { }
void stub_frame_done(WlrSurfaceHandle, uint32_t) { }

WsApi make_ws_api()
{
    return WsApi { &stub_create, &stub_destroy, &stub_set_title, &stub_damage,
        &stub_commit, &stub_release, &stub_note_focus, &stub_note_resize,
        &stub_forbidden_focus, &stub_forbidden_geometry };
}

WlrApi make_wlr_api()
{
    return WlrApi { &stub_lock, &stub_unlock, &stub_configure, &stub_close,
        &stub_enter, &stub_clear_focus, &stub_frame_done };
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: wswlr-gui-smoke <windows.tsv>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    std::vector<RealWindow> windows;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string xid_text, title, w_text, h_text;
        if (!std::getline(fields, xid_text, '\t') || !std::getline(fields, title, '\t')
            || !std::getline(fields, w_text, '\t') || !std::getline(fields, h_text, '\t'))
            continue;
        windows.push_back({ std::stoull(xid_text, nullptr, 16), title,
            std::stoi(w_text), std::stoi(h_text) });
    }
    if (windows.empty()) {
        std::cerr << "no real top-level windows found\n";
        return 2;
    }

    Bridge bridge(make_ws_api(), make_wlr_api());
    WlrSerial serial = 1000;
    for (auto const& window : windows) {
        auto* surface = reinterpret_cast<void*>(window.xid);
        bridge.on_wlr_new_surface(surface);
        bridge.on_wlr_map(surface, { window.width, window.height }, 1);
        bridge.on_wlr_title_changed(surface, window.title.c_str());
        BufferRef ref {};
        ref.handle = window.xid;
        ref.width = window.width;
        ref.height = window.height;
        Rect damage { 0, 0, window.width, window.height };
        bridge.on_wlr_commit(surface, reinterpret_cast<void*>(window.xid + 1), ref, &damage, 1, serial++);
        bridge.on_ws_commit_done(900);
    }

    std::cout << "真实窗口数=" << windows.size()
              << "  建窗=" << g.created.size()
              << "  commit=" << g.commit_calls
              << "  damage=" << g.damage_calls
              << "  title同步=" << g.titles.size() << "\n";
    for (size_t i = 0; i < windows.size() && i < g.created.size(); ++i) {
        std::cout << "  xid=0x" << std::hex << windows[i].xid << std::dec
                  << "  " << windows[i].width << "x" << windows[i].height
                  << "  \"" << windows[i].title << "\"  -> ws_window_id=" << g.created[i]
                  << "  origin=" << (g.geometries[i].x == kOriginUnspecified ? "由Serenity决定" : "被桥指定(违规)")
                  << "\n";
    }

    // Serenity 的焦点决定：验证 wlr 侧拿回的是同一个真实 surface。
    auto* first_surface = reinterpret_cast<void*>(windows.front().xid);
    bridge.on_ws_focus_decided(g.created.front(), true, serial);
    bool focus_ok = g.enter_calls == 1 && g.entered_surface == first_surface;
    bool no_policy = g.forbidden_focus == 0 && g.forbidden_geometry == 0;
    bool all_mapped = bridge.mapped_window_count() == windows.size();
    bool origin_ok = true;
    for (auto const& geometry : g.geometries)
        origin_ok = origin_ok && geometry.x == kOriginUnspecified;

    std::cout << "焦点决定转发=" << (focus_ok ? "OK" : "FAIL")
              << "  无越权决策=" << (no_policy ? "OK" : "FAIL")
              << "  全部映射=" << (all_mapped ? "OK" : "FAIL")
              << "  位置未由桥决定=" << (origin_ok ? "OK" : "FAIL") << "\n";

    return (focus_ok && no_policy && all_mapped && origin_ok) ? 0 : 1;
}
