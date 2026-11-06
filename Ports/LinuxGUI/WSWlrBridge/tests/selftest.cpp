/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
// 桥接层自检。注入记录用 stub 表，验证五条规则是否被代码真正遵守：
//   规则 1/4 —— 别名签名相同、指向同一实现、不出现在对外头文件里
//   规则 2   —— 时序/坐标/ID/所有权差异确实由 wrapper 吸收
//   规则 3   —— 桥从未调用 WindowServer 的焦点/布局决策接口
//   规则 5   —— 不链接 wlroots、不依赖 WindowServer 内部类型

#include "WsWlrBridge/Bridge.h"
#include "WsWlrBridge/Classification.h"
#include "detail/EquivalentAliases.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace WsWlrBridge;

namespace {

struct Recorder {
    WsWindowId next_id { 100 };
    WsCookie next_cookie { 500 };

    int create_calls { 0 };
    int destroy_calls { 0 };
    int title_calls { 0 };
    int damage_calls { 0 };
    int commit_calls { 0 };
    int release_calls { 0 };
    int focus_requests { 0 };
    int resize_requests { 0 };
    // 这两个是「越权探测器」：桥一旦调用即违反规则 3。
    int set_focus_calls { 0 };
    int set_geometry_calls { 0 };

    int lock_calls { 0 };
    int unlock_calls { 0 };
    int enter_calls { 0 };
    int clear_focus_calls { 0 };
    int configure_calls { 0 };
    int close_calls { 0 };

    WsWindowId last_created_id { kInvalidWindowId };
    WsCookie last_cookie { kInvalidCookie };
    Rect created_geometry {};
    std::string last_title;
    std::vector<Rect> damage_rects;
    WlrSurfaceHandle last_focus_surface { nullptr };
    WlrSerial last_focus_serial { 0 };
    int last_conf_width { 0 };
    int last_conf_height { 0 };
    WlrSerial last_conf_serial { 0 };

