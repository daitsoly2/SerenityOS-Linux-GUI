// SPDX-License-Identifier: BSD-2-Clause
/*
 * serenity-input-bridge - feed Linux evdev input into SerenityOS semantics.
 *
 * Same split as the audio path: the kernel (serenity_compat) owns a character
 * device with Serenity's native record format, and this userspace daemon is the
 * only thing that knows about the host. Here it reads /dev/input/event* and
 * writes MousePacket / KeyEvent records, which the ported WindowServer then
 * reads with a plain read() - no SDL event pump in the middle.
 *
 * Record layout is taken verbatim from the kernel headers:
 *   struct MousePacket { int x, y, z, w; unsigned char buttons; bool is_relative; }
 *   struct KeyEvent    { KeyCode key; u8 map_entry_index; u64 scancode;
 *                        u32 code_point; u8 flags; bool caps_lock_on; }
 *
 * Build:  make
 * Use:    serenity-input-bridge --debug            # print translated records
 *         serenity-input-bridge                    # write to /dev/{mouse,keyboard}/0
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "serenity_keycode.h"

/* ---- ABI with the kernel side ------------------------------------------- */

struct serenity_mouse_packet {
    int32_t x, y, z, w;
    uint8_t buttons;
    bool is_relative;
};

struct serenity_key_event {
    uint8_t key;
    uint8_t map_entry_index;
    uint64_t scancode;
    uint32_t code_point;
    uint8_t flags;
    bool caps_lock_on;
};

_Static_assert(sizeof(struct serenity_mouse_packet) == 20, "MousePacket ABI changed");
_Static_assert(sizeof(struct serenity_key_event) == 24, "KeyEvent ABI changed");

#define MOUSE_BUTTON_LEFT 0x01
#define MOUSE_BUTTON_RIGHT 0x02
#define MOUSE_BUTTON_MIDDLE 0x04
#define MOUSE_BUTTON_BACKWARD 0x08
#define MOUSE_BUTTON_FORWARD 0x10

#define MOD_ALT 0x01
#define MOD_CTRL 0x02
#define MOD_SHIFT 0x04
#define MOD_SUPER 0x08
#define MOD_ALTGR 0x10
#define MOD_KEYPAD 0x20
#define FLAG_IS_PRESS 0x80

/* ---- evdev -> Serenity KeyCode ------------------------------------------ */

struct keymap_entry {
    uint16_t evdev;
    uint8_t serenity;
    char plain;
    char shifted;
};

/* Only the keys a desktop actually produces are listed; everything else maps
 * to Key_Invalid and is dropped rather than guessed at. */
