/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Client side of the input method bridge. The wire protocol is documented in
// Ports/LinuxGUI/IMEFrontend/serenityime.cpp (the fcitx5 addon that serves it):
//
//   client -> engine           engine -> client
//     create                     commit <hex>
//     cap <flags>                delete <offset> <size>
//     focus <0|1>                forward <keysym> <states> <release>
//     cursor <x> <y> <w> <h>     panel <cursor> <aux> <preedit> <sel> <n> [...]
//     surround <cursor> <anchor> <hex>   (full surrounding text, char offsets)
//     key <keysym> <states> <release>   ->  handled <0|1>
//     reset
//     destroy
//
// Strings are hex encoded so the protocol stays line oriented. A `key` command
// may be answered with any number of commit/panel lines before the final
// `handled` line, so read_until_handled() drains all of them.

#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibGUI/Event.h>
#include <LibGUI/InputMethod.h>
#include <LibGUI/InputMethodCandidateWindow.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Window.h>
#include <LibGfx/Rect.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace GUI {

namespace {

char const* socket_path()
{
    if (auto* from_environment = getenv("SERENITY_IME_SOCKET"))
        return from_environment;
    return "/tmp/serenity-ime.sock";
}

// X11 keysym for keys that have no printable code point.
u32 key_code_to_keysym(KeyCode key)
{
    switch (key) {
    case KeyCode::Key_Escape:
        return 0xff1b;
    case KeyCode::Key_Tab:
        return 0xff09;
    case KeyCode::Key_Backspace:
        return 0xff08;
    case KeyCode::Key_Return:
        return 0xff0d;
    case KeyCode::Key_Insert:
        return 0xff63;
    case KeyCode::Key_Delete:
        return 0xffff;
    case KeyCode::Key_Home:
        return 0xff50;
    case KeyCode::Key_End:
        return 0xff57;
    case KeyCode::Key_Left:
        return 0xff51;
    case KeyCode::Key_Up:
        return 0xff52;
    case KeyCode::Key_Right:
        return 0xff53;
    case KeyCode::Key_Down:
        return 0xff54;
    case KeyCode::Key_PageUp:
        return 0xff55;
    case KeyCode::Key_PageDown:
        return 0xff56;
    case KeyCode::Key_Space:
        return 0x20;
    case KeyCode::Key_LeftShift:
        return 0xffe1;
    case KeyCode::Key_RightShift:
        return 0xffe2;
    case KeyCode::Key_LeftControl:
        return 0xffe3;
    case KeyCode::Key_RightControl:
        return 0xffe4;
    case KeyCode::Key_LeftAlt:
        return 0xffe9;
    case KeyCode::Key_RightAlt:
        return 0xffea;
    case KeyCode::Key_LeftSuper:
        return 0xffeb;
    case KeyCode::Key_RightSuper:
        return 0xffec;
    case KeyCode::Key_CapsLock:
        return 0xffe5;
    case KeyCode::Key_NumLock:
        return 0xff7f;
    case KeyCode::Key_ScrollLock:
        return 0xff14;
    default:
        break;
    }

    // X11 keysyms in 0x20..0x7e are the ASCII code points.
    if (key >= KeyCode::Key_0 && key <= KeyCode::Key_9)
        return static_cast<u32>('0' + (key - KeyCode::Key_0));
    if (key >= KeyCode::Key_A && key <= KeyCode::Key_Z)
        return static_cast<u32>('a' + (key - KeyCode::Key_A));

    switch (key) {
    case KeyCode::Key_Comma:
        return ',';
    case KeyCode::Key_Minus:
        return '-';
    case KeyCode::Key_Period:
        return '.';
    case KeyCode::Key_Slash:
        return '/';
    case KeyCode::Key_Semicolon:
        return ';';
    case KeyCode::Key_Equal:
        return '=';
    case KeyCode::Key_LeftBracket:
        return '[';
    case KeyCode::Key_RightBracket:
        return ']';
    case KeyCode::Key_Backslash:
        return '\\';
    case KeyCode::Key_Backtick:
        return '`';
    case KeyCode::Key_Apostrophe:
        return '\'';
    default:
        break;
    }

    return 0;
}

// Prefer the produced code point: fcitx normalises keysyms, so passing 'A'
// with no shift state is the same as 'a' with shift. Only fall back to the
// physical key when a modifier means the code point is not the character.
u32 event_to_keysym(KeyEvent const& event)
{
    if (!event.ctrl() && !event.alt()) {
        auto code_point = event.code_point();
        if (code_point >= 0x20 && code_point < 0x7f)
            return code_point;
        if (code_point >= 0x80)
            return 0x01000000u | code_point;
    }
    return key_code_to_keysym(event.key());
}

// Reverse of key_code_to_keysym, used when replaying a forwarded key. `code_point`
// is what the editor should insert; the key code is only needed for shortcuts
// and editing-engine commands.
KeyCode keysym_to_key_code(u32 keysym, u32& code_point)
{
    code_point = 0;

    if (keysym >= 0x01000000) {
        code_point = keysym & 0x00ffffff;
        return KeyCode::Key_Invalid;
    }

    if (keysym >= 0x20 && keysym < 0x7f) {
        code_point = keysym;
        if (keysym >= 'a' && keysym <= 'z')
            return static_cast<KeyCode>(KeyCode::Key_A + (keysym - 'a'));
        if (keysym >= 'A' && keysym <= 'Z')
            return static_cast<KeyCode>(KeyCode::Key_A + (keysym - 'A'));
        if (keysym >= '0' && keysym <= '9')
            return static_cast<KeyCode>(KeyCode::Key_0 + (keysym - '0'));

        switch (keysym) {
        case ' ':
            return KeyCode::Key_Space;
        case '!':
            return KeyCode::Key_ExclamationPoint;
        case '"':
            return KeyCode::Key_DoubleQuote;
        case '#':
            return KeyCode::Key_Hashtag;
        case '$':
            return KeyCode::Key_Dollar;
        case '%':
            return KeyCode::Key_Percent;
        case '&':
            return KeyCode::Key_Ampersand;
        case '\'':
            return KeyCode::Key_Apostrophe;
        case '(':
            return KeyCode::Key_LeftParen;
        case ')':
            return KeyCode::Key_RightParen;
        case '*':
            return KeyCode::Key_Asterisk;
        case '+':
            return KeyCode::Key_Plus;
        case ',':
            return KeyCode::Key_Comma;
        case '-':
            return KeyCode::Key_Minus;
        case '.':
            return KeyCode::Key_Period;
        case '/':
            return KeyCode::Key_Slash;
        case ':':
            return KeyCode::Key_Colon;
        case ';':
            return KeyCode::Key_Semicolon;
        case '<':
            return KeyCode::Key_LessThan;
        case '=':
            return KeyCode::Key_Equal;
        case '>':
            return KeyCode::Key_GreaterThan;
        case '?':
            return KeyCode::Key_QuestionMark;
        case '@':
            return KeyCode::Key_AtSign;
        case '[':
            return KeyCode::Key_LeftBracket;
        case '\\':
            return KeyCode::Key_Backslash;
        case ']':
            return KeyCode::Key_RightBracket;
        case '^':
            return KeyCode::Key_Circumflex;
        case '_':
            return KeyCode::Key_Underscore;
        case '`':
            return KeyCode::Key_Backtick;
        case '{':
            return KeyCode::Key_LeftBrace;
        case '|':
            return KeyCode::Key_Pipe;
        case '}':
            return KeyCode::Key_RightBrace;
        case '~':
            return KeyCode::Key_Tilde;
        default:
            return KeyCode::Key_Invalid;
        }
    }

    switch (keysym) {
    case 0xff1b:
        return KeyCode::Key_Escape;
    case 0xff09:
        return KeyCode::Key_Tab;
    case 0xff08:
        return KeyCode::Key_Backspace;
    case 0xff0d:
        return KeyCode::Key_Return;
    case 0xff63:
        return KeyCode::Key_Insert;
    case 0xffff:
        return KeyCode::Key_Delete;
    case 0xff50:
        return KeyCode::Key_Home;
    case 0xff57:
        return KeyCode::Key_End;
    case 0xff51:
        return KeyCode::Key_Left;
    case 0xff52:
        return KeyCode::Key_Up;
    case 0xff53:
        return KeyCode::Key_Right;
    case 0xff54:
        return KeyCode::Key_Down;
    case 0xff55:
        return KeyCode::Key_PageUp;
    case 0xff56:
        return KeyCode::Key_PageDown;
    case 0xffe1:
        return KeyCode::Key_LeftShift;
    case 0xffe2:
        return KeyCode::Key_RightShift;
    case 0xffe3:
        return KeyCode::Key_LeftControl;
    case 0xffe4:
        return KeyCode::Key_RightControl;
    case 0xffe9:
        return KeyCode::Key_LeftAlt;
    case 0xffea:
        return KeyCode::Key_RightAlt;
    case 0xffeb:
        return KeyCode::Key_LeftSuper;
    case 0xffec:
        return KeyCode::Key_RightSuper;
    case 0xffe5:
        return KeyCode::Key_CapsLock;
    case 0xff7f:
        return KeyCode::Key_NumLock;
    case 0xff14:
        return KeyCode::Key_ScrollLock;
    default:
        break;
    }

    if (keysym >= 0xffbe && keysym <= 0xffc9)
        return static_cast<KeyCode>(KeyCode::Key_F1 + (keysym - 0xffbe));

    return KeyCode::Key_Invalid;
}

// fcitx5 KeyState uses the X11 modifier bit layout.
unsigned modifiers_to_states(u8 modifiers)
{
    unsigned states = 0;
    if (modifiers & Mod_Shift)
        states |= 1u << 0;
    if (modifiers & Mod_Ctrl)
        states |= 1u << 2;
    if (modifiers & Mod_Alt)
        states |= 1u << 3;
    if (modifiers & Mod_Super)
        states |= 1u << 6;
    if (modifiers & Mod_AltGr)
        states |= 1u << 7;
    return states;
}

u8 states_to_modifiers(unsigned states)
{
    u8 modifiers = 0;
    if (states & (1u << 0))
        modifiers |= Mod_Shift;
    if (states & (1u << 2))
        modifiers |= Mod_Ctrl;
    if (states & (1u << 3))
        modifiers |= Mod_Alt;
    if (states & (1u << 6))
        modifiers |= Mod_Super;
    if (states & (1u << 7))
        modifiers |= Mod_AltGr;
    return modifiers;
}

String decode_hex_string(StringView hex)
{
    if (hex.is_empty() || hex.length() % 2 != 0)
        return String {};

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    Vector<u8> bytes;
    bytes.ensure_capacity(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        auto high = nibble(hex[i]);
        auto low = nibble(hex[i + 1]);
        if (high < 0 || low < 0)
            return String {};
        bytes.append(static_cast<u8>((high << 4) | low));
    }

    return String::from_utf8_without_validation(ReadonlyBytes { bytes.data(), bytes.size() });
}

// Splits off one space-delimited token, preserving empty tokens (the preedit
// field is empty whenever there is no composition).
StringView take_token(StringView& input)
{
    auto space = input.find(' ');
    if (!space.has_value()) {
        auto token = input;
        input = {};
        return token;
    }
    auto token = input.substring_view(0, *space);
    input = input.substring_view(*space + 1);
    return token;
}

// --- surrounding-text geometry -------------------------------------------
//
// fcitx5 talks about surrounding text and deletions in UCS4 characters (code
// points), while TextPosition columns are also counted in code points. So the
// offsets the engine sends can be applied directly to the buffer as long as we
// count the same way here: every code point is one, and a newline is one.

int utf8_byte_length(u8 lead)
{
    if ((lead & 0x80) == 0)
        return 1;
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

size_t utf8_code_point_count(StringView view)
{
    size_t count = 0;
    auto bytes = view.bytes();
    auto length = view.length();
    for (size_t i = 0; i < length; i += static_cast<size_t>(utf8_byte_length(bytes[i])))
        ++count;
    return count;
}

// Absolute code-point index (newline == one) -> TextPosition.
GUI::TextPosition char_index_to_position(StringView text, size_t target)
{
    size_t line = 0;
    size_t column = 0;
    size_t seen = 0;
    auto bytes = text.bytes();
    auto length = text.length();
    for (size_t i = 0; i < length;) {
        if (seen == target)
            return { line, column };
        if (bytes[i] == '\n') {
            ++line;
            column = 0;
            ++seen;
            ++i;
            continue;
        }
        i += static_cast<size_t>(utf8_byte_length(bytes[i]));
        ++column;
        ++seen;
    }
    return { line, column };
}

// TextPosition -> absolute code-point index (newline == one).
size_t position_to_char_index(StringView text, GUI::TextPosition const& position)
{
    size_t line = 0;
    size_t column = 0;
    size_t seen = 0;
    auto bytes = text.bytes();
    auto length = text.length();
    for (size_t i = 0; i < length;) {
        if (line == position.line() && column == position.column())
            return seen;
        if (bytes[i] == '\n') {
            ++line;
            column = 0;
            ++seen;
            ++i;
            continue;
        }
        i += static_cast<size_t>(utf8_byte_length(bytes[i]));
        ++column;
        ++seen;
    }
    return seen;
}

// Absolute code-point index -> byte offset, used to slice a surrounding-text
// window without splitting a UTF-8 sequence.
size_t byte_offset_for_char_index(StringView text, size_t target)
{
    auto bytes = text.bytes();
    auto length = text.length();
    size_t seen = 0;
    size_t i = 0;
    while (i < length && seen < target) {
        i += static_cast<size_t>(utf8_byte_length(bytes[i]));
        ++seen;
    }
    return i;
}

// Hex encode raw bytes; the addon decodes with the matching hex_decode().
ByteString hex_encode(StringView input)
{
    static char const* digits = "0123456789abcdef";
    StringBuilder builder;
    for (auto byte : input) {
        auto value = static_cast<u8>(byte);
        builder.append(static_cast<char>(digits[value >> 4]));
        builder.append(static_cast<char>(digits[value & 0xf]));
    }
    return builder.to_byte_string();
}

}

InputMethod& InputMethod::the()
{
    // Leaked on purpose: the singleton is reached from widget destructors while
    // the GUI connection is being torn down, and a static destructor here would
    // then talk to a closed socket.
    static auto* instance = new InputMethod;
    return *instance;
}

InputMethod::~InputMethod()
{
    disconnect();
}

bool InputMethod::is_available()
{
    return access(socket_path(), F_OK) == 0;
}

bool InputMethod::connect_if_needed()
{
    if (m_fd >= 0)
        return true;

    auto path = socket_path();
    auto path_length = strlen(path);
    if (path_length >= sizeof(sockaddr_un::sun_path))
        return false;

    auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_length);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return false;
    }

    m_fd = fd;
    m_read_buffer = {};
    m_saw_handled = false;
    m_handled_value = false;
    m_forwarded_keys.clear();

    send_line("create"sv);
    send_line("cap 0"sv);
    return true;
}