    void reset() { *this = Recorder {}; }
};

Recorder g;

// ── ws 侧 stub ───────────────────────────────────────────────────────────────
WsWindowId stub_create(Rect const* geometry, char const* title)
{
    (void)title;
    ++g.create_calls;
    g.created_geometry = *geometry;
    g.last_created_id = g.next_id++;
    return g.last_created_id;
}
void stub_destroy(WsWindowId) { ++g.destroy_calls; }
void stub_set_title(WsWindowId, char const* title)
{
    ++g.title_calls;
    g.last_title = title ? title : "";
}
void stub_damage(WsWindowId, Rect const* rects, size_t count)
{
    ++g.damage_calls;
    for (size_t i = 0; i < count; ++i)
        g.damage_rects.push_back(rects[i]);
}
WsCookie stub_commit(WsWindowId, BufferRef const*)
{
    ++g.commit_calls;
    g.last_cookie = g.next_cookie++;
    return g.last_cookie;
}
void stub_release(uintptr_t) { ++g.release_calls; }
void stub_note_focus(WsWindowId, WlrSerial) { ++g.focus_requests; }
void stub_note_resize(WsWindowId, Size const*, WlrSerial) { ++g.resize_requests; }
void stub_forbidden_focus(WsWindowId, bool) { ++g.set_focus_calls; }
void stub_forbidden_geometry(WsWindowId, Rect const*) { ++g.set_geometry_calls; }

// ── wlr 侧 stub ─────────────────────────────────────────────────────────────
void stub_lock(WlrBufferHandle) { ++g.lock_calls; }
void stub_unlock(WlrBufferHandle) { ++g.unlock_calls; }
void stub_configure(WlrSurfaceHandle, int w, int h, WlrSerial serial)
{
    ++g.configure_calls;
    g.last_conf_width = w;
    g.last_conf_height = h;
    g.last_conf_serial = serial;
}
void stub_close(WlrSurfaceHandle) { ++g.close_calls; }
void stub_enter(WlrSurfaceHandle surface, WlrSerial serial)
{
    ++g.enter_calls;
    g.last_focus_surface = surface;
    g.last_focus_serial = serial;
}
void stub_clear_focus(WlrSerial) { ++g.clear_focus_calls; }
void stub_frame_done(WlrSurfaceHandle, uint32_t) { }

WsApi make_ws_api()
{
    return WsApi {
        &stub_create,
        &stub_destroy,
        &stub_set_title,
        &stub_damage,
        &stub_commit,
        &stub_release,
        &stub_note_focus,
        &stub_note_resize,
        &stub_forbidden_focus,
        &stub_forbidden_geometry,
    };
}

WlrApi make_wlr_api()
{
    return WlrApi {
        &stub_lock,
        &stub_unlock,
        &stub_configure,
        &stub_close,
        &stub_enter,
        &stub_clear_focus,
        &stub_frame_done,
    };
}

int g_failures = 0;
#define CHECK(expr, msg)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::cerr << "  FAIL: " << msg << "  [" #expr "]\n";                \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

void log(char const* name) { std::cout << "[ RUN ] " << name << "\n"; }

// ── 规则 1：等价别名 ────────────────────────────────────────────────────────
void test_equivalent_aliases()
{
    log("规则1 等价别名共用一份实现");
    using namespace WsWlrBridge::detail;
    CHECK(wlr_rect_contains_point == ws_rect_contains_point, "两个名字必须指向同一函数");
    CHECK(wlr_rect_intersect == ws_rect_intersect, "两个名字必须指向同一函数");
    CHECK(wlr_scale_size == ws_scale_size, "两个名字必须指向同一函数");
    CHECK(wlr_bytes_per_pixel == ws_bytes_per_pixel, "两个名字必须指向同一函数");

    Rect r { 0, 0, 10, 10 };
    CHECK(wlr_rect_contains_point(r, 9, 9), "右下边界为排他");
    CHECK(!wlr_rect_contains_point(r, 10, 0), "x==right 不算命中");
    CHECK((ws_scale_size({ 800, 600 }, 2) == Size { 400, 300 }), "单位换算一致");
    CHECK(wlr_bytes_per_pixel(0x34325241) == 4, "fourcc 查表一致");
}

// ── 规则 4：别名不向外暴露 ──────────────────────────────────────────────────
void test_public_header_isolation()
{
    log("规则4 别名/内部状态不出现在对外头文件");
    namespace fs = std::filesystem;
    std::vector<std::string> banned = {
        "EquivalentAliases", "IndependentState", "AdaptationWrappers",
        "detail::", "ws_rect_contains_point", "wlr_scale_size",
    };
    size_t files = 0;
    for (auto const& entry : fs::recursive_directory_iterator(WSWLR_PUBLIC_INCLUDE_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".h")
            continue;
        ++files;
        std::ifstream in(entry.path());
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto text = buffer.str();
        for (auto const& token : banned)
            CHECK(text.find(token) == std::string::npos, "对外头泄漏了内部符号");
    }
    CHECK(files >= 4, "应检查到全部对外契约头");
}

// ── 分类登记 ────────────────────────────────────────────────────────────────
void test_classification()
{
    log("三类区分：登记齐全");
    size_t counts[3] = { 0, 0, 0 };
    for (size_t i = 0; i < classification_table_size(); ++i)
        ++counts[static_cast<size_t>(classification_table()[i].kind)];
    CHECK(counts[0] > 0 && counts[1] > 0 && counts[2] > 0, "三类都必须非空");
    CHECK(classification_table_size() >= 30, "登记表应覆盖主要入口");

    char const* public_entries[] = {
        "Bridge::on_wlr_new_surface", "Bridge::on_wlr_map", "Bridge::on_wlr_commit",
        "Bridge::on_wlr_damage", "Bridge::on_wlr_unmap", "Bridge::on_wlr_destroy",
        "Bridge::on_wlr_title_changed", "Bridge::on_wlr_focus_request",
        "Bridge::on_wlr_resize_request", "Bridge::on_ws_focus_decided",
        "Bridge::on_ws_configure_decided", "Bridge::on_ws_close_decided",
        "Bridge::on_ws_commit_done",
    };
    for (auto const* name : public_entries) {
        BridgeKind kind;
        CHECK(bridge_kind_of(name, kind), "每个对外入口都要登记分类");
        CHECK(kind == BridgeKind::AdaptationWrapper, "门面归入适配层的转发入口");
    }
}

// ── 规则 3：桥不做任何决策 ──────────────────────────────────────────────────
void test_no_policy_decisions()
{
    log("规则3 焦点/布局/位置决策全部留给 Serenity");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1100);
    auto* buffer = reinterpret_cast<void*>(0x2200);
    BufferRef ref {};
    ref.handle = 0xabc;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_map(surface, { 800, 600 }, 1);
    bridge.on_wlr_commit(surface, buffer, ref, nullptr, 0, 1);
    auto id = g.last_created_id;

