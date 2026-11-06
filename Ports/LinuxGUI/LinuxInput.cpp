/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ports/LinuxGUI/LinuxInput.h>
#include <Ports/LinuxGUI/Common/keymap.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <libinput.h>
#include <libudev.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

namespace WindowServer::LinuxInput {

namespace {

struct State {
    libinput* input { nullptr };
    udev* udev_context { nullptr };
    int fd { -1 };

    int screen_width { 0 };
    int screen_height { 0 };

    unsigned buttons { 0 };
    bool pointer_is_absolute { false };
    // Absolute positions are cached so button/wheel events (which carry no
    // coordinates of their own) can be reported at the right place.
    int absolute_x { 0 };
    int absolute_y { 0 };

    unsigned modifiers { 0 };
    bool caps_lock_on { false };
};

State s_state;

int open_restricted(char const* path, int flags, void*)
{
    int fd = ::open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void close_restricted(int fd, void*)
{
    ::close(fd);
}

libinput_interface const s_interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

Function<void(MousePacket const&)> s_on_mouse;
Function<void(::KeyEvent const&)> s_on_key;

void emit_buttons_packet()
{
    if (!s_on_mouse)
        return;

    MousePacket packet;
    if (s_state.pointer_is_absolute) {
        packet.is_relative = false;
        packet.x = s_state.absolute_x;
        packet.y = s_state.absolute_y;
    } else {
        // Relative button events: zero motion, only the button bits change.
        packet.is_relative = true;
        packet.x = 0;
        packet.y = 0;
    }
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    s_on_mouse(packet);
}

void handle_pointer_motion_absolute(libinput_event* event)
{
    if (!s_on_mouse)
        return;

    auto* pointer_event = libinput_event_get_pointer_event(event);
    // ScreenInput treats absolute coordinates as 16-bit fractions of the
    // screen, so ask libinput to normalise straight into that range.
    auto x = libinput_event_pointer_get_absolute_x_transformed(pointer_event, 0xffff);
    auto y = libinput_event_pointer_get_absolute_y_transformed(pointer_event, 0xffff);

    s_state.pointer_is_absolute = true;
    s_state.absolute_x = static_cast<int>(x);
    s_state.absolute_y = static_cast<int>(y);

    MousePacket packet;
    packet.is_relative = false;
    packet.x = s_state.absolute_x;
    packet.y = s_state.absolute_y;
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    s_on_mouse(packet);
}

void handle_pointer_motion(libinput_event* event)
{
    if (!s_on_mouse)
        return;

    auto* pointer_event = libinput_event_get_pointer_event(event);
    auto dx = libinput_event_pointer_get_dx(pointer_event);
    auto dy = libinput_event_pointer_get_dy(pointer_event);

    // Sub-pixel motion would otherwise be rounded away and the pointer would
    // feel stuck at low speeds.
    static double residual_x = 0;
    static double residual_y = 0;
    residual_x += dx;
    residual_y += dy;

    auto step_x = static_cast<int>(residual_x);
    auto step_y = static_cast<int>(residual_y);
    if (step_x == 0 && step_y == 0)
        return;
    residual_x -= step_x;
    residual_y -= step_y;

    s_state.pointer_is_absolute = false;

    MousePacket packet;
    packet.is_relative = true;
    packet.x = step_x;
    packet.y = step_y;
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    s_on_mouse(packet);
}

// 按钮位映射 + 状态更新抽出来：libinput 直连路径和 wlroots 喂事件路径共用。
void apply_pointer_button(uint32_t button, bool pressed)
{
    unsigned char bit = 0;
    switch (button) {
    case 0x110: // BTN_LEFT
        bit = MousePacket::LeftButton;
        break;
    case 0x111: // BTN_RIGHT
        bit = MousePacket::RightButton;
        break;
    case 0x112: // BTN_MIDDLE
        bit = MousePacket::MiddleButton;
        break;
    case 0x113: // BTN_SIDE
        bit = MousePacket::BackwardButton;
        break;
    case 0x114: // BTN_EXTRA
        bit = MousePacket::ForwardButton;
        break;
    default:
        return;
    }

    if (pressed)
        s_state.buttons |= bit;
    else
        s_state.buttons &= static_cast<unsigned>(~bit);

    if (getenv("SERENITY_INPUT_DEBUG"))
        dbgln("INBTN code={} pressed={} buttons={} abs={}", button,
            pressed, s_state.buttons, s_state.pointer_is_absolute);

    emit_buttons_packet();
}

void handle_pointer_button(libinput_event* event)
{
    auto* pointer_event = libinput_event_get_pointer_event(event);
    auto button = libinput_event_pointer_get_button(pointer_event);
    auto state = libinput_event_pointer_get_button_state(pointer_event);
    apply_pointer_button(button, state == LIBINPUT_BUTTON_STATE_PRESSED);
}

void handle_pointer_axis(libinput_event* event)
{
    if (!s_on_mouse)
        return;

    auto* pointer_event = libinput_event_get_pointer_event(event);
    // Only read an axis that the event actually carries; querying an unset axis
    // makes libinput spam "value requested for unset axis" and can misreport the
    // scroll direction. GTK/Wayland often send a vertical wheel event with no
    // horizontal component (or vice versa).
    bool has_v = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    bool has_h = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    if (!has_v && !has_h)
        return;
    double vertical = has_v ? libinput_event_pointer_get_axis_value(
        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
                            : 0.0;
    double horizontal = has_h ? libinput_event_pointer_get_axis_value(
        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
                              : 0.0;

    if (vertical == 0 && horizontal == 0)
        return;

    MousePacket packet;
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    if (s_state.pointer_is_absolute) {
        packet.is_relative = false;
        packet.x = s_state.absolute_x;
        packet.y = s_state.absolute_y;
    } else {
        // A relative device (a real mouse) has no absolute position: absolute_x/y
        // are 0, and reporting them as an absolute packet teleports the cursor
        // to the top-left corner on every wheel tick. Send a zero delta instead
        // so ScreenInput leaves the cursor where it is; z/w still drive the
        // scroll.
        packet.is_relative = true;
        packet.x = 0;
        packet.y = 0;
    }
    // libinput reports "positive = down"; Serenity's z is positive upwards.
    packet.z = static_cast<int>(lround(-vertical / 15.0));
    packet.w = static_cast<int>(lround(horizontal / 15.0));
    s_on_mouse(packet);
}

// 键码翻译（keymap 查表 + 修饰位 + 大小写）抽出来，两条输入路径共用。
void apply_keyboard_key(uint32_t code, bool pressed)
{
    auto const* entry = serenity_keymap_lookup(code);
    if (!entry)
        return;

    bool is_modifier = entry->plain == 0 && entry->shifted == 0;
    if (is_modifier) {
        unsigned bit = 0;
        switch (code) {
        case 42: // KEY_LEFTSHIFT
        case 54: // KEY_RIGHTSHIFT
            bit = Mod_Shift;
            break;
        case 29: // KEY_LEFTCTRL
        case 97: // KEY_RIGHTCTRL
            bit = Mod_Ctrl;
            break;
        case 56: // KEY_LEFTALT
            bit = Mod_Alt;
            break;
        case 100: // KEY_RIGHTALT
            bit = Mod_AltGr;
            break;
        case 125: // KEY_LEFTMETA
        case 126: // KEY_RIGHTMETA
            bit = Mod_Super;
            break;
        case 58: // KEY_CAPSLOCK
            if (pressed)
                s_state.caps_lock_on = !s_state.caps_lock_on;
            break;
        default:
            break;
        }
        if (bit) {
            if (pressed)
                s_state.modifiers |= bit;
            else
                s_state.modifiers &= ~bit;
        }
    }

    u32 code_point = 0;
    if (entry->plain) {
        bool upper = (s_state.modifiers & Mod_Shift) ? true : s_state.caps_lock_on;
        // Caps lock only affects letters.
        if (!(entry->plain >= 'a' && entry->plain <= 'z'))
            upper = (s_state.modifiers & Mod_Shift) != 0;
        code_point = static_cast<u32>(static_cast<unsigned char>(upper ? entry->shifted : entry->plain));
    }

    if (!s_on_key)
        return;

    ::KeyEvent translated {};
    translated.key = static_cast<KeyCode>(entry->serenity);
    translated.map_entry_index = 0;
    translated.scancode = code;
    translated.code_point = code_point;
    translated.flags = static_cast<u8>(s_state.modifiers | (pressed ? Is_Press : 0));
    translated.caps_lock_on = s_state.caps_lock_on;
    s_on_key(translated);
}

void handle_keyboard_key(libinput_event* event)
{
    auto* key_event = libinput_event_get_keyboard_event(event);
    apply_keyboard_key(libinput_event_keyboard_get_key(key_event),
        libinput_event_keyboard_get_key_state(key_event) == LIBINPUT_KEY_STATE_PRESSED);
}

} // namespace

// ── wlroots 输入路径的入口 ───────────────────────────────────────────────────
// 内嵌 wlroots 合成器拥有 libinput，命中不到外来窗口的事件由桥回调回来。
// 这里只做「像素坐标 → Serenity 的 16 位绝对坐标」和直接调用上面的翻译函数，
// 保证两条路径（直连 libinput / 经 wlroots）产出完全一致的 MousePacket / KeyEvent。
void feed_pointer_motion_absolute(double x_pixels, double y_pixels)
{
    if (!s_on_mouse)
        return;

    int const screen_w = s_state.screen_width > 0 ? s_state.screen_width : 1;
    int const screen_h = s_state.screen_height > 0 ? s_state.screen_height : 1;
    s_state.pointer_is_absolute = true;
    s_state.absolute_x = static_cast<int>(lround(x_pixels / double(screen_w) * 0xffff));
    s_state.absolute_y = static_cast<int>(lround(y_pixels / double(screen_h) * 0xffff));

    MousePacket packet;
    packet.is_relative = false;
    packet.x = s_state.absolute_x;
    packet.y = s_state.absolute_y;
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    s_on_mouse(packet);
}

void feed_pointer_button(uint32_t evdev_button, bool pressed)
{
    apply_pointer_button(evdev_button, pressed);
}

void feed_pointer_axis(double vertical, double horizontal)
{
    if (!s_on_mouse)
        return;
    if (vertical == 0 && horizontal == 0)
        return;

    MousePacket packet;
    packet.buttons = static_cast<unsigned char>(s_state.buttons);
    if (s_state.pointer_is_absolute) {
        packet.is_relative = false;
        packet.x = s_state.absolute_x;
        packet.y = s_state.absolute_y;
    } else {
        packet.is_relative = true;
        packet.x = 0;
        packet.y = 0;
    }
    packet.z = static_cast<int>(lround(-vertical / 15.0));
    packet.w = static_cast<int>(lround(horizontal / 15.0));
    s_on_mouse(packet);
}

void feed_keyboard_key(uint32_t evdev_code, bool pressed)
{
    apply_keyboard_key(evdev_code, pressed);
}

bool initialize(int screen_width, int screen_height)
{
    if (s_state.input)
        return true;

    s_state.screen_width = screen_width;
    s_state.screen_height = screen_height;

    s_state.udev_context = udev_new();
    if (!s_state.udev_context) {
        warnln("LinuxInput: udev_new failed");
        return false;
    }

    s_state.input = libinput_udev_create_context(&s_interface, nullptr, s_state.udev_context);
    if (!s_state.input) {
        warnln("LinuxInput: libinput_udev_create_context failed");
        return false;
    }

    if (libinput_udev_assign_seat(s_state.input, "seat0") != 0) {
        warnln("LinuxInput: libinput_udev_assign_seat failed (errno={})", errno);
        libinput_unref(s_state.input);
        s_state.input = nullptr;
        return false;
    }

    s_state.fd = libinput_get_fd(s_state.input);
    libinput_dispatch(s_state.input);
    warnln("LinuxInput: libinput ready on seat0 (fd={})", s_state.fd);
    return true;
}

void set_screen_size(int screen_width, int screen_height)
{
    s_state.screen_width = screen_width;
    s_state.screen_height = screen_height;
}

bool is_initialized()
{
    return s_state.input != nullptr;
}

void dispatch()
{
    if (!s_state.input)
        return;

    // Cheap poll first: at 8ms ticks libinput is idle most of the time.
    pollfd pfd {};
    pfd.fd = s_state.fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0)
        return;

    if (libinput_dispatch(s_state.input) != 0)
        return;

    while (auto* event = libinput_get_event(s_state.input)) {
        switch (libinput_event_get_type(event)) {
        case LIBINPUT_EVENT_DEVICE_ADDED: {
            // Test/VM aid: QEMU has only a relative pointer, and libinput's
            // default adaptive acceleration makes open-loop positioning (VNC
            // absolute clicks converted to deltas) drift unpredictably. With a
            // flat profile an injected delta maps 1:1 to screen pixels, which
            // is what the screenshot/click scripts assume.
            static bool const flat_accel = getenv("SERENITY_POINTER_FLAT_ACCEL") != nullptr;
            auto* device = libinput_event_get_device(event);
            if (flat_accel && libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER))
                libinput_device_config_accel_set_profile(device, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION:
            handle_pointer_motion(event);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            handle_pointer_motion_absolute(event);
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON:
            handle_pointer_button(event);
            break;
        case LIBINPUT_EVENT_POINTER_AXIS:
            handle_pointer_axis(event);
            break;
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            handle_keyboard_key(event);
            break;
        default:
            break;
        }
        libinput_event_destroy(event);
    }
}

Function<void(MousePacket const&)>& on_mouse()
{
    return s_on_mouse;
}

Function<void(::KeyEvent const&)>& on_key()
{
    return s_on_key;
}

}