void InputMethod::disconnect()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_read_buffer = {};
    m_saw_handled = false;
    m_handled_value = false;
    m_forwarded_keys.clear();

    // A dropped connection also drops any composition in flight.
    m_panel_visible = false;
    hide_panel_window();
}

void InputMethod::send_line(StringView line)
{
    if (m_fd < 0)
        return;

    StringBuilder builder;
    builder.append(line);
    builder.append('\n');
    auto payload = builder.to_byte_string();

    size_t written = 0;
    while (written < payload.length()) {
        // MSG_NOSIGNAL: the engine only serves one client, so it may close this
        // socket when another app takes focus. A plain write() would then raise
        // SIGPIPE and kill the whole application.
        auto result = ::send(m_fd, payload.characters() + written, payload.length() - written, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            disconnect();
            return;
        }
        written += static_cast<size_t>(result);
    }
}

bool InputMethod::handle_key_event(KeyEvent& event)
{
    // A key we synthesize for a `forward` request must not be offered back to
    // the engine: it already decided the application should get it.
    if (m_replaying_forwarded_key)
        return false;

    if (m_fd < 0) {
        if (!connect_if_needed())
            return false;
        // Reconnected after a timeout: restore focus and caret position, or the
        // engine would treat this key as belonging to no input context.
        send_line("focus 1"sv);
        if (auto editor = m_editor.strong_ref()) {
            update_cursor_rect(*editor);
            send_surrounding_text();
        }
    }

    auto keysym = event_to_keysym(event);
    if (keysym == 0)
        return false;

    auto states = modifiers_to_states(event.modifiers());
    auto is_release = event.type() == Event::Type::KeyUp;

    auto line = String::formatted("key {} {} {}", keysym, states, is_release ? 1 : 0);
    if (line.is_error())
        return false;

    send_line(line.value().bytes_as_string_view());
    auto handled = read_until_handled();
    replay_forwarded_keys(handled);
    // Keep the engine's cache of the surrounding text in sync so that a later
    // deleteSurroundingText() has a valid context to clamp against. This runs
    // after every key, whether or not the engine consumed it.
    send_surrounding_text();
    return handled;
}

