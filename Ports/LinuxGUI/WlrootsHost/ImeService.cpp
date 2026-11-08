/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "ImeService.h"

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <LibCore/System.h>
#include <LibGfx/Point.h>
#include <LibGUI/Application.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/InputMethodCandidateWindow.h>
#include <LibGUI/Window.h>

#include <errno.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ImeService {

namespace {

int s_fd { -1 };
ByteString s_read_buffer;
bool s_saw_handled { false };
bool s_handled_value { false };
bool s_focused { false };
Gfx::IntRect s_caret;
Panel s_panel;

RefPtr<GUI::Window> s_window;
GUI::InputMethodPanelWidget* s_widget { nullptr };

Function<void(String const&)> s_on_commit;
Function<void(int, int)> s_on_delete;
Function<void(unsigned, unsigned, bool)> s_on_forward_key;

char const* socket_path()
{
    if (auto const* from_env = getenv("SERENITY_IME_SOCKET"))
        return from_env;
    return "/tmp/serenity-ime.sock";
}

String decode_hex(StringView hex)
{
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    StringBuilder builder;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        auto high = nibble(hex[i]);
        auto low = nibble(hex[i + 1]);
        if (high < 0 || low < 0)
            break;
        builder.append(static_cast<char>((high << 4) | low));
    }
    return builder.to_string_without_validation();
}

StringView take_token(StringView& rest)
{
    auto space = rest.find(' ');
    if (!space.has_value()) {
        auto token = rest;
        rest = {};
        return token;
    }
    auto token = rest.substring_view(0, *space);
    rest = rest.substring_view(*space + 1);
    return token;
}

void ensure_window()
{
    if (s_window)
        return;
    auto window = GUI::Window::construct();
    /* 必须用 Autocomplete，不能用 Popup：WS 的 WindowManager 对 Popup 会调
     * notify_active_window_input_preempted() → 宿主窗口 focusout → 引擎 reset，
     * 组字刚起就丢。Autocomplete 和原生候选窗同类型，明确不抢焦点。 */
    window->set_window_type(GUI::WindowType::Autocomplete);
    window->set_frameless(true);
    window->set_has_alpha_channel(true);
    window->set_title("Input Method");
    s_widget = window->set_main_widget<GUI::InputMethodPanelWidget>();
    s_widget->set_fill_with_background_color(true);
    s_widget->set_focus_policy(GUI::FocusPolicy::NoFocus);
    s_widget->on_candidate_clicked = [](size_t index) {
        select_candidate(static_cast<int>(index));
    };
    s_widget->on_pager_clicked = [](bool next) {
        request_page(next);
    };
    s_window = window;
}

void position_window()
{
    if (!s_window || !s_widget)
        return;
    auto size = s_widget->preferred_size();
    if (size.width() <= 0 || size.height() <= 0)
        return;

    auto pivot = s_caret;
    int x = pivot.x();
    int y = pivot.y() + (pivot.height() > 0 ? pivot.height() : 0) + 2;

    /* 贴边时翻到光标上方 / 夹在屏幕内（和原生候选窗同一套规则）。 */
    auto screen = GUI::Desktop::the().rect();
    if (y + size.height() > screen.bottom())
        y = pivot.y() - size.height() - 2;
    if (y < screen.top())
        y = screen.top();
    if (x + size.width() > screen.right())
        x = screen.right() - size.width();
    if (x < screen.left())
        x = screen.left();

    s_window->set_rect({ x, y, size.width(), size.height() });
    if (getenv("SERENITY_IME_DEBUG"))
        fprintf(stderr, "ime: panel caret=%d,%d %dx%d -> %d,%d %dx%d\n",
            pivot.x(), pivot.y(), pivot.width(), pivot.height(),
            x, y, size.width(), size.height());
}

void hide_window()
{
    if (s_window && s_window->is_visible())
        s_window->hide();
}