    CHECK(id != kInvalidWindowId, "首帧 commit 后应建立 ws 窗口");
    CHECK(g.created_geometry.x == kOriginUnspecified && g.created_geometry.y == kOriginUnspecified,
        "位置必须是未指定哨兵，桥不得编造坐标");
    CHECK(g.created_geometry.width == 800 && g.created_geometry.height == 600, "尺寸只做单位翻译");
    CHECK(g.set_focus_calls == 0 && g.set_geometry_calls == 0,
        "桥不得调用 WindowServer 的焦点/布局决策接口");

    // 客户端请求：只上报，绝不自行生效。
    bridge.on_wlr_focus_request(surface, 77);
    CHECK(g.focus_requests == 1, "focus 请求应上报 Serenity");
    CHECK(g.set_focus_calls == 0, "请求不能变成 ws 侧焦点设置");
    CHECK(g.enter_calls == 0, "请求阶段不能给 wlr 侧设焦点");
    bridge.on_wlr_resize_request(surface, { 640, 480 }, 78);
    CHECK(g.resize_requests == 1, "resize 请求应上报 Serenity");
    CHECK(g.set_geometry_calls == 0 && g.configure_calls == 0, "请求不能变成决定");

    // Serenity 的决定：原样转发，不检查、不翻转。
    bridge.on_ws_focus_decided(id, true, 77);
    CHECK(g.enter_calls == 1 && g.last_focus_surface == surface && g.last_focus_serial == 77,
        "激活决定应原样翻译成 wlr_seat_keyboard_enter");
    CHECK(g.set_focus_calls == 0, "决定只去 wlr 侧，桥不反向改 WS 状态");
    bridge.on_ws_focus_decided(id, false, 79);
    CHECK(g.clear_focus_calls == 1, "失活决定应转发为 clear_focus");
    bridge.on_ws_configure_decided(id, { 12, 34, 640, 480 }, 78);
    CHECK(g.configure_calls == 1 && g.last_conf_width == 640 && g.last_conf_height == 480
            && g.last_conf_serial == 78,
        "configure 决定只翻译尺寸，x/y 因 wlr 无对应字段丢弃");
    bridge.on_ws_close_decided(id);
    CHECK(g.close_calls == 1, "close 决定应转发");
}

// ── 规则 2：回调时序 ────────────────────────────────────────────────────────
void test_callback_ordering()
{
    log("规则2 commit 早于 map 时 FIFO 排队并按原序补发");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1200);
    BufferRef ref {};
    ref.handle = 2;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2300), ref, nullptr, 0, 5);
    CHECK(bridge.pending_event_count() == 1, "未 map 的 commit 应排队");
    CHECK(g.create_calls == 0, "map 之前不建窗");

    bridge.on_wlr_map(surface, { 400, 300 }, 1);
    CHECK(g.create_calls == 1 && g.commit_calls == 1, "map 后补发排队的 commit");
    CHECK(bridge.pending_event_count() == 0, "队列应清空");
    CHECK(bridge.mapped_window_count() == 1, "镜像状态同步为已映射");
}