bool InputMethod::handle_key_up_event(KeyEvent& event)
{
    if (m_replaying_forwarded_key || m_fd < 0)
        return false;

    auto keysym = event_to_keysym(event);
    if (keysym == 0)
        return false;

    auto line = String::formatted("key {} {} 1", keysym, modifiers_to_states(event.modifiers()));
    if (line.is_error())
        return false;

    send_line(line.value().bytes_as_string_view());
    auto handled = read_until_handled();
    replay_forwarded_keys(handled);
    return handled;
}

void InputMethod::replay_forwarded_keys(bool original_was_handled)
{
    if (m_forwarded_keys.is_empty())
        return;

    auto forwarded_keys = move(m_forwarded_keys);
    m_forwarded_keys.clear();

    // If the engine did not consume the original key, the editor will process
    // that event normally. Replaying now would insert or execute it twice.
    if (!original_was_handled)
        return;

    auto editor = m_editor.strong_ref();
    if (!editor)
        return;

    m_replaying_forwarded_key = true;
    for (auto const& forwarded : forwarded_keys) {
        u32 code_point = 0;
        auto key = keysym_to_key_code(forwarded.keysym, code_point);
        KeyEvent event {
            forwarded.release ? Event::Type::KeyUp : Event::Type::KeyDown,
            key, 0, states_to_modifiers(forwarded.states), code_point, 0
        };

        if (forwarded.release)
            editor->keyup_event(event);
        else
            editor->keydown_event(event);
    }
    m_replaying_forwarded_key = false;
}