void update_window()
{
    /* 隐藏候选窗用 SERENITY_IME_PANEL=0；默认显示（Serenity 外观，跟随主题）。 */
    if (getenv("SERENITY_IME_PANEL") && !strcmp(getenv("SERENITY_IME_PANEL"), "0"))
        return;
    bool empty = s_panel.preedit.is_empty() && s_panel.texts.is_empty();
    if (empty) {
        hide_window();
        return;
    }
    ensure_window();
    s_widget->set_content(s_panel.preedit, s_panel.preedit_cursor,
        s_panel.labels, s_panel.texts, s_panel.cursor_index,
        s_panel.aux, s_panel.has_prev, s_panel.has_next);
    position_window();
    if (!s_window->is_visible())
        s_window->show();
    /* 不要 move_to_front()：Autocomplete 已经由 WS 置顶，额外请求会打扰焦点。 */
}

void handle_panel(StringView rest)
{
    auto cursor = take_token(rest).to_number<int>().value_or(0);
    auto aux = decode_hex(take_token(rest));
    auto preedit = decode_hex(take_token(rest));
    auto candidate_cursor = take_token(rest).to_number<int>().value_or(-1);
    auto count = take_token(rest).to_number<int>().value_or(0);

    s_panel.aux = move(aux);
    s_panel.preedit = move(preedit);
    s_panel.preedit_cursor = cursor;
    s_panel.cursor_index = candidate_cursor;
    s_panel.labels.clear();
    s_panel.texts.clear();

    for (int i = 0; i < count; ++i) {
        s_panel.labels.append(decode_hex(take_token(rest)));
        s_panel.texts.append(decode_hex(take_token(rest)));
    }
    /* 老格式没有这两个尾标志；缺省当作「不能翻页」。 */
    s_panel.has_prev = take_token(rest).to_number<int>().value_or(0) != 0;
    s_panel.has_next = take_token(rest).to_number<int>().value_or(0) != 0;

    update_window();
}

void handle_line(StringView line)
{
    if (line.starts_with("handled "sv)) {
        s_saw_handled = true;
        s_handled_value = line.substring_view(8) == "1"sv;
        return;
    }
    if (line.starts_with("commit "sv)) {
        auto text = decode_hex(line.substring_view(7));
        if (s_on_commit)
            s_on_commit(text);
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
        if (s_on_delete)
            s_on_delete(offset, size);
        return;
    }
    if (line.starts_with("forward "sv)) {
        auto rest = line.substring_view(8);
        auto keysym = take_token(rest).to_number<unsigned>().value_or(0);
        auto states = take_token(rest).to_number<unsigned>().value_or(0);
        auto release = take_token(rest).to_number<int>().value_or(0) != 0;
        if (keysym != 0 && s_on_forward_key)
            s_on_forward_key(keysym, states, release);
        return;
    }
    /* "ok" / "active" / "err …" 不需要动作。 */
}

/* 读到 handled 为止（一次按键的回复可能是多行 panel + 最后一行 handled）。 */
bool read_until_handled()
{
    for (;;) {
        for (;;) {
            auto newline = s_read_buffer.find('\n');
            if (!newline.has_value())
                break;
            auto line = s_read_buffer.substring(0, *newline);
            s_read_buffer = s_read_buffer.substring(*newline + 1);
            handle_line(line);
            if (s_saw_handled)
                return s_handled_value;
        }

        pollfd pfd { s_fd, POLLIN, 0 };
        if (::poll(&pfd, 1, 200) <= 0) {
            fprintf(stderr, "ime: engine timeout (no reply to key)\n");
            return false;
        }
        char buffer[4096];
        auto count = ::read(s_fd, buffer, sizeof(buffer));
        if (count <= 0) {
            /* 引擎没了：断开，别把 app 卡死。 */
            ::close(s_fd);
            s_fd = -1;
            s_focused = false;
            hide_window();
            return false;
        }
        s_read_buffer = ByteString::formatted("{}{}", s_read_buffer.view(),
            StringView { buffer, static_cast<size_t>(count) });
    }
}

void send_line(StringView line)
{
    if (s_fd < 0)
        return;
    auto payload = ByteString::formatted("{}\n", line);
    size_t written = 0;
    while (written < static_cast<size_t>(payload.length())) {
        auto result = ::send(s_fd, payload.characters() + written, payload.length() - written, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        written += static_cast<size_t>(result);
    }
}

bool connect_if_needed()
{
    if (s_fd >= 0)
        return true;

    auto socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0)
        return false;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    auto const* path = socket_path();
    if (strlen(path) >= sizeof(address.sun_path)) {
        ::close(socket);
        return false;
    }
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket);
        fprintf(stderr, "ime: connect(%s) failed: %s\n", path, strerror(errno));
        return false;
    }
    fprintf(stderr, "ime: engine connected on %s\n", path);
    s_fd = socket;
    s_read_buffer = {};
    send_line("create"sv);
    send_line("cap 0"sv);
    return true;
}

}

