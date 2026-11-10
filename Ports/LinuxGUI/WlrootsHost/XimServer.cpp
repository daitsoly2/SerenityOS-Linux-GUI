/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * XIM 服务端 —— X11 客户端的输入法入口。
 *
 * 协议层用 XCBImdkit（fcitx5 的 xim addon 用的同一个库，走 XCB 原生事件），
 * 不用 Xlib 版 IMdkit：后者依赖 libX11 私有 _XRegisterFilterByType，在较新的
 * libX11 上不再可靠地吞掉 _XIM_XCONNECT，客户端会卡死在 XOpenIM。
 *
 * 桥只做三件事：
 *   1) XIM_FORWARD_EVENT → keysym/states/release → ImeService::handle_key()
 *      （→ serenity-ime 服务 → 裁剪版 fcitx5(rime)）；
 *   2) 引擎 commit → xcb_im_commit_string 回给 X11 客户端；
 *   3) 候选窗/预编辑由 ImeService 的 Serenity UI 统一画，不走 XIM callbacks。
 */
#include "XimServer.h"
#include "ImeService.h"

#include <AK/ByteString.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>
#include <LibGfx/Rect.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/xcb.h>
#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imdkit.h>
#include <xcb-imdkit/ximproto.h>

namespace XimServer {

namespace {

xcb_connection_t* s_conn { nullptr };
xcb_window_t s_window { 0 };
xcb_im_t* s_im { nullptr };
Display* s_keysym_display { nullptr };   /* 只用来把 keycode 翻成 keysym */
RefPtr<Core::Notifier> s_notifier;
RefPtr<Core::Timer> s_pump_timer;
bool s_focused { false };
bool s_ws_x11_focus { false };
xcb_im_input_context_t* s_ic { nullptr };
/* 仅在 xcb_im_filter_event() 的回调栈内有效。引擎 commit 是在
 * FORWARD_EVENT 回调里同步产生的，用这个指针回灌比用全局 s_ic 安全：
 * 客户端异常断开时 imdkit 可能已经把 s_ic 指向的 IC 释放掉。 */
xcb_im_input_context_t* s_callback_ic { nullptr };
CaretResolver s_caret_resolver;

uint32_t s_styles[] = {
    XCB_IM_PreeditNothing | XCB_IM_StatusNothing,
    XCB_IM_PreeditPosition | XCB_IM_StatusNothing,
    XCB_IM_PreeditCallbacks | XCB_IM_StatusNothing,
    XCB_IM_PreeditNothing | XCB_IM_StatusCallbacks,
    XCB_IM_PreeditPosition | XCB_IM_StatusCallbacks,
    XCB_IM_PreeditCallbacks | XCB_IM_StatusCallbacks,
};
char s_compound_text[] = "COMPOUND_TEXT";
char s_utf8_string[] = "UTF8_STRING";
char* s_encodings[] = { s_compound_text, s_utf8_string };

char const* display_name()
{
    if (auto const* name = getenv("DISPLAY"); name && *name)
        return name;
    if (auto const* name = getenv("SERENITY_APP_DISPLAY"); name && *name)
        return name;
    return ":100";
}

Gfx::IntRect caret_screen_rect(xcb_im_input_context_t* ic)
{
    if (!ic || !s_conn)
        return {};
    auto* attr = xcb_im_input_context_get_preedit_attr(ic);
    if (!attr)
        return {};

    /* XNSpotLocation 是**相对 client/focus window** 的坐标，直接当屏幕坐标用
     * 会让候选窗飘到屏幕左上角。必须换算到 root（屏幕）坐标系。 */
    xcb_window_t window = xcb_im_input_context_get_focus_window(ic);
    if (!window)
        window = xcb_im_input_context_get_client_window(ic);

    int x = attr->spot_location.x;
    int y = attr->spot_location.y;
    if (window) {
        const xcb_setup_t* setup = xcb_get_setup(s_conn);
        xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
        if (iter.data) {
            auto cookie = xcb_translate_coordinates(s_conn, window, iter.data->root,
                attr->spot_location.x, attr->spot_location.y);
            if (auto* reply = xcb_translate_coordinates_reply(s_conn, cookie, nullptr)) {
                x = reply->dst_x;
                y = reply->dst_y;
                free(reply);
            }
        }
    }
    /* 行高：客户端报的 XNArea / XNLineSpace 才是真正的行盒高度。
     * 硬编码 20 在 WordCraft 这种行高 ~30 的客户端上会让 +22 落在字形里，
     * 候选窗就「压在文字上」。 */
    int line_height = attr->area.height > 0 ? attr->area.height : (int)attr->line_space;
    if (line_height <= 0)
        line_height = 20;
    /* X root 坐标 ≠ WS 屏幕坐标：同一个 X 窗口在 WS 上被摆到了别处
     * （级联位置 + 标题栏），直接用 root 坐标会让候选窗整体偏出光标。 */
    if (s_caret_resolver) {
        auto rect = s_caret_resolver(static_cast<unsigned long>(window), x, y);
        rect.set_height(line_height);
        fprintf(stderr, "xim: caret root=%d,%d -> screen=%d,%d h=%d\n",
            x, y, rect.x(), rect.y(), line_height);
        return rect;
    }
    return { x, y, 0, line_height };
}

/* 把 UTF-8 上屏文本转成 COMPOUND_TEXT 后交给 XIM 客户端。 */
void commit_utf8(char const* text)
{
    // 只认当前回调里的 IC：全局 s_ic 在客户端断开后可能是悬垂指针。
    if (!s_im || !s_callback_ic || !text)
        return;
    size_t length = 0;
    char* compound = xcb_utf8_to_compound_text(text, strlen(text), &length);
    if (!compound) {
        /* 转换失败时直接用 ISO10646-1 转义 + UTF-8（ibus/fcitx5 同款兜底）。 */
        auto fallback = ByteString::formatted("\033%G{}", text);
        xcb_im_commit_string(s_im, s_callback_ic, XCB_XIM_LOOKUP_CHARS,
            fallback.characters(), fallback.length(), 0);
        return;
    }
    xcb_im_commit_string(s_im, s_callback_ic, XCB_XIM_LOOKUP_CHARS, compound, length, 0);
    free(compound);
    fprintf(stderr, "xim: commit '%s'\n", text);
}

/* XCBImdkit 的协议回调。arg 对 FORWARD_EVENT 是 xcb_key_press_event_t*。 */
void protocol_callback(xcb_im_t*, xcb_im_client_t*, xcb_im_input_context_t* ic,
    const xcb_im_packet_header_fr_t* hdr, void*, void* arg, void*)
{
    if (!hdr)
        return;

    switch (hdr->major_opcode) {
    case XCB_XIM_CREATE_IC:
        s_ic = ic;
        fprintf(stderr, "xim: CREATE_IC\n");
        break;
    case XCB_XIM_SET_IC_VALUES:
        s_ic = ic;
        if (s_focused)
            ImeService::update_caret(caret_screen_rect(ic));
        break;
    case XCB_XIM_SET_IC_FOCUS:
        s_ic = ic;
        s_focused = true;
        fprintf(stderr, "xim: SET_IC_FOCUS\n");
        ImeService::focus_in(caret_screen_rect(ic));
        break;
    case XCB_XIM_UNSET_IC_FOCUS:
        s_focused = false;
        ImeService::focus_out();
        break;
    case XCB_XIM_RESET_IC:
        ImeService::reset();
        break;
    case XCB_XIM_DESTROY_IC:
        if (s_ic == ic)
            s_ic = nullptr;
        s_focused = false;
        ImeService::focus_out();
        break;
    case XCB_XIM_DISCONNECT:
        s_ic = nullptr;
        s_focused = false;
        ImeService::focus_out();
        break;

    case XCB_XIM_FORWARD_EVENT: {
        auto* event = static_cast<xcb_key_press_event_t*>(arg);
        if (!event)
            break;
        bool release = (event->response_type & ~0x80) == XCB_KEY_RELEASE;
        /* XCB detail 就是 X11 keycode；借用 Xlib 连接的 keymap 翻 keysym。 */
        KeySym keysym = s_keysym_display
            ? XKeycodeToKeysym(s_keysym_display, event->detail, 0)
            : NoSymbol;
        unsigned states = event->state & 0xff;

        /* 引擎在 handle_key() 里同步回 commit，那一下必须用本次回调的 IC。 */
        s_callback_ic = ic;
        bool handled = keysym != NoSymbol
            ? ImeService::handle_key(static_cast<unsigned>(keysym), states, release)
            : false;
        s_callback_ic = nullptr;
        if (handled) {
            fprintf(stderr, "xim: key %lu consumed by engine\n",
                static_cast<unsigned long>(keysym));
            break; /* 吞掉，客户端不看到原始键 */
        }
        /* 引擎不消费 → 原样还给客户端（英文、Ctrl+C、方向键）。 */
        xcb_im_forward_event(s_im, ic, event);
        break;
    }
    default:
        break;
    }
}

}

bool initialize()
{
    if (s_im)
        return true;
    if (getenv("XIM_SERVER") && !strcmp(getenv("XIM_SERVER"), "0"))
        return false;

    int screen_number = 0;
    s_conn = xcb_connect(nullptr, &screen_number);
    if (!s_conn || xcb_connection_has_error(s_conn)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            fprintf(stderr, "xim: xcb_connect not ready yet\n");
        }
        if (s_conn)
            xcb_disconnect(s_conn);
        s_conn = nullptr;
        return false;
    }