bool InputMethod::read_until_handled()
{
    m_saw_handled = false;
    m_handled_value = false;

    for (;;) {
        if (consume_buffered_lines())
            return m_handled_value;
        if (m_fd < 0)
            return false;

        struct pollfd descriptor {
            m_fd, POLLIN, 0
        };
        auto result = ::poll(&descriptor, 1, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            disconnect();
            return false;
        }
        if (result == 0) {
            // No reply in time. Drop the connection instead of letting a late
            // `handled` line be read as the next key's answer.
            disconnect();
            return false;
        }

        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            disconnect();
            return false;
        }

        char buffer[4096];
        auto count = ::read(m_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            disconnect();
            return false;
        }
        if (count == 0) {
            disconnect();
            return false;
        }

        StringBuilder builder;
        builder.append(m_read_buffer);
        builder.append(buffer, static_cast<size_t>(count));
        m_read_buffer = builder.to_byte_string();
    }
}

bool InputMethod::consume_buffered_lines()
{
    for (;;) {
        auto newline = m_read_buffer.find('\n');
        if (!newline.has_value())
            return false;

        auto line = m_read_buffer.substring(0, *newline);
        m_read_buffer = m_read_buffer.substring(*newline + 1);
        handle_line(line);
        if (m_saw_handled)
            return true;
    }
}