static struct keymap_entry const s_keymap[] = {
    { KEY_ESC, SERENITY_KEY_Escape, 0, 0 },
    { KEY_TAB, SERENITY_KEY_Tab, 0, 0 },
    { KEY_BACKSPACE, SERENITY_KEY_Backspace, 0, 0 },
    { KEY_ENTER, SERENITY_KEY_Return, 0, 0 },
    { KEY_KPENTER, SERENITY_KEY_Return, 0, 0 },
    { KEY_INSERT, SERENITY_KEY_Insert, 0, 0 },
    { KEY_DELETE, SERENITY_KEY_Delete, 0, 0 },
    { KEY_HOME, SERENITY_KEY_Home, 0, 0 },
    { KEY_END, SERENITY_KEY_End, 0, 0 },
    { KEY_PAGEUP, SERENITY_KEY_PageUp, 0, 0 },
    { KEY_PAGEDOWN, SERENITY_KEY_PageDown, 0, 0 },
    { KEY_UP, SERENITY_KEY_Up, 0, 0 },
    { KEY_DOWN, SERENITY_KEY_Down, 0, 0 },
    { KEY_LEFT, SERENITY_KEY_Left, 0, 0 },
    { KEY_RIGHT, SERENITY_KEY_Right, 0, 0 },
    { KEY_LEFTSHIFT, SERENITY_KEY_LeftShift, 0, 0 },
    { KEY_RIGHTSHIFT, SERENITY_KEY_RightShift, 0, 0 },
    { KEY_LEFTCTRL, SERENITY_KEY_LeftControl, 0, 0 },
    { KEY_RIGHTCTRL, SERENITY_KEY_RightControl, 0, 0 },
    { KEY_LEFTALT, SERENITY_KEY_LeftAlt, 0, 0 },
    { KEY_RIGHTALT, SERENITY_KEY_RightAlt, 0, 0 },
    { KEY_LEFTMETA, SERENITY_KEY_LeftSuper, 0, 0 },
    { KEY_RIGHTMETA, SERENITY_KEY_RightSuper, 0, 0 },
    { KEY_CAPSLOCK, SERENITY_KEY_CapsLock, 0, 0 },
    { KEY_SPACE, SERENITY_KEY_Space, ' ', ' ' },
    { KEY_MINUS, SERENITY_KEY_Minus, '-', '_' },
    { KEY_EQUAL, SERENITY_KEY_Equal, '=', '+' },
    { KEY_LEFTBRACE, SERENITY_KEY_LeftBracket, '[', '{' },
    { KEY_RIGHTBRACE, SERENITY_KEY_RightBracket, ']', '}' },
    { KEY_BACKSLASH, SERENITY_KEY_Backslash, '\\', '|' },
    { KEY_SEMICOLON, SERENITY_KEY_Semicolon, ';', ':' },
    { KEY_APOSTROPHE, SERENITY_KEY_Apostrophe, '\'', '"' },
    { KEY_GRAVE, SERENITY_KEY_Backtick, '`', '~' },
    { KEY_COMMA, SERENITY_KEY_Comma, ',', '<' },
    { KEY_DOT, SERENITY_KEY_Period, '.', '>' },
    { KEY_SLASH, SERENITY_KEY_Slash, '/', '?' },
    { KEY_1, SERENITY_KEY_1, '1', '!' },
    { KEY_2, SERENITY_KEY_2, '2', '@' },
    { KEY_3, SERENITY_KEY_3, '3', '#' },
    { KEY_4, SERENITY_KEY_4, '4', '$' },
    { KEY_5, SERENITY_KEY_5, '5', '%' },
    { KEY_6, SERENITY_KEY_6, '6', '^' },
    { KEY_7, SERENITY_KEY_7, '7', '&' },
    { KEY_8, SERENITY_KEY_8, '8', '*' },
    { KEY_9, SERENITY_KEY_9, '9', '(' },
    { KEY_0, SERENITY_KEY_0, '0', ')' },
    { KEY_F1, SERENITY_KEY_F1, 0, 0 },
    { KEY_F2, SERENITY_KEY_F2, 0, 0 },
    { KEY_F3, SERENITY_KEY_F3, 0, 0 },
    { KEY_F4, SERENITY_KEY_F4, 0, 0 },
    { KEY_F5, SERENITY_KEY_F5, 0, 0 },
    { KEY_F6, SERENITY_KEY_F6, 0, 0 },
    { KEY_F7, SERENITY_KEY_F7, 0, 0 },
    { KEY_F8, SERENITY_KEY_F8, 0, 0 },
    { KEY_F9, SERENITY_KEY_F9, 0, 0 },
    { KEY_F10, SERENITY_KEY_F10, 0, 0 },
    { KEY_F11, SERENITY_KEY_F11, 0, 0 },
    { KEY_F12, SERENITY_KEY_F12, 0, 0 },
};