    const xcb_setup_t* setup = xcb_get_setup(s_conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t* screen = iter.data;
    if (!screen)
        return false;

    s_window = xcb_generate_id(s_conn);
    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[] = { 1 };
    xcb_create_window(s_conn, XCB_COPY_FROM_PARENT, s_window, screen->root,
        0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        mask, values);

    xcb_im_styles_t styles {};
    styles.nStyles = sizeof(s_styles) / sizeof(s_styles[0]);
    styles.styles = s_styles;
    xcb_im_encodings_t encodings {};
    encodings.nEncodings = 2;
    encodings.encodings = s_encodings;

    s_im = xcb_im_create(s_conn, screen_number, s_window, "serenity",
        XCB_IM_ALL_LOCALES, &styles, nullptr, nullptr, &encodings,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE,
        protocol_callback, nullptr);
    if (!s_im || !xcb_im_open_im(s_im)) {
        fprintf(stderr, "xim: xcb_im_open_im failed\n");
        if (s_im)
            xcb_im_destroy(s_im);
        s_im = nullptr;
        xcb_disconnect(s_conn);
        s_conn = nullptr;
        return false;
    }

    /* keysym 换算只需要 keymap，不用这个 Display 收事件。 */
    s_keysym_display = XOpenDisplay(display_name());

    s_notifier = Core::Notifier::construct(xcb_get_file_descriptor(s_conn),
        Core::Notifier::Type::Read);
    s_notifier->on_activation = [] {
        pump();
    };
    /* XCB 事件可能已被读进缓冲，fd 不再可读；按拍轮询兜底。 */
    s_pump_timer = Core::Timer::create_repeating(20, [] {
        pump();
    });
    s_pump_timer->start();

    xcb_flush(s_conn);
    fprintf(stderr, "xim: serving @server=serenity via XCBImdkit\n");
    return true;
}

void pump()
{
    if (!s_conn || !s_im)
        return;
    while (auto* event = xcb_poll_for_event(s_conn)) {
        xcb_im_filter_event(s_im, event);
        free(event);
    }
    xcb_flush(s_conn);
}

void shutdown()
{
    if (!s_conn)
        return;
    if (s_notifier)
        s_notifier->set_enabled(false);
    s_notifier = nullptr;
    if (s_pump_timer)
        s_pump_timer->stop();
    s_pump_timer = nullptr;
    if (s_im) {
        xcb_im_close_im(s_im);
        xcb_im_destroy(s_im);
        s_im = nullptr;
    }
    if (s_keysym_display) {
        XCloseDisplay(s_keysym_display);
        s_keysym_display = nullptr;
    }
    xcb_disconnect(s_conn);
    s_conn = nullptr;
}

bool is_active()
{
    return s_focused && s_ws_x11_focus;
}

void set_ws_x11_focus(bool x11)
{
    if (s_ws_x11_focus == x11)
        return;
    s_ws_x11_focus = x11;
    fprintf(stderr, "xim: ws keyboard focus -> %s\n", x11 ? "X11" : "Wayland");
}

void commit(char const* utf8_text)
{
    commit_utf8(utf8_text);
}

void set_caret_resolver(CaretResolver resolver)
{
    s_caret_resolver = move(resolver);
}

}