void InputMethod::handle_line(StringView line)
{
    if (line.starts_with("handled "sv)) {
        m_saw_handled = true;
        m_handled_value = line.substring_view(8) == "1"sv;
        return;
    }

    if (line.starts_with("commit "sv)) {
        auto text = decode_hex_string(line.substring_view(7));
        if (auto editor = m_editor.strong_ref(); editor && editor->is_editable()) {
            editor->insert_at_cursor_or_replace_selection(text);
            editor->set_preedit(String {}, 0);
        }
        return;
    }

    if (line.starts_with("panel "sv)) {
        handle_panel(line.substring_view(6));
        return;
    }

    if (line.starts_with("delete "sv)) {
        auto rest = line.substring_view(7);
        auto offset = take_token(rest).to_number<int>().value_or(0);
        auto size = take_token(rest).to_number<int>().value_or(0);
        handle_delete(offset, size);
        return;
    }

    if (line.starts_with("forward "sv)) {
        auto rest = line.substring_view(8);
        ForwardedKey forwarded;
        forwarded.keysym = take_token(rest).to_number<u32>().value_or(0);
        forwarded.states = take_token(rest).to_number<u32>().value_or(0);
        forwarded.release = take_token(rest).to_number<u32>().value_or(0) != 0;
        if (forwarded.keysym != 0)
            m_forwarded_keys.append(forwarded);
        return;
    }

    // "ok" (reply to create) and "active" need no action here. A key the
    // engine does not consume is answered with `handled 0`, and the editor then
    // handles it itself; `forward` is collected above and replayed after the
    // roundtrip.
}

