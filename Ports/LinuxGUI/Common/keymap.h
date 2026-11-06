/* Shared evdev -> SerenityOS key mapping.
 *
 * Kept in one place because both the WindowServer (libinput backend) and the
 * standalone evdev bridge need the same translation, and a mismatch between
 * them is the kind of bug that only shows up as "one key is wrong".
 */
#pragma once

#include "serenity_keycode.h"

struct serenity_keymap_entry {
    unsigned short evdev;
    unsigned char serenity;
    char plain;
    char shifted;
};

static const struct serenity_keymap_entry serenity_keymap_simple[] = {
    { 1, SERENITY_KEY_Escape, 0, 0 },          /* KEY_ESC */
    { 15, SERENITY_KEY_Tab, 0, 0 },            /* KEY_TAB */
    { 14, SERENITY_KEY_Backspace, 0, 0 },      /* KEY_BACKSPACE */
    { 28, SERENITY_KEY_Return, 0, 0 },         /* KEY_ENTER */
    { 96, SERENITY_KEY_Return, 0, 0 },         /* KEY_KPENTER */
    { 110, SERENITY_KEY_Insert, 0, 0 },
    { 111, SERENITY_KEY_Delete, 0, 0 },
    { 102, SERENITY_KEY_Home, 0, 0 },
    { 107, SERENITY_KEY_End, 0, 0 },
    { 104, SERENITY_KEY_PageUp, 0, 0 },
    { 109, SERENITY_KEY_PageDown, 0, 0 },
    { 103, SERENITY_KEY_Up, 0, 0 },
    { 108, SERENITY_KEY_Down, 0, 0 },
    { 105, SERENITY_KEY_Left, 0, 0 },
    { 106, SERENITY_KEY_Right, 0, 0 },
    { 42, SERENITY_KEY_LeftShift, 0, 0 },
    { 54, SERENITY_KEY_RightShift, 0, 0 },
    { 29, SERENITY_KEY_LeftControl, 0, 0 },
    { 97, SERENITY_KEY_RightControl, 0, 0 },
    { 56, SERENITY_KEY_LeftAlt, 0, 0 },
    { 100, SERENITY_KEY_RightAlt, 0, 0 },
    { 125, SERENITY_KEY_LeftSuper, 0, 0 },
    { 126, SERENITY_KEY_RightSuper, 0, 0 },
    { 58, SERENITY_KEY_CapsLock, 0, 0 },
    { 57, SERENITY_KEY_Space, ' ', ' ' },
    { 12, SERENITY_KEY_Minus, '-', '_' },
    { 13, SERENITY_KEY_Equal, '=', '+' },
    { 26, SERENITY_KEY_LeftBracket, '[', '{' },
    { 27, SERENITY_KEY_RightBracket, ']', '}' },
    { 43, SERENITY_KEY_Backslash, '\\', '|' },
    { 39, SERENITY_KEY_Semicolon, ';', ':' },
    { 40, SERENITY_KEY_Apostrophe, '\'', '"' },
    { 41, SERENITY_KEY_Backtick, '`', '~' },
    { 51, SERENITY_KEY_Comma, ',', '<' },
    { 52, SERENITY_KEY_Period, '.', '>' },
    { 53, SERENITY_KEY_Slash, '/', '?' },
    { 2, SERENITY_KEY_1, '1', '!' },
    { 3, SERENITY_KEY_2, '2', '@' },
    { 4, SERENITY_KEY_3, '3', '#' },
    { 5, SERENITY_KEY_4, '4', '$' },
    { 6, SERENITY_KEY_5, '5', '%' },
    { 7, SERENITY_KEY_6, '6', '^' },
    { 8, SERENITY_KEY_7, '7', '&' },
    { 9, SERENITY_KEY_8, '8', '*' },
    { 10, SERENITY_KEY_9, '9', '(' },
    { 11, SERENITY_KEY_0, '0', ')' },
    { 59, SERENITY_KEY_F1, 0, 0 },
    { 60, SERENITY_KEY_F2, 0, 0 },
    { 61, SERENITY_KEY_F3, 0, 0 },
    { 62, SERENITY_KEY_F4, 0, 0 },
    { 63, SERENITY_KEY_F5, 0, 0 },
    { 64, SERENITY_KEY_F6, 0, 0 },
    { 65, SERENITY_KEY_F7, 0, 0 },
    { 66, SERENITY_KEY_F8, 0, 0 },
    { 67, SERENITY_KEY_F9, 0, 0 },
    { 68, SERENITY_KEY_F10, 0, 0 },
    { 87, SERENITY_KEY_F11, 0, 0 },
    { 88, SERENITY_KEY_F12, 0, 0 },
};

/* QWERTY letter rows: evdev numbers are contiguous within each row. */
static const struct serenity_keymap_entry serenity_keymap_row_qwerty[] = {
    { 16, SERENITY_KEY_Q, 'q', 'Q' }, { 17, SERENITY_KEY_W, 'w', 'W' },
    { 18, SERENITY_KEY_E, 'e', 'E' }, { 19, SERENITY_KEY_R, 'r', 'R' },
    { 20, SERENITY_KEY_T, 't', 'T' }, { 21, SERENITY_KEY_Y, 'y', 'Y' },
    { 22, SERENITY_KEY_U, 'u', 'U' }, { 23, SERENITY_KEY_I, 'i', 'I' },
    { 24, SERENITY_KEY_O, 'o', 'O' }, { 25, SERENITY_KEY_P, 'p', 'P' },
};

static const struct serenity_keymap_entry serenity_keymap_row_asdf[] = {
    { 30, SERENITY_KEY_A, 'a', 'A' }, { 31, SERENITY_KEY_S, 's', 'S' },
    { 32, SERENITY_KEY_D, 'd', 'D' }, { 33, SERENITY_KEY_F, 'f', 'F' },
    { 34, SERENITY_KEY_G, 'g', 'G' }, { 35, SERENITY_KEY_H, 'h', 'H' },
    { 36, SERENITY_KEY_J, 'j', 'J' }, { 37, SERENITY_KEY_K, 'k', 'K' },
    { 38, SERENITY_KEY_L, 'l', 'L' },
};

static const struct serenity_keymap_entry serenity_keymap_row_zxcv[] = {
    { 44, SERENITY_KEY_Z, 'z', 'Z' }, { 45, SERENITY_KEY_X, 'x', 'X' },
    { 46, SERENITY_KEY_C, 'c', 'C' }, { 47, SERENITY_KEY_V, 'v', 'V' },
    { 48, SERENITY_KEY_B, 'b', 'B' }, { 49, SERENITY_KEY_N, 'n', 'N' },
    { 50, SERENITY_KEY_M, 'm', 'M' },
};

static inline const struct serenity_keymap_entry* serenity_keymap_lookup(unsigned evdev)
{
    unsigned i;
    if (evdev >= 16 && evdev <= 25)
        return &serenity_keymap_row_qwerty[evdev - 16];
    if (evdev >= 30 && evdev <= 38)
        return &serenity_keymap_row_asdf[evdev - 30];
    if (evdev >= 44 && evdev <= 50)
        return &serenity_keymap_row_zxcv[evdev - 44];
    for (i = 0; i < sizeof(serenity_keymap_simple) / sizeof(serenity_keymap_simple[0]); ++i) {
        if (serenity_keymap_simple[i].evdev == evdev)
            return &serenity_keymap_simple[i];
    }
    return 0;
}
