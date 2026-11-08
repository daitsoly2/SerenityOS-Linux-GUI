/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGfx/Rect.h>

namespace GUI {

class KeyEvent;
class TextEditor;
class InputMethodCandidateWindow;

// Client side of the out-of-process input method bridge.
//
// The engine (fcitx5 plus the `serenityime` addon in
// Ports/LinuxGUI/IMEFrontend/) runs in its own process and listens on a unix
// socket. This class speaks that line protocol for a LibGUI process:
//
//   * every key pressed in an editable TextEditor is offered to the engine;
//   * text the engine commits is inserted at the cursor;
//   * the preedit string and the candidate list are drawn in a small popup.
//
// There is no glyph work here: the popup and the editor render with the
// process' normal font, so CJK only appears if that font actually has CJK
// coverage (see LINUX_GUI_PORT.md section 4.5).
//
// The engine is entirely optional. When no socket is reachable every method is
// a cheap no-op and typing behaves exactly as before.
class InputMethod {
public:
    static InputMethod& the();

    // Whether an engine socket could be reached. Only probes the filesystem;
    // it does not connect.
    static bool is_available();

    bool is_active() const { return m_fd >= 0; }

    // Called by TextEditor before it handles a key. Returns true when the input
    // method consumed the key (composition, candidate navigation, ...).
    bool handle_key_event(KeyEvent&);

    // Key releases are only interesting while an engine is connected (some
    // engines track modifier state, and a consumed key also needs its release
    // swallowed). Unlike handle_key_event this never opens a connection.
    bool handle_key_up_event(KeyEvent&);

    void focus_in(TextEditor&);
    void focus_out(TextEditor&);

    // Tells the engine where the caret is so it can place the panel.
    void update_cursor_rect(TextEditor&);

    // Drops any in-progress composition.
    void reset();

    // Candidate-window mouse interaction, driven by InputMethodCandidateWindow.
    // `select_candidate` commits the given 0-based candidate; `request_page`
    // pages the candidate list (true = next page, false = previous). Both are
    // cheap no-ops when no engine is connected.
    void select_candidate(int index);
    void request_page(bool next);

private:
    InputMethod() = default;
    ~InputMethod();

    AK_MAKE_NONCOPYABLE(InputMethod);
    AK_MAKE_NONMOVABLE(InputMethod);

    bool connect_if_needed();
    void disconnect();

    void send_line(StringView);
    bool read_until_handled();
    bool consume_buffered_lines();

    void handle_line(StringView);
    void handle_panel(StringView);
    void handle_delete(int offset, int size);

    // Pushes the buffer and caret to the engine so surrounding-text deletion
    // has a valid context. Cheap no-op when no engine is connected.
    void send_surrounding_text();

    // Replays keys the engine asked us to hand back to the editor. Only called
    // after the original key was consumed, otherwise the app would see it
    // twice.
    void replay_forwarded_keys(bool original_was_handled);

    struct ForwardedKey {
        u32 keysym { 0 };
        unsigned states { 0 };
        bool release { false };
    };

    void update_panel_window();
    void hide_panel_window();

    int m_fd { -1 };
    ByteString m_read_buffer;

    bool m_saw_handled { false };
    bool m_handled_value { false };

    // Set while replaying a `forward`ed key, so the synthetic key event does
    // not go back into the engine and loop forever.
    bool m_replaying_forwarded_key { false };
    Vector<ForwardedKey> m_forwarded_keys;

    WeakPtr<TextEditor> m_editor;

    OwnPtr<InputMethodCandidateWindow> m_panel_window;

    // Latest panel state, pushed by the engine.
    String m_preedit;
    String m_aux_up;
    int m_preedit_cursor { 0 };
    int m_candidate_cursor { -1 };
    Vector<String> m_candidate_labels;
    Vector<String> m_candidate_texts;
    bool m_has_prev { false };
    bool m_has_next { false };
    bool m_panel_visible { false };

    Gfx::IntRect m_cursor_rect;
};

// fcitx5 风格的 X11 keysym 换算。桥内建输入法（serenity-wlroots-host）自己没有
// TextEditor，但需要和原生应用**完全一致**的 key→keysym 映射，否则同一个键在
// GTK 应用和 Serenity 应用里会被引擎解释成不同的字符。
u32 ime_keysym_for_key_event(KeyEvent const&);
// keysym → KeyCode（用于引擎要求「把这个键还给应用」时反向还原）。
// 名字带 ime_ 前缀：InputMethod.cpp 内部已有一个同签名的静态辅助函数，
// 直接叫 keysym_to_key_code 会让那个文件里的调用点二义。
KeyCode ime_key_code_for_keysym(u32 keysym, u32& code_point);

}