void InputMethod::handle_panel(StringView rest)
{
    auto cursor_token = take_token(rest);
    auto aux_token = take_token(rest);
    auto preedit_token = take_token(rest);
    auto candidate_cursor_token = take_token(rest);
    auto count_token = take_token(rest);

    auto cursor = cursor_token.to_number<int>().value_or(0);
    auto candidate_cursor = candidate_cursor_token.to_number<int>().value_or(-1);
    auto count = count_token.to_number<int>().value_or(0);

    m_preedit = decode_hex_string(preedit_token);
    m_aux_up = decode_hex_string(aux_token);
    m_preedit_cursor = cursor;
    m_candidate_cursor = candidate_cursor;

    m_candidate_labels.clear();
    m_candidate_texts.clear();

    for (int i = 0; i < count; ++i) {
        auto label = decode_hex_string(take_token(rest));
        auto text = decode_hex_string(take_token(rest));
        m_candidate_labels.append(move(label));
        m_candidate_texts.append(move(text));
    }

    // The engine may append two trailing flags (has-prev, has-next) after the
    // candidate list. Older addons omit them; default to "no extra pages".
    m_has_prev = false;
    m_has_next = false;
    if (!rest.is_empty()) {
        m_has_prev = take_token(rest).to_number<int>().value_or(0) != 0;
        m_has_next = take_token(rest).to_number<int>().value_or(0) != 0;
    }

    m_panel_visible = !m_preedit.is_empty() || !m_candidate_texts.is_empty();
    update_panel_window();
}