bool is_available()
{
    if (s_fd >= 0)
        return true;
    return access(socket_path(), F_OK) == 0;
}

void set_on_commit(Function<void(String const&)> handler) { s_on_commit = move(handler); }
void set_on_delete(Function<void(int, int)> handler) { s_on_delete = move(handler); }
void set_on_forward_key(Function<void(unsigned, unsigned, bool)> handler) { s_on_forward_key = move(handler); }

void focus_in(Gfx::IntRect const& caret_screen_rect)
{
    s_caret = caret_screen_rect;
    if (!connect_if_needed())
        return;
    s_focused = true;
    fprintf(stderr, "ime: focus_in caret=%d,%d %dx%d\n", caret_screen_rect.x(),
        caret_screen_rect.y(), caret_screen_rect.width(), caret_screen_rect.height());
    send_line("focus 1"sv);
    /* 桥没有文本 buffer，surrounding 只能给空串；引擎据此拒绝
     * deleteSurroundingText，这是可接受的（拼音不需要它）。 */
    send_line("surround 0 0 "sv);
    update_caret(caret_screen_rect);
}

void focus_out()
{
    fprintf(stderr, "ime: focus_out (fd=%d focused=%d)\n", s_fd, s_focused);
    if (s_fd < 0) {
        s_focused = false;
        return;
    }
    s_focused = false;
    send_line("focus 0"sv);
    s_panel = {};
    hide_window();
}

void reset()
{
    if (s_fd >= 0)
        send_line("reset"sv);
    s_panel = {};
    hide_window();
}

void update_caret(Gfx::IntRect const& caret_screen_rect)
{
    s_caret = caret_screen_rect;
    if (s_fd < 0 || !s_focused)
        return;
    auto line = String::formatted("cursor {} {} {} {}", caret_screen_rect.x(), caret_screen_rect.y(),
        caret_screen_rect.width(), caret_screen_rect.height());
    if (!line.is_error())
        send_line(line.value().bytes_as_string_view());
    /* 窗口已经显示时，光标移动要跟着走。 */
    if (s_window && s_window->is_visible())
        position_window();
}

bool handle_key(unsigned keysym, unsigned states, bool release)
{
    /* 引擎重启（fcitx5 崩过/被 Xwayland 带走）后 socket 会消失；
     * 这里顺手重连，别等下一次 focus 才恢复。 */
    if (s_fd < 0 && s_focused)
        connect_if_needed();
    if (s_fd < 0 || !s_focused || keysym == 0) {
        fprintf(stderr, "ime: key SKIP fd=%d focused=%d keysym=%u\n", s_fd, s_focused, keysym);
        return false;
    }
    auto line = String::formatted("key {} {} {}", keysym, states, release ? 1 : 0);
    if (line.is_error())
        return false;
    send_line(line.value().bytes_as_string_view());
    s_saw_handled = false;
    auto handled = read_until_handled();
    fprintf(stderr, "ime: key %u states=%u release=%d -> %s\n", keysym, states,
        release ? 1 : 0, handled ? "handled" : "passed");
    return handled;
}

void select_candidate(int index)
{
    if (s_fd < 0)
        return;
    auto line = String::formatted("select {}", index);
    if (!line.is_error())
        send_line(line.value().bytes_as_string_view());
}

void request_page(bool next)
{
    if (s_fd < 0)
        return;
    auto keysym = next ? 0xff56u : 0xff55u; /* XK_Page_Down / XK_Page_Up */
    auto press = String::formatted("key {} 0 0", keysym);
    auto release = String::formatted("key {} 0 1", keysym);
    if (!press.is_error())
        send_line(press.value().bytes_as_string_view());
    if (!release.is_error())
        send_line(release.value().bytes_as_string_view());
}

}
