/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * serenity-wlroots-host
 *
 * 结构上和已退役的旧桥（原 WaylandHost）一致 —— 一个 LibGUI 客户端，把每个外来
 * toplevel 变成一个普通 Serenity 窗口。区别只在协议端：
 *
 *   wayland_host.cpp : 自己实现 wl_compositor / wl_shm / xdg_wm_base / XWM
 *   本文件           : 交给裁减版 wlroots（wlroots_proto.c），自己只做
 *                      「wlr_buffer 的 CPU 像素 → Gfx::Bitmap → GUI::Window」
 *
 * 因为没有 wlr_scene / wlr_output / GLES，这条路径上不存在纹理缓存、
 * damage ring、swapchain buffer-age —— 画面由 WindowServer 的 Compositor
 * 统一合成后经 KMS 直出，和原生窗口走同一条已验证干净的路径。
 */

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Application.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Event.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

#include <drm_fourcc.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* X11 侧整套窗口管理直接复用旧桥的编译单元，不在新桥里重写一遍。 */
#include "../X11Compat/xwayland_bridge.h"
#include "../X11Compat/xwm.h"

extern "C" {
#include "wlroots_proto.h"
}

namespace {

static wlroots_proto* s_proto = nullptr;

// Wayland shm 给的是非预乘的 ARGB/XRGB，Gfx::Bitmap 是预乘 BGRA。
// 和小端 uint32 布局一致（内存字节 B,G,R,A），只差 alpha 语义。
static void copy_buffer_to_bitmap(void const* pixels, int width, int height, int stride,
    uint32_t format, Gfx::Bitmap& bitmap)
{
    bool const opaque = (format == DRM_FORMAT_XRGB8888);
    for (int y = 0; y < height; ++y) {
        auto const* src = reinterpret_cast<uint32_t const*>(
            static_cast<uint8_t const*>(pixels) + static_cast<size_t>(y) * stride);
        auto* dst = reinterpret_cast<uint32_t*>(bitmap.scanline(y));
        for (int x = 0; x < width; ++x) {
            uint32_t argb = src[x];
            if (opaque) {
                dst[x] = argb | 0xff000000u;
                continue;
            }
            uint32_t a = (argb >> 24) & 0xff;
            if (a == 0xff) {
                dst[x] = argb;
            } else if (a == 0) {
                dst[x] = 0;
            } else {
                uint32_t r = (argb >> 16) & 0xff;
                uint32_t g = (argb >> 8) & 0xff;
                uint32_t b = argb & 0xff;
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
                dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

static uint32_t now_msec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Serenity KeyCode (Kernel/API/KeyCode.h, u8 enum) -> Linux evdev keycode。
 * 与原旧桥（已删除的 WaylandHost/wayland_host.cpp）::serenity_key_to_evdev 是同一份映射：
 * Serenity 把 shift 后的符号也做成独立 KeyCode（Key_ExclamationPoint 等），
 * 这里一律映射回物理键，由 wl_keyboard.modifiers 让客户端自己解出字符。
 */
static int serenity_key_to_evdev(unsigned key)
{
    switch (key) {
    case 0x01: return 1;   // Escape
    case 0x02: return 15;  // Tab
    case 0x03: return 14;  // Backspace
    case 0x04: return 28;  // Return
    case 0x05: return 110; // Insert
    case 0x06: return 111; // Delete
    case 0x07: return 99;  // PrintScreen
    case 0x08: return 119; // PauseBreak
    case 0x0a: return 102; // Home
    case 0x0b: return 107; // End
    case 0x0c: return 105; // Left
    case 0x0d: return 103; // Up
    case 0x0e: return 106; // Right
    case 0x0f: return 108; // Down
    case 0x10: return 104; // PageUp
    case 0x11: return 109; // PageDown
    case 0x19: return 58;  // CapsLock
    case 0x1a: return 69;  // NumLock
    case 0x1b: return 70;  // ScrollLock
    case 0x1c: return 59;  // F1
    case 0x1d: return 60;  // F2
    case 0x1e: return 61;  // F3
    case 0x1f: return 62;  // F4
    case 0x20: return 63;  // F5
    case 0x21: return 64;  // F6
    case 0x22: return 65;  // F7
    case 0x23: return 66;  // F8
    case 0x24: return 67;  // F9
    case 0x25: return 68;  // F10
    case 0x26: return 87;  // F11
    case 0x27: return 88;  // F12
    case 0x28: return 57;  // Space
    // 未 shift 的标点（物理键）
    case 0x2f: return 40;  // Apostrophe
    case 0x33: return 13;  // Plus -> Equal
    case 0x34: return 51;  // Comma
    case 0x35: return 12;  // Minus
    case 0x36: return 52;  // Period
    case 0x37: return 53;  // Slash
    case 0x43: return 39;  // Semicolon
    case 0x45: return 13;  // Equal
    case 0x63: return 26;  // LeftBracket
    case 0x64: return 27;  // RightBracket
    case 0x65: return 43;  // Backslash
    case 0x6c: return 41;  // Backtick
    // shift 后的标点 -> 物理键（客户端按 modifiers 还原）
    case 0x29: return 2;   // ! on 1
    case 0x2a: return 40;  // " on '
    case 0x2b: return 4;   // # on 3
    case 0x2c: return 5;   // $ on 4
    case 0x2d: return 6;   // % on 5
    case 0x2e: return 8;   // & on 7
    case 0x30: return 10;  // ( on 9
    case 0x31: return 11;  // ) on 0
    case 0x32: return 9;   // * on 8
    case 0x42: return 39;  // : on ;
    case 0x44: return 51;  // < on ,
    case 0x46: return 52;  // > on .
    case 0x47: return 53;  // ? on /
    case 0x48: return 3;   // @ on 2
    case 0x66: return 7;   // ^ on 6
    case 0x67: return 12;  // _ on -
    case 0x68: return 26;  // { on [
    case 0x69: return 27;  // } on ]
    case 0x6a: return 43;  // | on backslash
    case 0x6b: return 41;  // ~ on `
    default:
        break;
    }
    // Key_0 (0x38) .. Key_9 (0x41)
    if (key >= 0x38 && key <= 0x41) {
        static int const digit_map[10] = { 11, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        return digit_map[key - 0x38];
    }
    // Key_A (0x49) .. Key_Z (0x62) -> QWERTY 物理位置
    if (key >= 0x49 && key <= 0x62) {
        static int const letter_map[26] = {
            30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
            49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
        };
        return letter_map[key - 0x49];
    }
    return -1;
}

/* Serenity 的 MouseButton 是位标志；seat 要的是 Linux evdev 按钮码。 */
static uint32_t linux_button(GUI::MouseButton button)
{
    if (button == GUI::MouseButton::Primary)
        return 0x110; // BTN_LEFT
    if (button == GUI::MouseButton::Secondary)
        return 0x111; // BTN_RIGHT
    if (button == GUI::MouseButton::Middle)
        return 0x112; // BTN_MIDDLE
    if (button == GUI::MouseButton::Backward)
        return 0x113; // BTN_SIDE
    if (button == GUI::MouseButton::Forward)
        return 0x114; // BTN_EXTRA
    return 0x110;
}

class SurfaceWidget final : public GUI::Widget {
    C_OBJECT(SurfaceWidget)

public:
    void set_bitmap(NonnullRefPtr<Gfx::Bitmap> bitmap)
    {
        m_bitmap = move(bitmap);
        update();
    }
    Gfx::Bitmap* bitmap() const { return m_bitmap.ptr(); }

    /* 叠加层：wl_subsurface 中「铺满父窗口内容区、偏移 (0,0)」的那种（Firefox
     * 把整个窗口内容放进一个 subsurface）。Wayland 语义里子表面是父表面的一
     * 部分，输入只发给顶层表面，所以它不能单独成窗（会截走输入），必须画在
     * 父窗口的画布上。key 用子 surface 指针。 */
    void set_overlay(void* key, NonnullRefPtr<Gfx::Bitmap> bitmap, int x, int y)
    {
        m_overlays.set(key, Overlay { bitmap, x, y });
        update();
    }
    void remove_overlay(void* key)
    {
        if (m_overlays.remove(key))
            update();
    }
    void set_surface(void* surface) { m_surface = surface; }
    /* popup/subsurface 类窗口：点击时不要把键盘焦点转过去。 */
    void set_popup_surface(bool is_popup) { m_popup_surface = is_popup; }

    /* WS 原生窗口框被拖动时，把新尺寸转译给外来客户端（X11 走 XWM）。 */
    Function<void(int, int)> on_resize;
    void set_suppress_resize(bool suppress) { m_suppress_resize = suppress; }

private:
    virtual void resize_event(GUI::ResizeEvent& event) override
    {
        GUI::Widget::resize_event(event);
        if (on_resize && !m_suppress_resize)
            on_resize(width(), height());
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        /* 父 surface 可能一帧 buffer 都没有（Firefox 把内容全放在子表面里），
         * 此时也必须继续画叠加层，否则窗口一片空白。 */
        if (m_bitmap)
            painter.blit({}, *m_bitmap, m_bitmap->rect());
        for (auto& it : m_overlays) {
            auto const& overlay = it.value;
            painter.blit({ overlay.x, overlay.y }, *overlay.bitmap, overlay.bitmap->rect());
        }
    }

    /*
     * 输入映射：WindowServer 是唯一的事件源与焦点决策者（它拥有 libinput、
     * 窗口栈和激活状态），这里只把它交给 seat，让外来客户端收到标准的
     * wl_pointer / wl_keyboard 事件。坐标是 widget 局部坐标，与 surface
     * 局部坐标 1:1（WS 的复古边框不属于 widget 区域）。
     */
    virtual void enter_event(Core::Event& event) override
    {
        GUI::Widget::enter_event(event);
        if (!m_surface)
            return;
        /* Enter 事件不带坐标。**不能**在这里发 enter(0,0)：客户端会把它当成
         * 「指针在 surface 左上角」，而窗口映射时如果指针已经停在窗口内，
         * 后续不会再有 MouseMove 来纠正 —— 于是第一次点击全部打到窗口左上角
         * （实测 gedit 右上角 ☰ 菜单按钮完全点不中，客户端日志里只有
         * enter(…,0,0)+button，没有任何 motion）。
         * 正确做法（与旧桥 wayland_host.cpp 一致）：只有拿到真实坐标时才
         * enter+motion。真坐标来自 mousemove_event / mousedown_event，所以这里
         * 用最近一次已知位置补；从没收到过就跳过，等第一次移动/点击自己带坐标。 */
        if (!m_has_pointer_position)
            return;
        wlroots_proto_pointer_enter(s_proto, m_surface, m_last_pointer_position.x(), m_last_pointer_position.y());
        wlroots_proto_pointer_motion(s_proto, m_last_pointer_position.x(), m_last_pointer_position.y(), now_msec());
    }

    virtual void leave_event(Core::Event& event) override
    {
        GUI::Widget::leave_event(event);
        wlroots_proto_pointer_leave(s_proto);
    }

    virtual void mousemove_event(GUI::MouseEvent& event) override
    {
        if (!m_surface)
            return;
        auto local = to_surface_position(event);
        m_last_pointer_position = local;
        m_has_pointer_position = true;
        wlroots_proto_pointer_motion(s_proto, local.x(), local.y(), now_msec());
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (!m_surface)
            return;
        auto local = to_surface_position(event);
        m_last_pointer_position = local;
        m_has_pointer_position = true;
        /* 先确保 seat 焦点落在这个 surface 上，否则第一次点击/按键会被吞掉。
         * enter 之后补一条 motion：客户端只用 motion 更新内部指针位置。 */
        wlroots_proto_pointer_enter(s_proto, m_surface, local.x(), local.y());
        wlroots_proto_pointer_motion(s_proto, local.x(), local.y(), now_msec());
        if (!m_popup_surface)
            wlroots_proto_keyboard_enter(s_proto, m_surface);
        wlroots_proto_pointer_button(s_proto, linux_button(event.button()), true, now_msec());
    }

    virtual void keydown_event(GUI::KeyEvent& event) override
    {
        GUI::Widget::keydown_event(event);
        if (!m_surface)
            return;
        auto code = serenity_key_to_evdev(static_cast<unsigned>(event.key()));
        if (code < 0)
            return;
        wlroots_proto_keyboard_key(s_proto, static_cast<uint32_t>(code), true,
            now_msec(), event.modifiers());
    }

    virtual void keyup_event(GUI::KeyEvent& event) override
    {
        GUI::Widget::keyup_event(event);
        if (!m_surface)
            return;
        auto code = serenity_key_to_evdev(static_cast<unsigned>(event.key()));
        if (code < 0)
            return;
        wlroots_proto_keyboard_key(s_proto, static_cast<uint32_t>(code), false,
            now_msec(), event.modifiers());
    }

    virtual void mouseup_event(GUI::MouseEvent& event) override
    {
        wlroots_proto_pointer_button(s_proto, linux_button(event.button()), false, now_msec());
    }

    virtual void mousewheel_event(GUI::MouseEvent& event) override
    {
        wlroots_proto_pointer_axis(s_proto, event.wheel_delta_y(), event.wheel_delta_x(), now_msec());
    }

    /* widget 局部坐标 → 客户端 surface 坐标。
     * toplevel 的 buffer 是按 window_geometry 裁掉 CSD 阴影后才显示/进窗口的
     * （见 wlroots_proto.c 的 toplevel_committed），所以反算坐标必须把阴影
     * 偏移加回去。不加的话客户端收到的位置整体偏上/偏左一个阴影厚度（GTK 的
     * 阴影是 26x23），点击落在它自己的标题栏上 —— 实测表现为「点按钮变成拖动
     * 窗口」（客户端直接发 xdg_toplevel.resize），菜单按钮永远点不中。 */
    Gfx::IntPoint to_surface_position(GUI::MouseEvent const& event)
    {
        int gx = 0;
        int gy = 0;
        wlroots_proto_toplevel_geometry_offset(s_proto, m_surface, &gx, &gy);
        return Gfx::IntPoint { event.x() + gx, event.y() + gy };
    }

    /* 最近一次已知的指针位置（surface 坐标）。enter_event 没有坐标可用，
     * 只有这里记下的真坐标才能安全地补 enter+motion。 */
    struct Overlay {
        /* 必须是非空引用：paint 时无条件解引用。用 RefPtr 存 move 进来的
         * NonnullRefPtr 会在某些构造路径下留空，paint 直接 VERIFICATION FAILED。 */
        NonnullRefPtr<Gfx::Bitmap> bitmap;
        int x { 0 };
        int y { 0 };
    };
    HashMap<void*, Overlay> m_overlays;

    bool m_popup_surface { false };

    Gfx::IntPoint m_last_pointer_position;
    bool m_has_pointer_position { false };

    void* m_surface { nullptr };
    bool m_suppress_resize { false };
    RefPtr<Gfx::Bitmap> m_bitmap;
};

struct HostedWindow {
    RefPtr<GUI::Window> window;
    SurfaceWidget* widget { nullptr };
    RefPtr<Gfx::Bitmap> bitmap;
    int width { 0 };
    int height { 0 };
    /* 0 = 原生 Wayland toplevel；非 0 = X11 窗口（XWM 的 xid）。 */
    unsigned long xid { 0 };
    XWM::WindowInfo info;

    /* popup / subsurface 子窗口（菜单）。Serenity 侧是独立的 frameless
     * WindowType::Popup 窗口，由 WS 控制堆叠/关闭；位置由适配层推算。 */
    enum class ChildKind {
        None,
        XdgPopup,
        Subsurface,
        /* 内容型 wl_subsurface：偏移 (0,0) 且铺满父窗口内容区（Firefox 把整
         * 个窗口内容放在这样一个子表面里）。Wayland 语义中它是父表面的一部分，
         * 不是独立窗口 —— 不能建窗口（会截走输入、且鼠标永远点不进内容），
         * 而是作为叠加层画进父窗口的画布。这条 entry 没有 window/widget。 */
        ContentLayer
    } child { ChildKind::None };
    void* popup_handle { nullptr };       /* xdg_popup*（XdgPopup） */
    void* subsurface_handle { nullptr };  /* wlr_subsurface*（Subsurface） */
    void* parent_surface { nullptr };     /* 根 toplevel 的 wl_surface* */
    int lx { 0 }, ly { 0 };              /* 相对父窗口内容区左上角的偏移 */
    int pw { 0 }, ph { 0 };              /* xdg_popup 几何尺寸 */
    int anchor_x { -1 }, anchor_y { -1 };/* 上次记录的父内容区左上角（follow-parent 检测） */
    bool grabbing { false };
};

static HashMap<void*, HostedWindow> s_windows;

static HostedWindow* lookup(void* surface)
{
    auto it = s_windows.find(surface);
    return it == s_windows.end() ? nullptr : &it->value;
}

/* ── wlroots 协议回调 ──────────────────────────────────────────────────── */

/*
 * 唯一的「外来 surface → 普通 Serenity 窗口」入口。窗口框、标题栏、焦点、
 * 层叠、光标、合成全是 WindowServer 的原生能力；这里只把 surface 绑到一个
 * GUI::Window 上。原生 Wayland toplevel 和 X11 窗口走的是同一条路，差别
 * 只在窗口元数据从哪来（xdg_shell 事件 vs XWM 回调）。
 */
static HostedWindow* ensure_hosted_window(void* surface, char const* title,
    int x, int y, int w, int h, bool frameless, bool focus = true,
    GUI::WindowType window_type = GUI::WindowType::Normal, bool transparent = false,
    bool show_now = true)
{
    if (auto* existing = lookup(surface))
        return existing;

    HostedWindow hosted;
    hosted.window = GUI::Window::construct();
    /* 窗口类型必须在本函数 show() 之前设好：服务端只在 create 窗口时收到
     * m_window_type（LibGUI::Window::set_window_type 只改本地字段、不通知 WS，
     * 见 Window.cpp:405）。调用方事后设 Popup 是无效的，窗口早已按 Normal 建好，
     * 任务栏会给菜单/子表面加一个「popup」任务按钮 —— 真实客户端实测踩到过。 */
    hosted.window->set_window_type(window_type);
    hosted.window->set_title(ByteString { title ? title : "" });
    /* 有界级联偏移：多个窗口完全重叠时看起来像"只开了一个窗口"。 */
    int const slot = static_cast<int>(s_windows.size() % 6);
    int const px = x >= 0 ? x : 80 + slot * 24;
    int const py = y >= 0 ? y : 80 + slot * 24;
    hosted.window->set_rect({ px, py, w > 0 ? w : 640, h > 0 ? h : 480 });
    if (frameless)
        hosted.window->set_frameless(true);
    /* 菜单/子表面必须走「带 alpha 的窗口 + 不铺底色」：GTK 的 popover 会在
     * 内容外围画出半透明 CSD 阴影，如果窗口是不透明的、控件又先填了主题灰底，
     * 那些半透明像素就会混到灰底上 —— 视觉上就是菜单四周一圈灰边
     * （"合成器不支持阴影和透明"的假象）。开 alpha 之后阴影才会真正与桌面混合。 */
    if (transparent) {
        hosted.window->set_has_alpha_channel(true);
        hosted.window->set_frameless(true);
    }
    hosted.widget = hosted.window->set_main_widget<SurfaceWidget>().ptr();
    hosted.widget->set_fill_with_background_color(!transparent);
    hosted.widget->set_surface(surface);
    hosted.widget->set_popup_surface(window_type != GUI::WindowType::Normal);
    /* KeyEvent 只投递给有焦点的 widget；WS 激活窗口时也要重新聚焦。
     * popup 默认不抢焦点（父窗口继续收键），只有 grabbing popup 才要焦点。 */
    hosted.widget->set_focus_policy(GUI::FocusPolicy::StrongFocus);
    if (focus)
        hosted.widget->set_focus(true);

    s_windows.set(surface, move(hosted));
    auto* entry = lookup(surface);
    /* 菜单类窗口要等首帧到达、位置算好之后再 show：否则它会先按级联默认点
     * （左上角）显示一帧，再被 position_popup 挪走 —— 视觉上就是「点汉堡菜单时
     * 左边闪一下窗口边框」。 */
    if (show_now)
        entry->window->show();
    return entry;
}

static void on_toplevel_created(void* surface, char const* title, char const* app_id, void* user)
{
    (void)user;
    auto* entry = ensure_hosted_window(surface, title, -1, -1, 640, 480, false);

    /* 关闭请求交给客户端（xdg_toplevel.close），窗口等 surface 销毁时再拆。 */
    entry->window->on_close_request = [surface] {
        wlroots_proto_toplevel_close(s_proto, surface);
        return GUI::Window::CloseRequestDecision::StayOpen;
    };
    /* 激活态由 WindowServer 的 WindowManager 决定，单向同步给客户端。 */
    entry->window->on_active_window_change = [surface](bool active) {
        wlroots_proto_toplevel_set_activated(s_proto, surface, active);
        if (active) {
            wlroots_proto_keyboard_enter(s_proto, surface);
            if (auto* e = lookup(surface))
                e->widget->set_focus(true);
        } else {
            wlroots_proto_keyboard_leave(s_proto);
        }
    };
    /* 上面只是「变化」回调。窗口创建时往往已经处于激活态，那次变化早于注册，
     * 客户端就永远收不到 XDG_TOPLEVEL_STATE_ACTIVATED —— GTK 认为窗口是非激活
     * 的，于是**第一次点击只用来激活窗口**（实测 ☰ 汉堡菜单第一下没反应、第二下
     * 才弹）。这里补发一次当前状态。 */
    if (entry->window->is_active()) {
        wlroots_proto_toplevel_set_activated(s_proto, surface, true);
        wlroots_proto_keyboard_enter(s_proto, surface);
        entry->widget->set_focus(true);
    }


    if (app_id && *app_id)
        dbgln("wlroots-host: new toplevel '{}' app_id='{}'", title, app_id);
}

/* 前向声明：position_popup 定义在 on_toplevel_committed 之后（见 §popup 段）。 */
static void position_popup(HostedWindow& e);

static void on_toplevel_committed(void* surface, void const* pixels, int width, int height,
    int stride, uint32_t format, int damage_x, int damage_y, int damage_w, int damage_h, void* user)
{
    (void)user;
    (void)damage_x;
    (void)damage_y;
    (void)damage_w;
    (void)damage_h;

    auto* entry = lookup(surface);
    if (!entry)
        return;
    if (width <= 0 || height <= 0)
        return;

    /* 内容型子表面：更新父窗口的叠加层，不进入窗口路径。 */
    if (entry->child == HostedWindow::ChildKind::ContentLayer) {
        auto* parent = lookup(entry->parent_surface);
        /* 父窗口可能正在销毁（Firefox 的菜单关掉时，子层帧仍可能在路上）：
         * 此时 parent->widget 是野指针，往里 set_overlay 会写坏堆，表现为
         * 进程随后 "free(): invalid pointer"。父没了就把自己这条 entry 清掉。 */
        if (!parent || !parent->widget || !parent->window) {
            s_windows.remove(surface);
            return;
        }
        if (!entry->bitmap || entry->width != width || entry->height != height) {
            auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height });
            if (bitmap.is_error())
                return;
            entry->bitmap = bitmap.release_value();
            entry->width = width;
            entry->height = height;
        }
        copy_buffer_to_bitmap(pixels, width, height, stride, format, *entry->bitmap);
        /* 窗口尺寸跟随内容层：Firefox 按 wl_output 尺寸渲染内容（实测 1280x885），
         * 而窗口初始是 host 给的 640x480，两者不一致就会只显示左上角一块。
         * 内容层是「纯净内容、无 CSD 阴影」，跟随它不会触发阴影叠加回环。
         * 抑制 on_resize，避免又把尺寸回灌成 configure。 */
        if (parent->window && (parent->window->width() != width || parent->window->height() != height)) {
            parent->widget->set_suppress_resize(true);
            parent->window->set_rect({ parent->window->x(), parent->window->y(), width, height });
            parent->widget->set_suppress_resize(false);
        }
        parent->widget->set_overlay(surface, *entry->bitmap, entry->lx, entry->ly);
        return;
    }

    bool const resized = !entry->bitmap || entry->width != width || entry->height != height;
    if (resized) {
        auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height });
        if (bitmap.is_error())
            return;
        entry->bitmap = bitmap.release_value();
        entry->width = width;
        entry->height = height;

        /*
         * width/height 是 window_geometry（协议层已裁掉 CSD 阴影）。
         * 绝不在这里回灌 wlroots_proto_toplevel_set_size：把窗口内容尺寸
         * 再发成 configure 会让 GTK 每轮把阴影加一遍（实测每轮 +52px，
         * 窗口无限扩大）。configure 尺寸只能由 WS 的窗口管理决定。
         */
        auto window_size = entry->window->size();
        if (window_size.width() != width || window_size.height() != height) {
            /* 这是「贴合客户端 buffer」，不是用户拖框，别回灌成 configure /
             * XResizeWindow，否则 Xwayland 与 WS 会互相触发尺寸回环。 */
            entry->widget->set_suppress_resize(true);
            entry->window->set_rect({ entry->window->x(), entry->window->y(), width, height });
            entry->widget->set_suppress_resize(false);
        }
    }

    copy_buffer_to_bitmap(pixels, width, height, stride, format, *entry->bitmap);
    entry->widget->set_bitmap(*entry->bitmap);

    /* popup / subsurface 子窗口：每次提交都重新查询几何并定位（首帧放置、
     * 菜单长大后挪位都靠这里）。xdg_popup 的偏移来自 wlroots 算好的
     * toplevel 相对坐标；subsurface 的偏移在 on_subsurface_repositioned 里更新。 */
    if (entry->child != HostedWindow::ChildKind::None) {
        if (entry->child == HostedWindow::ChildKind::XdgPopup && entry->popup_handle) {
            int lx, ly, pw, ph;
            wlroots_proto_popup_geometry(s_proto, entry->popup_handle, &lx, &ly, &pw, &ph);
            entry->lx = lx;
            entry->ly = ly;
            if (pw > 0)
                entry->pw = pw;
            if (ph > 0)
                entry->ph = ph;
        }
        position_popup(*entry);
        /* 位置已经算好，现在才让窗口出现（见 ensure_hosted_window 的 show_now）。 */
        if (!entry->window->is_visible())
            entry->window->show();
    }

    /* frame 回调不在这里发：由 wlroots_proto_flush() 的 ~16ms 节拍统一
     * 批量完成（旧桥 §3.5 同款）。绑在 commit 上会让「先 frame 后 attach」
     * 的 Xwayland 永远等不到 done，首屏死锁。 */
}

static void on_toplevel_title(void* surface, char const* title, void* user)
{
    (void)user;
    if (auto* entry = lookup(surface))
        entry->window->set_title(ByteString { title });
}


/* 删除某 surface 的 entry 时，先把它名下的叠加层（ContentLayer）一并删掉，
 * 否则子 entry 会留着指向已释放父控件的 parent_surface。 */
static void drop_content_layers_of(void* parent_surface)
{
    Vector<void*> doomed;
    for (auto& it : s_windows) {
        auto& e = it.value;
        if (e.child == HostedWindow::ChildKind::ContentLayer && e.parent_surface == parent_surface)
            doomed.append(it.key);
    }
    for (auto* key : doomed)
        s_windows.remove(key);
}

static void on_toplevel_mapped(void* surface, void* user)
{
    (void)user;
    auto* entry = lookup(surface);
    if (!entry)
        return;
    entry->window->show();
    /* 这里**不要**调 wlroots_proto_toplevel_set_size：
     *  · 它内部会 schedule_configure，而该函数断言 surface->initialized，
     *    在 created 阶段调会 abort（wlr_xdg_surface.c:168）；
     *  · 即使挪到 map 之后，把 host 的初始 640x480 发下去也会让客户端按这个
     *    尺寸布局，而它自己的内容（subsurface）仍按 wl_output 尺寸渲染，
     *    两者不一致 → 窗口只有左上角一块有内容（实测）。
     * 正确做法：尺寸由客户端决定，WS 窗口跟随内容层尺寸（见 on_toplevel_committed
     * 的 ContentLayer 分支）。 */
}

static void on_toplevel_unmapped(void* surface, void* user)
{
    (void)user;
    if (auto* entry = lookup(surface))
        entry->window->hide();
}

static void on_toplevel_destroyed(void* surface, void* user)
{
    (void)user;
    drop_content_layers_of(surface);
    s_windows.remove(surface);
}

/* ── popup / subsurface 子窗口（菜单）────────────────────────────────────────
 * 两类菜单都映射成独立的 frameless WindowType::Popup 窗口，由 WindowServer 原
 * 生负责堆叠、输入与关闭；适配层只算"相对父窗口内容区的偏移"，并跟随父窗口
 * 移动。Firefox 走 xdg_popup（wlroots 已做 FLIP/SLIDE 避让），GTK/gedit 走
 * wl_subsurface（旧桥 wayland_host.cpp:901 注释）。
 */

/* 把子窗口定位到「父窗口内容区左上角 + (lx,ly)」。父内容区用 widget 的
 * screen_relative_rect()，自动扣掉复古窗口框的偏移。 */
static void position_popup(HostedWindow& e)
{
    /* ContentLayer 是没有窗口的 entry（内容画在父窗口画布上）；pump 的
     * 「跟随父窗口移动」循环会扫到它，这里必须挡掉，否则解引用空 window。 */
    if (e.child == HostedWindow::ChildKind::None || e.child == HostedWindow::ChildKind::ContentLayer
        || !e.parent_surface || !e.window)
        return;
    auto* parent = lookup(e.parent_surface);
    if (!parent || !parent->widget)
        return;
    auto const& pr = parent->widget->screen_relative_rect();
    int w = e.width > 0 ? e.width : (e.pw > 0 ? e.pw : e.window->width());
    int h = e.height > 0 ? e.height : (e.ph > 0 ? e.ph : e.window->height());
    Gfx::IntRect target { pr.x() + e.lx, pr.y() + e.ly, w, h };
    if (e.window->rect() != target)
        e.window->set_rect(target);
    e.anchor_x = pr.x();
    e.anchor_y = pr.y();

    /* Position changes only, so this does not flood the log on every frame. */
    static Gfx::IntRect last_logged_target { -1, -1, -1, -1 };
    if (target != last_logged_target) {
        outln("wlroots-host: popup pos parent={},{} lx={} ly={} -> {}+{}+{}x{}",
            pr.x(), pr.y(), e.lx, e.ly, target.x(), target.y(), target.width(), target.height());
        last_logged_target = target;
    }
}

static void on_popup_created(void* popup, void* surface, void* toplevel_surface, void* user)
{
    (void)user;
    int lx, ly, pw, ph;
    wlroots_proto_popup_geometry(s_proto, popup, &lx, &ly, &pw, &ph);
    bool grabbing = wlroots_proto_popup_is_grabbing(s_proto, popup) != 0;

    auto* entry = ensure_hosted_window(surface, "popup", -1, -1, 1, 1, /*frameless*/ true, grabbing,
        GUI::WindowType::Popup, /*transparent*/ true, /*show_now*/ false);
    entry->child = HostedWindow::ChildKind::XdgPopup;
    entry->popup_handle = popup;
    entry->parent_surface = toplevel_surface;
    entry->grabbing = grabbing;
    entry->lx = lx;
    entry->ly = ly;
    entry->pw = pw;
    entry->ph = ph;
    entry->widget->set_focus_policy(grabbing ? GUI::FocusPolicy::StrongFocus : GUI::FocusPolicy::NoFocus);

    if (grabbing) {
        /* grabbing popup（gedit）取键盘焦点做 ↑↓ 导航；非 grabbing（Firefox
         * XUL 菜单不调 grab）保持 NoFocus，键继续走父窗口。 */
        entry->window->on_active_window_change = [surface](bool active) {
            if (active) {
                wlroots_proto_keyboard_enter(s_proto, surface);
            } else {
                wlroots_proto_keyboard_leave(s_proto);
            }
        };
    }

    /* 用父窗口内容区精确框再做一次约束避让，触发 reposition → 重定位。 */
    if (auto* parent = lookup(toplevel_surface)) {
        auto const& r = parent->widget->screen_relative_rect();
        wlroots_proto_popup_unconstrain(s_proto, popup, -r.x(), -r.y(),
            GUI::Desktop::the().rect().width(), GUI::Desktop::the().rect().height());
        position_popup(*entry);
    }
    /* 不要在这里 show：此刻 popup 的 geometry 往往还是 (0,0)，窗口会先在父窗口
     * 左上角闪一下，再由首帧的 position_popup 挪到正确位置。等首帧到达、位置
     * 算好之后再显示（见 on_toplevel_committed 的 child 分支）。 */
    outln("wlroots-host: new xdg_popup surf={:#x} grab={}", (uintptr_t)surface, grabbing ? "yes" : "no");
}

static void on_popup_destroyed(void* popup, void* user)
{
    (void)user;
    /* AK::HashMap::remove(Iterator&) 会令迭代器失效且返回 void，
     * 不能边遍历边 erase。先收集 key，再统一删除。 */
    Vector<void*> to_remove;
    for (auto& it : s_windows) {
        if (it.value.child == HostedWindow::ChildKind::XdgPopup && it.value.popup_handle == popup)
            to_remove.append(it.key);
    }
    for (auto* key : to_remove) {
        drop_content_layers_of(key);
        s_windows.remove(key);
    }
}

static void on_popup_repositioned(void* popup, void* user)
{
    (void)user;
    for (auto& it : s_windows) {
        auto& e = it.value;
        if (e.child != HostedWindow::ChildKind::XdgPopup || e.popup_handle != popup)
            continue;
        int lx, ly, pw, ph;
        wlroots_proto_popup_geometry(s_proto, popup, &lx, &ly, &pw, &ph);
        e.lx = lx;
        e.ly = ly;
        if (pw > 0)
            e.pw = pw;
        if (ph > 0)
            e.ph = ph;
        position_popup(e);
        break;
    }
}

/* xdg_popup.configure has just been sent: wlroots has finished FLIP/SLIDE and
 * popup->scheduled.geometry now holds the real position. GTK acks a pure
 * position change without attaching a new buffer, so no commit will follow and
 * on_toplevel_committed() would never reposition the window. Do it here. */
static void on_popup_configured(void* popup, void* user)
{
    (void)user;
    for (auto& it : s_windows) {
        auto& e = it.value;
        if (e.child != HostedWindow::ChildKind::XdgPopup || e.popup_handle != popup)
            continue;
        int lx, ly, pw, ph;
        wlroots_proto_popup_geometry(s_proto, popup, &lx, &ly, &pw, &ph);
        e.lx = lx;
        e.ly = ly;
        if (pw > 0)
            e.pw = pw;
        if (ph > 0)
            e.ph = ph;
        position_popup(e);
        if (e.window && !e.window->is_visible())
            e.window->show();
        break;
    }
}

static void on_subsurface_created(void* subsurface, void* surface, void* parent_surface,
    int sub_x, int sub_y, void* user)
{
    (void)user;

    /* 贴在父窗口内容区原点、铺满内容区的子表面 = 内容层（Firefox 的窗口内容），
     * 不建窗口，改为叠加到父窗口画布上（输入仍然只发给父 toplevel）。
     * 注意 sub_x/sub_y 是相对父 **buffer**（含 CSD 阴影）的坐标，必须减掉
     * window_geometry 偏移才是「相对内容区」——Firefox 实测给的是 (26,23)。 */
    int parent_gx = 0, parent_gy = 0;
    wlroots_proto_toplevel_geometry_offset(s_proto, parent_surface, &parent_gx, &parent_gy);
    if (sub_x - parent_gx == 0 && sub_y - parent_gy == 0) {
        HostedWindow hosted;
        hosted.child = HostedWindow::ChildKind::ContentLayer;
        hosted.subsurface_handle = subsurface;
        hosted.parent_surface = parent_surface;
        hosted.lx = 0;
        hosted.ly = 0;
        s_windows.set(surface, move(hosted));
        outln("wlroots-host: subsurface {:#x} is a content layer of {:#x} (composited, no window)",
            (uintptr_t)surface, (uintptr_t)parent_surface);
        return;
    }

    auto* entry = ensure_hosted_window(surface, "popup", -1, -1, 1, 1, /*frameless*/ true, /*focus*/ false,
        GUI::WindowType::Popup, /*transparent*/ true, /*show_now*/ false);
    entry->child = HostedWindow::ChildKind::Subsurface;
    entry->subsurface_handle = subsurface;
    entry->parent_surface = parent_surface;
    entry->widget->set_focus_policy(GUI::FocusPolicy::NoFocus);
    /* sub_x/sub_y 相对父 surface buffer（含 CSD）；减掉 geometry 偏移落到内容区。 */
    int gx = 0, gy = 0;
    wlroots_proto_toplevel_geometry_offset(s_proto, parent_surface, &gx, &gy);
    entry->lx = sub_x - gx;
    entry->ly = sub_y - gy;
    position_popup(*entry);
    outln("wlroots-host: new subsurface popup surf={:#x} parent={:#x} at {},{}",
        (uintptr_t)surface, (uintptr_t)parent_surface, sub_x, sub_y);
}

static void on_subsurface_repositioned(void* subsurface, void* user)
{
    (void)user;
    auto* entry = [&]() -> HostedWindow* {
        for (auto& it : s_windows) {
            if ((it.value.child == HostedWindow::ChildKind::Subsurface
                    || it.value.child == HostedWindow::ChildKind::ContentLayer)
                && it.value.subsurface_handle == subsurface)
                return &it.value;
        }
        return nullptr;
    }();
    if (!entry)
        return;
    int sx = 0, sy = 0;
    wlroots_proto_subsurface_offset(s_proto, subsurface, &sx, &sy);
    int gx = 0, gy = 0;
    wlroots_proto_toplevel_geometry_offset(s_proto, entry->parent_surface, &gx, &gy);
    entry->lx = sx - gx;
    entry->ly = sy - gy;
    if (entry->child == HostedWindow::ChildKind::ContentLayer) {
        /* 内容层位置变了：把叠加层按新偏移重放一次。 */
        if (entry->bitmap) {
            if (auto* parent = lookup(entry->parent_surface); parent && parent->widget) {
                for (auto& it : s_windows) {
                    if (it.value.subsurface_handle == subsurface && it.value.parent_surface == entry->parent_surface) {
                        parent->widget->set_overlay(it.key, *entry->bitmap, entry->lx, entry->ly);
                        break;
                    }
                }
            }
        }
        return;
    }
    position_popup(*entry);
}

static void on_subsurface_destroyed(void* subsurface, void* user)
{
    (void)user;
    Vector<void*> to_remove;
    for (auto& it : s_windows) {
        auto& e = it.value;
        if ((e.child == HostedWindow::ChildKind::Subsurface
                || e.child == HostedWindow::ChildKind::ContentLayer)
            && e.subsurface_handle == subsurface) {
            /* 内容层没有窗口，只需要把叠加层从父画布上摘掉。 */
            if (e.child == HostedWindow::ChildKind::ContentLayer) {
                if (auto* parent = lookup(e.parent_surface); parent && parent->widget)
                    parent->widget->remove_overlay(it.key);
            }
            to_remove.append(it.key);
        }
    }
    for (auto* key : to_remove) {
        drop_content_layers_of(key);
        s_windows.remove(key);
    }
}

/* ── Xwayland 桥接（复用旧桥 XWM + XwaylandBridge，只做 WS 转译） ──────────
 *
 * X11/Wine 客户端 → Xwayland（我们 spawn 的 Wayland 客户端）→ 裸 wl_surface
 *   → 和原生 toplevel 同一条 shm → Gfx::Bitmap → GUI::Window → WS 合成。
 *
 * X 侧窗口管理（WM_Sn、Composite redirect、MapNotify、WL_SURFACE_ID
 * ClientMessage、焦点/关闭/尺寸协议）全部是旧 WaylandHost 的 xwm.cpp 与
 * xwayland_bridge.cpp，直接复用编译单元，这里一行都没重写。新代码只有
 * 「WindowInfo → GUI::Window」这一段翻译：窗口框、标题栏、层叠、焦点、
 * 光标、合成一律交给 WindowServer 的原生能力。
 */
struct SurfaceEntry {
    void* surface;
    uint32_t id;
    int32_t pid;
};

static Vector<SurfaceEntry> s_surfaces;
static Vector<XWM::WindowInfo> s_pending_x11;
static XwaylandBridge* s_xwayland { nullptr };

static int32_t xwayland_pid()
{
    return s_xwayland && s_xwayland->pid() > 0 ? static_cast<int32_t>(s_xwayland->pid()) : -1;
}

/* Wayland object id 是 per-client 的：Xwayland 与 GTK 客户端会撞号，所以
 * 优先按 Xwayland 的 pid 精确匹配；external 模式（pid 未知）才退化按 id。 */
static void* find_x11_surface(uint32_t id, int32_t pid)
{
    if (pid >= 0) {
        for (auto const& entry : s_surfaces) {
            if (entry.id == id && entry.pid == pid)
                return entry.surface;
        }
    }
    for (auto const& entry : s_surfaces) {
        if (entry.id == id)
            return entry.surface;
    }
    return nullptr;
}

static void pair_x11_surface(void* surface, XWM::WindowInfo const& info)
{
    /* 给裸 surface 挂帧转发；若它已有 xdg toplevel role 就拒绝错配。 */
    if (!wlroots_proto_watch_surface(s_proto, surface))
        return;

    bool const popup = info.override_redirect || info.is_popup;
    int x = -1;
    int y = -1;
    if (popup && info.transient_for) {
        /* Xwayland 的 X 屏幕坐标和 WS 的窗口栈是两套坐标系；popup 相对父
         * toplevel 平移，否则菜单会飘到随机位置（旧桥同款处理）。 */
        for (auto const& it : s_windows) {
            auto const& parent = it.value;
            if (parent.xid == info.transient_for && parent.info.xid != 0) {
                x = parent.window->x() + (info.x - parent.info.x);
                y = parent.window->y() + (info.y - parent.info.y);
                break;
            }
        }
    }

    auto* entry = ensure_hosted_window(surface, info.title.c_str(), x, y,
        info.width, info.height, popup,
        /*focus*/ true, popup ? GUI::WindowType::Popup : GUI::WindowType::Normal,
        /*transparent*/ popup);
    entry->xid = info.xid;
    entry->info = info;
    entry->window->set_title(ByteString { info.title.c_str(), info.title.size() });
    if (popup)
        entry->widget->set_focus_policy(GUI::FocusPolicy::NoFocus);

    /* 关闭 / 尺寸 / 焦点：WS 的原生窗口行为单向转译给 X11 侧。 */
    unsigned long const xid = info.xid;
    entry->window->on_close_request = [xid] {
        XWM::close_window(xid);
        return GUI::Window::CloseRequestDecision::StayOpen;
    };
    entry->widget->on_resize = [xid](int w, int h) {
        if (w > 0 && h > 0)
            XWM::resize_window(xid, w, h);
    };
    if (!popup) {
        auto* widget = entry->widget;
        entry->window->on_active_window_change = [surface, widget, xid](bool active) {
            if (active) {
                widget->set_focus(true);
                XWM::focus_window(xid);
                wlroots_proto_keyboard_enter(s_proto, surface);
            } else {
                wlroots_proto_keyboard_leave(s_proto);
            }
        };
    }

    /* 补一次 watch 之前就可能已经提交的帧，避免 X11 首屏空白。 */
    wlroots_proto_surface_repaint(s_proto, surface);

    outln("xwm: paired X 0x{:x} <-> wl_surface #{} '{}' {}x{}{}", xid,
        info.wl_surface_id, info.title.c_str(), info.width, info.height,
        popup ? " (popup)" : "");
}

static void on_surface_created(void*, void* surface, uint32_t id, int32_t pid)
{
    s_surfaces.append({ surface, id, pid });
    /* MapNotify 可能早于 Xwayland 的 create_surface 请求，配对延后到这里。 */
    int32_t const xpid = xwayland_pid();
    if (pid >= 0 && xpid >= 0 && pid != xpid)
        return;
    for (size_t i = 0; i < s_pending_x11.size();) {
        if (s_pending_x11[i].wl_surface_id == id) {
            auto info = s_pending_x11[i];
            s_pending_x11.remove(i);
            pair_x11_surface(surface, info);
        } else {
            ++i;
        }
    }
}

static void on_surface_destroyed(void*, void* surface)
{
    s_surfaces.remove_first_matching([&](auto const& entry) {
        return entry.surface == surface;
    });
}

static void setup_x11_host()
{
    XWM::on_window_mapped = [](XWM::WindowInfo const& info) {
        if (info.wl_surface_id == 0) {
            /* Xwayland 还没把 surface id 挂到窗口上；PropertyNotify 会再报。 */
            return;
        }
        if (auto* surface = find_x11_surface(static_cast<uint32_t>(info.wl_surface_id), xwayland_pid())) {
            pair_x11_surface(surface, info);
            return;
        }
        s_pending_x11.remove_first_matching([&](auto const& pending) {
            return pending.xid == info.xid;
        });
        s_pending_x11.append(info);
    };

    XWM::on_title_changed = [](unsigned long xid, std::string const& title) {
        for (auto& it : s_windows) {
            if (it.value.xid == xid) {
                it.value.window->set_title(ByteString { title.c_str(), title.size() });
                break;
            }
        }
    };

    XWM::on_window_destroyed = [](unsigned long xid) {
        void* doomed = nullptr;
        for (auto& it : s_windows) {
            if (it.value.xid == xid) {
                doomed = it.key;
                break;
            }
        }
        if (doomed)
            s_windows.remove(doomed);
        s_pending_x11.remove_first_matching([&](auto const& pending) {
            return pending.xid == xid;
        });
    };

    ByteString xwm_display { ":100" };
    if (auto* display_name = getenv("SERENITY_XWAYLAND_DISPLAY"); display_name && *display_name)
        xwm_display = ByteString { display_name };
    bool const external = getenv("SERENITY_XWAYLAND_EXTERNAL") != nullptr;

    static XwaylandBridge bridge;
    s_xwayland = &bridge;
    /* 传实际 socket 名：wl_display_add_socket_auto 可能改过名字。 */
    bridge.start(xwm_display, ByteString { wlroots_proto_socket(s_proto) }, external);
}

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    /* 旧桥踩过的坑（STATUS_XWM_SEGV.md §5）：stdout 重定向到 session log 时
     * 是块缓冲，配对日志会长时间滞留，看起来像"配对没发生"。设成行缓冲。 */
    setvbuf(stdout, nullptr, _IOLBF, 0);

    auto app = TRY(GUI::Application::create(arguments));
    app->set_quit_when_last_window_deleted(false);

    auto const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    auto const* socket_name = getenv("SERENITY_WL_SOCKET");
    if (!socket_name || !*socket_name)
        socket_name = "wayland-serenity-0";
    if (runtime_dir) {
        auto path = ByteString::formatted("{}/{}", runtime_dir, socket_name);
        unlink(path.characters());
    }

    wlroots_proto_callbacks callbacks {};
    callbacks.user = nullptr;
    callbacks.toplevel_created = [](void* user, void* surface, char const* title, char const* app_id) {
        on_toplevel_created(surface, title, app_id, user);
    };
    callbacks.toplevel_committed = [](void* user, void* surface, void const* pixels,
                                      int width, int height, int stride, uint32_t format,
                                      int dx, int dy, int dw, int dh) {
        on_toplevel_committed(surface, pixels, width, height, stride, format, dx, dy, dw, dh, user);
    };
    callbacks.toplevel_title_changed = [](void* user, void* surface, char const* title) {
        on_toplevel_title(surface, title, user);
    };
    callbacks.toplevel_mapped = [](void* user, void* surface) { on_toplevel_mapped(surface, user); };
    callbacks.toplevel_unmapped = [](void* user, void* surface) { on_toplevel_unmapped(surface, user); };
    callbacks.toplevel_destroyed = [](void* user, void* surface) { on_toplevel_destroyed(surface, user); };
    callbacks.surface_created = [](void*, void* surface, uint32_t id, int32_t pid) {
        on_surface_created(nullptr, surface, id, pid);
    };
    callbacks.surface_destroyed = [](void*, void* surface) {
        on_surface_destroyed(nullptr, surface);
    };
    callbacks.popup_created = [](void* user, void* popup, void* surface, void* toplevel_surface) {
        on_popup_created(popup, surface, toplevel_surface, user);
    };
    callbacks.popup_destroyed = [](void* user, void* popup) {
        on_popup_destroyed(popup, user);
    };
    callbacks.popup_repositioned = [](void* user, void* popup) {
        on_popup_repositioned(popup, user);
    };
    callbacks.popup_configured = [](void* user, void* popup) {
        on_popup_configured(popup, user);
    };
    callbacks.subsurface_created = [](void* user, void* subsurface, void* surface, void* parent_surface,
        int sub_x, int sub_y) {
        on_subsurface_created(subsurface, surface, parent_surface, sub_x, sub_y, user);
    };
    callbacks.subsurface_repositioned = [](void* user, void* subsurface) {
        on_subsurface_repositioned(subsurface, user);
    };
    callbacks.subsurface_destroyed = [](void* user, void* subsurface) {
        on_subsurface_destroyed(subsurface, user);
    };

    /* wl_output 只上报几何，尺寸取自 WS 的实际屏幕，避免 GDK 用错 workarea。 */
    auto screen = GUI::Desktop::the().rect();
    int output_width = screen.width() > 0 ? screen.width() : 1920;
    int output_height = screen.height() > 0 ? screen.height() : 1080;
    s_proto = wlroots_proto_create(socket_name, output_width, output_height, &callbacks);
    if (!s_proto) {
        warnln("wlroots-host: protocol server failed to start");
        return 1;
    }

    /* X11/Wine 兼容：复用旧桥的 XwaylandBridge（spawn + displayfd 就绪）与
     * XWM（X 侧窗口管理），把 X 窗口转译成普通 GUI::Window。 */
    setup_x11_host();

    int fd = wlroots_proto_event_fd(s_proto);
    auto notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    notifier->on_activation = [] {
        wlroots_proto_dispatch(s_proto, 0);
    };

    /* Xwayland 启动时要对我们的 socket 做阻塞 roundtrip，所以就绪探测之前
     * 必须先 dispatch Wayland loop，否则两边互等死锁。连上后 poll() 幂等，
     * 顺带 reap 子进程。 */
    auto x11_ready = Core::Timer::create_repeating(200, [] {
        wlroots_proto_dispatch(s_proto, 0);
        if (s_xwayland)
            s_xwayland->poll();
    });
    x11_ready->start();

    // frame 回调、X 事件与客户端重绘共用一个 8ms 节拍，和 wayland_host 一致。
    auto pump = Core::Timer::create_repeating(8, [] {
        wlroots_proto_dispatch(s_proto, 0);
        XWM::pump();
        wlroots_proto_flush(s_proto);

        /* popup 跟随父窗口移动：父内容区左上角变了就重新定位子窗口。 */
        for (auto& it : s_windows) {
            auto& e = it.value;
            if (e.child == HostedWindow::ChildKind::None || !e.parent_surface)
                continue;
            auto* parent = lookup(e.parent_surface);
            if (!parent || !parent->widget)
                continue;
            auto const& pr = parent->widget->screen_relative_rect();
            if (pr.x() != e.anchor_x || pr.y() != e.anchor_y)
                position_popup(e);
        }

        /* 点桌面/任务栏（非 Wayland 窗口）时关闭 grabbing popup（gedit 这类
         * 调过 xdg_popup.grab 的菜单）。这是旧桥已知未完善项（STATUS/WAYLAND_
         * ROADMAP §3.8）：wlroots 的 grab 只在点击落在某个 Wayland surface 时
         * 触发 dismiss，点击原生 Serenity 窗口（桌面/任务栏）收不到，故这里补。 */
        auto* active = GUI::Application::the()->active_window();
        bool active_is_wayland = false;
        if (active) {
            for (auto& it : s_windows) {
                if (it.value.window == active) {
                    active_is_wayland = true;
                    break;
                }
            }
        }
        if (active && !active_is_wayland) {
            Vector<void*> to_dismiss;
            for (auto& it : s_windows) {
                auto& e = it.value;
                if (e.child == HostedWindow::ChildKind::XdgPopup && e.grabbing)
                    to_dismiss.append(e.popup_handle);
            }
            for (auto* popup : to_dismiss)
                wlroots_proto_popup_dismiss(s_proto, popup);
        }
    });
    pump->start();

    outln("wlroots-host ready: WAYLAND_DISPLAY={} DISPLAY={} (no renderer, no output)",
        wlroots_proto_socket(s_proto), s_xwayland->display().characters());

    return app->exec();
}