// ── 规则 2：状态同步 + 参数翻译 ─────────────────────────────────────────────
void test_state_sync_and_translation()
{
    log("规则2 title/damage 早于建窗时的状态同步与坐标翻译");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1300);
    BufferRef ref {};
    ref.handle = 3;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_title_changed(surface, "复古终端");
    Rect damage { 20, 40, 100, 60 };
    bridge.on_wlr_damage(surface, &damage, 1);
    CHECK(g.title_calls == 0 && g.damage_calls == 0, "窗口未建时先记账");

    bridge.on_wlr_map(surface, { 400, 200 }, 2); // scale = 2
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2400), ref, nullptr, 0, 6);
    CHECK(g.last_title == "复古终端", "建窗后补发 title");
    CHECK(g.damage_calls == 1, "建窗后补发排队的 damage");
    CHECK(g.damage_rects.size() == 1, "不合并、不丢弃矩形");
    CHECK(g.damage_rects[0] == (Rect { 10, 20, 50, 30 }), "buffer 坐标按 scale=2 译为逻辑坐标");
}

// ── 规则 2：对象所有权 / 生命周期 ───────────────────────────────────────────
void test_buffer_lifetime()
{
    log("规则2 wlr_buffer 引用计数 ↔ WS 借用视图");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1400);
    BufferRef ref {};
    ref.handle = 4;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_map(surface, { 200, 100 }, 1);
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2500), ref, nullptr, 0, 7);
    CHECK(g.lock_calls == 1 && g.unlock_calls == 0, "commit 时 lock，不能提前 unlock");
    CHECK(bridge.pending_release_count() == 1, "登记待释放票据");

    bridge.on_ws_commit_done(g.last_cookie);
    CHECK(g.unlock_calls == 1 && g.release_calls == 1, "WS 消费完成才释放两端");
    CHECK(bridge.pending_release_count() == 0, "票据兑现后清零");
}

// ── 规则 2：对象归属差异 + 陈旧 id ──────────────────────────────────────────
void test_unmap_remap()
{
    log("规则2 unmap 可逆 / ws_destroy 终态 + 陈旧 id 隔离");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1500);
    BufferRef ref {};
    ref.handle = 5;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_map(surface, { 320, 240 }, 1);
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2600), ref, nullptr, 0, 8);
    auto old_id = g.last_created_id;

    bridge.on_wlr_unmap(surface);
    CHECK(g.destroy_calls == 1 && bridge.mapped_window_count() == 0, "unmap 销毁 ws 窗口");
    bridge.on_ws_focus_decided(old_id, true, 99);
    CHECK(g.enter_calls == 0, "陈旧 id 不得复活，桥也不得猜测替代窗口");

    bridge.on_wlr_map(surface, { 320, 240 }, 1);
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2600), ref, nullptr, 0, 9);
    CHECK(g.create_calls == 2 && g.last_created_id != old_id, "remap 用新的 ws 窗口 id");
}

void test_destroy_releases_pending()
{
    log("规则2 surface 销毁时兑现未决 buffer 票据");
    g.reset();
    Bridge bridge(make_ws_api(), make_wlr_api());
    auto* surface = reinterpret_cast<void*>(0x1600);
    BufferRef ref {};
    ref.handle = 6;

    bridge.on_wlr_new_surface(surface);
    bridge.on_wlr_map(surface, { 100, 100 }, 1);
    bridge.on_wlr_commit(surface, reinterpret_cast<void*>(0x2700), ref, nullptr, 0, 10);
    CHECK(bridge.pending_release_count() == 1, "存在未决票据");

    bridge.on_wlr_destroy(surface);
    CHECK(g.unlock_calls == 1, "销毁时解锁 wlr_buffer，避免引用泄漏");
    CHECK(bridge.pending_release_count() == 0, "票据已清理");
    CHECK(bridge.mapped_window_count() == 0, "镜像已清理");
}

}

int main()
{
    std::cout << "ws-wlr-bridge selftest\n";
    test_equivalent_aliases();
    test_public_header_isolation();
    test_classification();
    test_no_policy_decisions();
    test_callback_ordering();
    test_state_sync_and_translation();
    test_buffer_lifetime();
    test_unmap_remap();
    test_destroy_releases_pending();

    if (g_failures != 0) {
        std::cerr << "FAILED: " << g_failures << " check(s)\n";
        return 1;
    }
    std::cout << "OK: 全部检查通过\n";
    return 0;
}