// Applies an engine-initiated deletion to the editor. fcitx5 documents
// `offset`/`size` in UCS4 characters relative to the cursor (negative offset
// means "before the cursor"), which is the IBus convention. We resolve them
// against the editor's real buffer and cursor, then hand a plain TextRange to
// the editor so the deletion joins the normal undo stack.
void InputMethod::handle_delete(int offset, int size)
{
    if (size <= 0)
        return;

    auto editor = m_editor.strong_ref();
    if (!editor || !editor->is_editable())
        return;

    auto const text_storage = editor->text();
    auto const text = text_storage.view();
    auto const total = utf8_code_point_count(text);
    auto const cursor_index = position_to_char_index(text, editor->cursor());

    long long start_index = static_cast<long long>(cursor_index) + offset;
    if (start_index < 0)
        start_index = 0;
    long long end_index = start_index + static_cast<long long>(size);
    if (end_index > static_cast<long long>(total))
        end_index = static_cast<long long>(total);
    if (end_index <= start_index)
        return;

    auto const start = char_index_to_position(text, static_cast<size_t>(start_index));
    auto const end = char_index_to_position(text, static_cast<size_t>(end_index));
    editor->delete_text_range(TextRange { start, end });
}

// Uploads the buffer and the caret/anchor as character offsets. fcitx5
// needs this to be present for deleteSurroundingText() to be accepted at all:
// with an empty surrounding text the engine's deletion is clamped away before
// it ever reaches us.
//
// Only a window around the cursor/anchor is sent. Engines use the surrounding
// text to clamp deletions and to know what a commit would replace, so the
// window has to include the whole selection, but there is no reason to send a
// megabyte document after every keystroke.
void InputMethod::send_surrounding_text()
{
    auto editor = m_editor.strong_ref();
    if (!editor || m_fd < 0)
        return;

    auto const text_storage = editor->text();
    auto const text = text_storage.view();
    auto const total = utf8_code_point_count(text);
    auto const cursor_index = position_to_char_index(text, editor->cursor());

    auto anchor_index = cursor_index;
    if (editor->has_selection()) {
        auto const selection = editor->normalized_selection();
        auto const selection_start = position_to_char_index(text, selection.start());
        auto const selection_end = position_to_char_index(text, selection.end());
        // The caret sits at one end of the selection; the anchor is the other.
        if (cursor_index == selection_start)
            anchor_index = selection_end;
        else if (cursor_index == selection_end)
            anchor_index = selection_start;
        else
            anchor_index = cursor_index < selection_start ? selection_end : selection_start;
    }

    constexpr size_t window_size = 512;
    auto window_start = size_t { 0 };
    auto window_end = total;
    if (total > window_size * 2) {
        auto const first = min(cursor_index, anchor_index);
        auto const last = max(cursor_index, anchor_index);
        window_start = first > window_size ? first - window_size : 0;
        window_end = min(total, last + window_size);
        // A pathological selection larger than the window would be clipped;
        // keep it intact because it describes what a commit replaces.
        if (window_end < last)
            window_end = total;
    }

    auto const byte_start = byte_offset_for_char_index(text, window_start);
    auto const byte_end = byte_offset_for_char_index(text, window_end);
    auto const window = text.substring_view(byte_start, byte_end - byte_start);

    StringBuilder builder;
    builder.append("surround "sv);
    builder.appendff("{} {} ", cursor_index - window_start, anchor_index - window_start);
    auto const hex = hex_encode(window);
    builder.append(hex.view());
    auto const line = builder.to_byte_string();
    send_line(line.view());
}

void InputMethod::focus_in(TextEditor& editor)
{
    // A different widget may live in a different window, so drop a popup that
    // was built for the previous one and rebuild it lazily.
    if (m_editor.ptr() != &editor)
        m_panel_window = nullptr;

    m_editor = editor;

    if (!connect_if_needed())
        return;

    send_line("focus 1"sv);
    update_cursor_rect(editor);
    send_surrounding_text();
}