/* Letters are contiguous on both sides, so they are handled arithmetically. */
static struct keymap_entry const* lookup_key(uint16_t evdev)
{
    if (evdev >= KEY_Q && evdev <= KEY_P) {
        static struct keymap_entry const row_qwerty[] = {
            { KEY_Q, SERENITY_KEY_Q, 'q', 'Q' }, { KEY_W, SERENITY_KEY_W, 'w', 'W' },
            { KEY_E, SERENITY_KEY_E, 'e', 'E' }, { KEY_R, SERENITY_KEY_R, 'r', 'R' },
            { KEY_T, SERENITY_KEY_T, 't', 'T' }, { KEY_Y, SERENITY_KEY_Y, 'y', 'Y' },
            { KEY_U, SERENITY_KEY_U, 'u', 'U' }, { KEY_I, SERENITY_KEY_I, 'i', 'I' },
            { KEY_O, SERENITY_KEY_O, 'o', 'O' }, { KEY_P, SERENITY_KEY_P, 'p', 'P' },
        };
        return &row_qwerty[evdev - KEY_Q];
    }
    if (evdev >= KEY_A && evdev <= KEY_L) {
        static struct keymap_entry const row_asdf[] = {
            { KEY_A, SERENITY_KEY_A, 'a', 'A' }, { KEY_S, SERENITY_KEY_S, 's', 'S' },
            { KEY_D, SERENITY_KEY_D, 'd', 'D' }, { KEY_F, SERENITY_KEY_F, 'f', 'F' },
            { KEY_G, SERENITY_KEY_G, 'g', 'G' }, { KEY_H, SERENITY_KEY_H, 'h', 'H' },
            { KEY_J, SERENITY_KEY_J, 'j', 'J' }, { KEY_K, SERENITY_KEY_K, 'k', 'K' },
            { KEY_L, SERENITY_KEY_L, 'l', 'L' },
        };
        return &row_asdf[evdev - KEY_A];
    }
    if (evdev >= KEY_Z && evdev <= KEY_M) {
        static struct keymap_entry const row_zxcv[] = {
            { KEY_Z, SERENITY_KEY_Z, 'z', 'Z' }, { KEY_X, SERENITY_KEY_X, 'x', 'X' },
            { KEY_C, SERENITY_KEY_C, 'c', 'C' }, { KEY_V, SERENITY_KEY_V, 'v', 'V' },
            { KEY_B, SERENITY_KEY_B, 'b', 'B' }, { KEY_N, SERENITY_KEY_N, 'n', 'N' },
            { KEY_M, SERENITY_KEY_M, 'm', 'M' },
        };
        return &row_zxcv[evdev - KEY_Z];
    }
    for (size_t i = 0; i < sizeof(s_keymap) / sizeof(s_keymap[0]); ++i) {
        if (s_keymap[i].evdev == evdev)
            return &s_keymap[i];
    }
    return NULL;
}

/* ---- device discovery ---------------------------------------------------- */

struct input_device {
    char path[64];
    int fd;
    bool is_keyboard;
    struct input_absinfo abs_x, abs_y;
    bool has_abs;
};