void InputMethod::focus_out(TextEditor& editor)
{
    if (m_editor.ptr() != &editor)
        return;

    editor.set_preedit(String {}, 0);
    m_editor.clear();
    m_preedit = {};
    m_aux_up = {};
    m_candidate_labels.clear();
    m_candidate_texts.clear();
    m_has_prev = false;
    m_has_next = false;
    m_panel_visible = false;
    hide_panel_window();
    m_panel_window = nullptr;

    if (m_fd >= 0) {
        send_line("destroy"sv);
        disconnect();
    }
}

void InputMethod::update_cursor_rect(TextEditor& editor)
{
    if (m_fd < 0 || !editor.window())
        return;

    auto rect = editor.cursor_screen_rect();
    m_cursor_rect = rect;

    auto line = String::formatted("cursor {} {} {} {}", rect.x(), rect.y(), rect.width(), rect.height());
    if (line.is_error())
        return;

    send_line(line.value().bytes_as_string_view());

    if (m_panel_visible)
        update_panel_window();
}

void InputMethod::reset()
{
    if (m_fd >= 0)
        send_line("reset"sv);

    if (auto editor = m_editor.strong_ref())
        editor->set_preedit(String {}, 0);
    m_preedit = {};
    m_aux_up = {};
    m_candidate_labels.clear();
    m_candidate_texts.clear();
    m_has_prev = false;
    m_has_next = false;
    m_panel_visible = false;
    hide_panel_window();
}

void InputMethod::update_panel_window()
{
    auto editor = m_editor.strong_ref();
    if (!editor || !editor->window())
        return;

    // 内联预编辑：把拼音/组字串画在编辑器输入区光标处（而不是只画在候选窗第一行）。
    editor->set_preedit(m_preedit, m_preedit_cursor);

    if (!m_panel_visible) {
        hide_panel_window();
        return;
    }

    if (!m_panel_window)
        m_panel_window = make<InputMethodCandidateWindow>(*editor);

    m_panel_window->set_on_candidate_selected([this](int i) { select_candidate(i); });
    m_panel_window->set_on_page_requested([this](bool next) { request_page(next); });

    m_panel_window->update(m_preedit, m_preedit_cursor, m_candidate_labels, m_candidate_texts, m_candidate_cursor, editor->cursor_screen_rect(), m_aux_up, m_has_prev, m_has_next);
}

void InputMethod::hide_panel_window()
{
    if (m_panel_window)
        m_panel_window->hide();
}

void InputMethod::select_candidate(int index)
{
    if (m_fd < 0 || index < 0)
        return;

    auto line = String::formatted("select {}", index);
    if (line.is_error())
        return;

    send_line(line.value().bytes_as_string_view());
    // The engine answers with a fresh panel (or a commit) followed by `handled`;
    // drain them so the popup reflects the new state.
    read_until_handled();
}

void InputMethod::request_page(bool next)
{
    if (m_fd < 0)
        return;

    // Paging is just a PageUp / PageDown key offered to the engine, exactly the
    // same path as pressing the arrow keys: the addon forwards it to fcitx5,
    // which moves the candidate window and pushes a new panel.
    u32 const keysym = next ? 0xff56u : 0xff55u; // PageDown / PageUp
    auto press = String::formatted("key {} 0 0", keysym);
    auto release = String::formatted("key {} 0 1", keysym);
    if (press.is_error() || release.is_error())
        return;

    send_line(press.value().bytes_as_string_view());
    read_until_handled();
    send_line(release.value().bytes_as_string_view());
    read_until_handled();
}


u32 ime_keysym_for_key_event(KeyEvent const& event)
{
    return event_to_keysym(event);
}

KeyCode ime_key_code_for_keysym(u32 keysym, u32& code_point)
{
    return keysym_to_key_code(keysym, code_point);
}

}