static bool test_bit(int bit, unsigned long const* array)
{
    return (array[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

static int scan_devices(struct input_device* keyboard, struct input_device* mouse,
    char const* keyboard_path, char const* mouse_path)
{
    keyboard->fd = -1;
    mouse->fd = -1;

    /* Explicit paths win: several devices may look like a keyboard/pointer and
       this is also how the bridge is tested against QEMU's virtio nodes. */
    if (keyboard_path) {
        keyboard->fd = open(keyboard_path, O_RDONLY | O_NONBLOCK);
        if (keyboard->fd >= 0) {
            snprintf(keyboard->path, sizeof(keyboard->path), "%s", keyboard_path);
            keyboard->is_keyboard = true;
            fprintf(stderr, "input-bridge: keyboard -> %s (forced)\n", keyboard_path);
        }
    }
    if (mouse_path) {
        mouse->fd = open(mouse_path, O_RDONLY | O_NONBLOCK);
        if (mouse->fd >= 0) {
            snprintf(mouse->path, sizeof(mouse->path), "%s", mouse_path);
            mouse->has_abs = true;
            ioctl(mouse->fd, EVIOCGABS(ABS_X), &mouse->abs_x);
            ioctl(mouse->fd, EVIOCGABS(ABS_Y), &mouse->abs_y);
            if (mouse->abs_x.maximum == 0)
                mouse->has_abs = false;
            fprintf(stderr, "input-bridge: pointer -> %s (forced, %s)\n",
                mouse_path, mouse->has_abs ? "absolute" : "relative");
        }
    }
    if (keyboard->fd >= 0 && mouse->fd >= 0)
        return 2;

    for (int index = 0; index < 32; ++index) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", index);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = { 0 };
        unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = { 0 };
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

        bool has_keys = test_bit(KEY_A, key_bits);
        bool has_buttons = test_bit(BTN_LEFT, key_bits);
        bool has_abs = test_bit(EV_ABS, ev_bits);
        bool has_rel = test_bit(EV_REL, ev_bits);

        if (keyboard->fd >= 0) {
        close(fd);
        continue;
    }
    if (has_keys && keyboard->fd < 0) {
            snprintf(keyboard->path, sizeof(keyboard->path), "%s", path);
            keyboard->fd = fd;
            keyboard->is_keyboard = true;
            fprintf(stderr, "input-bridge: keyboard -> %s\n", path);
            continue;
        }

        if (mouse->fd >= 0) {
            close(fd);
            continue;
        }
        if ((has_buttons || has_abs || has_rel) && mouse->fd < 0) {
            snprintf(mouse->path, sizeof(mouse->path), "%s", path);
            mouse->fd = fd;
            mouse->is_keyboard = false;
            mouse->has_abs = has_abs;
            if (has_abs) {
                ioctl(fd, EVIOCGABS(ABS_X), &mouse->abs_x);
                ioctl(fd, EVIOCGABS(ABS_Y), &mouse->abs_y);
            }
            fprintf(stderr, "input-bridge: pointer -> %s (%s)\n", path, has_abs ? "absolute" : "relative");
            continue;
        }

        close(fd);
    }

    return (keyboard->fd >= 0 ? 1 : 0) + (mouse->fd >= 0 ? 1 : 0);
}

/* ---- main ---------------------------------------------------------------- */

static volatile sig_atomic_t s_quit;

static void on_signal(int signal_number)
{
    (void)signal_number;
    s_quit = 1;
}

static void emit(int fd, void const* record, size_t size)
{
    if (fd < 0)
        return;
    ssize_t written = write(fd, record, size);
    (void)written;
}

int main(int argc, char** argv)
{
    char const* mouse_out_path = "/dev/mouse/0";
    char const* keyboard_out_path = "/dev/keyboard/0";
    char const* mouse_in_path = NULL;
    char const* keyboard_in_path = NULL;
    bool debug = false;
    int screen_width = 1280;
    int screen_height = 960;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--debug")) {
            debug = true;
        } else if (!strcmp(argv[i], "--mouse-out") && i + 1 < argc) {
            mouse_out_path = argv[++i];
        } else if (!strcmp(argv[i], "--keyboard-out") && i + 1 < argc) {
            keyboard_out_path = argv[++i];
        } else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) {
            mouse_in_path = argv[++i];
        } else if (!strcmp(argv[i], "--keyboard") && i + 1 < argc) {
            keyboard_in_path = argv[++i];
        } else if (!strcmp(argv[i], "--screen") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &screen_width, &screen_height);
            fprintf(stderr, "input-bridge: --screen is informational; absolute "
                            "coordinates are sent as 16-bit (0..0xffff)\n");
        } else {
            fprintf(stderr, "usage: %s [--debug] [--mouse PATH] [--keyboard PATH] "
                            "[--mouse-out PATH] [--keyboard-out PATH] [--screen WxH]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct input_device keyboard = { 0 }, mouse = { 0 };
    if (scan_devices(&keyboard, &mouse, keyboard_in_path, mouse_in_path) == 0) {
        fprintf(stderr, "input-bridge: no input devices found\n");
        return 1;
    }

    int mouse_out = open(mouse_out_path, O_WRONLY | O_NONBLOCK);
    int keyboard_out = open(keyboard_out_path, O_WRONLY | O_NONBLOCK);
    if (!debug && mouse_out < 0 && keyboard_out < 0) {
        fprintf(stderr, "input-bridge: %s and %s both missing; kernel side not loaded?\n",
            mouse_out_path, keyboard_out_path);
        return 1;
    }
    fprintf(stderr, "input-bridge: mouse -> %s%s, keyboard -> %s%s\n",
        mouse_out_path, mouse_out < 0 ? " (debug only)" : "",
        keyboard_out_path, keyboard_out < 0 ? " (debug only)" : "");

    unsigned char mouse_buttons = 0;
    /* For relative devices these accumulate deltas since the last report; for
       absolute devices they hold the mapped screen position. */
    int mouse_x = 0, mouse_y = 0;
    bool mouse_dirty = false;
    uint8_t modifiers = 0;
    bool caps_lock = false;

    while (!s_quit) {
        struct pollfd fds[2];
        int count = 0;
        if (mouse.fd >= 0)
            fds[count++] = (struct pollfd) { .fd = mouse.fd, .events = POLLIN };
        if (keyboard.fd >= 0)
            fds[count++] = (struct pollfd) { .fd = keyboard.fd, .events = POLLIN };

        if (poll(fds, count, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        for (int i = 0; i < count; ++i) {
            if (!(fds[i].revents & POLLIN))
                continue;

            struct input_event events[64];
            ssize_t bytes = read(fds[i].fd, events, sizeof(events));
            if (bytes <= 0)
                continue;

            size_t event_count = (size_t)bytes / sizeof(struct input_event);
            for (size_t e = 0; e < event_count; ++e) {
                struct input_event const* event = &events[e];

                if (fds[i].fd == mouse.fd) {
                    switch (event->type) {
                    case EV_REL:
                        if (event->code == REL_X || event->code == REL_Y) {
                            /* Keep raw deltas; WindowServer applies acceleration. */
                            if (event->code == REL_X)
                                mouse_x += event->value;
                            else
                                mouse_y += event->value;
                            mouse_dirty = true;
                        } else if (event->code == REL_WHEEL) {
                            mouse_dirty = true;
                        }
                        break;
                    case EV_ABS:
                        /* Serenity's absolute mouse coordinates are 16-bit, not
                           pixels: ScreenInput does `x * width / 0xffff`. Feed it
                           the same scale the kernel driver would. */
                        if (event->code == ABS_X && mouse.abs_x.maximum > 0)
                            mouse_x = (int)((long)event->value * 0xffff / mouse.abs_x.maximum);
                        else if (event->code == ABS_Y && mouse.abs_y.maximum > 0)
                            mouse_y = (int)((long)event->value * 0xffff / mouse.abs_y.maximum);
                        mouse_dirty = true;
                        break;
                    case EV_KEY:
                        switch (event->code) {
                        case BTN_LEFT: mouse_buttons = event->value ? (mouse_buttons | MOUSE_BUTTON_LEFT) : (mouse_buttons & ~MOUSE_BUTTON_LEFT); break;
                        case BTN_RIGHT: mouse_buttons = event->value ? (mouse_buttons | MOUSE_BUTTON_RIGHT) : (mouse_buttons & ~MOUSE_BUTTON_RIGHT); break;
                        case BTN_MIDDLE: mouse_buttons = event->value ? (mouse_buttons | MOUSE_BUTTON_MIDDLE) : (mouse_buttons & ~MOUSE_BUTTON_MIDDLE); break;
                        case BTN_SIDE: mouse_buttons = event->value ? (mouse_buttons | MOUSE_BUTTON_BACKWARD) : (mouse_buttons & ~MOUSE_BUTTON_BACKWARD); break;
                        case BTN_EXTRA: mouse_buttons = event->value ? (mouse_buttons | MOUSE_BUTTON_FORWARD) : (mouse_buttons & ~MOUSE_BUTTON_FORWARD); break;
                        default: break;
                        }
                        mouse_dirty = true;
                        break;
                    case EV_SYN:
                        if (event->code == SYN_REPORT && mouse_dirty) {
                            struct serenity_mouse_packet packet = {
                                .x = mouse_x,
                                .y = mouse_y,
                                .z = 0,
                                .w = 0,
                                .buttons = mouse_buttons,
                                .is_relative = !mouse.has_abs,
                            };
                            emit(mouse_out, &packet, sizeof(packet));
                            if (debug)
                                fprintf(stderr, "[mouse] %s x=%d y=%d buttons=0x%02x\n",
                                    packet.is_relative ? "rel" : "abs", packet.x, packet.y, packet.buttons);
                            /* Deltas are consumed once reported; absolute
                               positions persist. */
                            if (packet.is_relative)
                                mouse_x = mouse_y = 0;
                            mouse_dirty = false;
                        }
                        break;
                    default:
                        break;
                    }
                } else {
                    if (event->type != EV_KEY)
                        continue;

                    struct keymap_entry const* entry = lookup_key(event->code);
                    if (!entry)
                        continue;

                    bool pressed = event->value != 0;
                    bool is_modifier = entry->plain == 0 && entry->shifted == 0;

                    if (is_modifier) {
                        uint8_t bit = 0;
                        switch (event->code) {
                        case KEY_LEFTSHIFT:
                        case KEY_RIGHTSHIFT: bit = MOD_SHIFT; break;
                        case KEY_LEFTCTRL:
                        case KEY_RIGHTCTRL: bit = MOD_CTRL; break;
                        case KEY_LEFTALT: bit = MOD_ALT; break;
                        case KEY_RIGHTALT: bit = MOD_ALTGR; break;
                        case KEY_LEFTMETA:
                        case KEY_RIGHTMETA: bit = MOD_SUPER; break;
                        case KEY_CAPSLOCK:
                            if (pressed)
                                caps_lock = !caps_lock;
                            break;
                        default: break;
                        }
                        if (bit) {
                            if (pressed)
                                modifiers |= bit;
                            else
                                modifiers &= (uint8_t)~bit;
                        }
                    }

                    uint32_t code_point = 0;
                    if (entry->plain) {
                        bool upper = (modifiers & MOD_SHIFT) ? true : caps_lock;
                        char ch = upper ? entry->shifted : entry->plain;
                        /* Digits and punctuation ignore caps lock. */
                        if (!(entry->plain >= 'a' && entry->plain <= 'z'))
                            ch = (modifiers & MOD_SHIFT) ? entry->shifted : entry->plain;
                        code_point = (uint32_t)(unsigned char)ch;
                    }

                    struct serenity_key_event key_event = {
                        .key = entry->serenity,
                        .map_entry_index = 0,
                        .scancode = event->code,
                        .code_point = code_point,
                        .flags = (uint8_t)(modifiers | (pressed ? FLAG_IS_PRESS : 0)),
                        .caps_lock_on = caps_lock,
                    };
                    emit(keyboard_out, &key_event, sizeof(key_event));
                    if (debug)
                        fprintf(stderr, "[key] code=%u serenity=%u char='%c' flags=0x%02x press=%d\n",
                            event->code, entry->serenity,
                            code_point ? (char)code_point : '.', key_event.flags, pressed);
                }
            }
        }
    }

    if (mouse.fd >= 0)
        close(mouse.fd);
    if (keyboard.fd >= 0)
        close(keyboard.fd);
    if (mouse_out >= 0)
        close(mouse_out);
    if (keyboard_out >= 0)
        close(keyboard_out);
    return 0;
}
