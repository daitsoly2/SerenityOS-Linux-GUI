/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A fcitx5 frontend addon that bridges input contexts to SerenityOS processes
// over a unix socket.
//
// Why an addon instead of the D-Bus client protocol: an addon links
// libFcitx5Core and therefore sees the *typed* InputPanel (preedit text,
// candidate list, cursor) rather than the serialized client-side-UI blob that
// D-Bus clients have to decode. It also keeps libdbus out of the Serenity
// side entirely - the Serenity end only needs a socket.
//
// Protocol (line based, UTF-8 strings hex encoded so no escaping is needed):
//
//   client -> addon
//     create                          -> "ok"
//     cap <flags>                     capability flags (bitmask, see CapabilityFlag)
//     focus <0|1>
//     cursor <x> <y> <w> <h>          cursor rect in client pixels
//     surround <cursor> <anchor> <hex> surrounding text, offsets in characters
//     key <keysym> <states> <release> -> "handled <0|1>"
//     select <index>                  select the 0-based candidate (mouse)
//     reset
//     destroy
//
//   addon -> client
//     commit <hex>
//     delete <offset> <size>
//     forward <keysym> <states> <release>
//     panel <cursor> <aux_up> <preedit> <cand_cursor> <n> [<label> <text>]*
//     active <0|1>                     input method on/off (client should draw state)

#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string hex_encode(std::string const& input)
{
    static char const* digits = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xf]);
    }
    return out;
}

std::string hex_decode(std::string const& input)
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

    std::string out;
    out.reserve(input.size() / 2);
    for (size_t i = 0; i + 1 < input.size(); i += 2) {
        auto high = nibble(input[i]);
        auto low = nibble(input[i + 1]);
        if (high < 0 || low < 0)
            return {};
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

char const* socket_path()
{
    if (auto const* from_env = getenv("SERENITY_IME_SOCKET"))
        return from_env;
    return "/tmp/serenity-ime.sock";
}

char const* status_dir()
{
    if (auto const* from_env = getenv("SERENITY_IME_STATUS_DIR"))
        return from_env;
    return "/tmp";
}

// Mirrors the client's IMEIndicator::is_keyboard_layout(): keyboard/layout
// input methods are direct Latin input, everything else is a CJK input method.
bool is_keyboard_layout(std::string const& name)
{
    if (name.rfind("keyboard-", 0) == 0)
        return true;
    return name == "english" || name == "kbd";
}

// Write a small status file atomically (temp + rename) so the indicator never
// reads a half-written line while the addon is rewriting it.
void write_file(std::string const& path, std::string const& content)
{
    auto tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    size_t offset = 0;
    while (offset < content.size()) {
        auto written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    ::close(fd);
    ::rename(tmp.c_str(), path.c_str());
}

}

class SerenityIME;
struct ImeClient;

// One Serenity window == one fcitx input context. The three *Impl overrides are
// exactly the points where fcitx hands results back to a frontend.
class SerenityInputContext final : public fcitx::InputContext {
public:
    SerenityInputContext(fcitx::InputContextManager& manager, std::string const& display, ImeClient& client)
        : fcitx::InputContext(manager, display)
        , m_client(client)
    {
        created();
    }

    ~SerenityInputContext() override
    {
        // Do NOT call destroy() here: fcitx::InputContext's own destructor
        // already does it, and destroy() asserts !destroyed_. Calling it twice
        // aborts fcitx5 during AddonManager::unload, which is exactly the
        // "Assertion `!d->destroyed_` failed" crash we kept seeing on exit.
    }

protected:
    char const* frontend() const override { return "serenity"; }

    void commitStringImpl(std::string const& text) override;
    void deleteSurroundingTextImpl(int offset, unsigned int size) override;
    void forwardKeyImpl(fcitx::ForwardKeyEvent const& key) override;
    void updatePreeditImpl() override;
    void updateClientSideUIImpl() override;

private:
    ImeClient& m_client;
};

// One accepted socket connection. There used to be exactly one of these: the
// socket accepted a new client by *dropping* the previous one, because the only
// consumer was a focused TextEditor. The Wayland host bridge is now a second
// consumer (it relays text into Linux clients), and it must be able to hold the
// socket while a native Serenity app is also connected — so every connection
// gets its own input context and its own read buffer.
struct ImeClient {
    SerenityIME* ime { nullptr };
    int fd { -1 };
    std::string read_buffer;
    std::unique_ptr<fcitx::EventSourceIO> read_event;
    std::unique_ptr<SerenityInputContext> context;
    bool dead { false };
};

class SerenityIME final : public fcitx::AddonInstance {
public:
    explicit SerenityIME(fcitx::AddonManager* manager)
        : m_manager(manager)
    {
        open_server();
        open_control_server();
        // NOTE: do NOT publish_methods() here. At addon-creation time the
        // InputMethodManager has not loaded its groups yet, so
        // currentGroup() is invalid and dereferencing it crashes fcitx5.
        // The methods file is published lazily when a client sends `create`.
    }

    ~SerenityIME() override
    {
        // Do NOT call focusOut() from here: it dereferences the input
        // context's focus group. During AddonManager::unload that group is
        // already being torn down, so focusOut() segfaults and takes fcitx5
        // down with it - which in turn kills every GTK/Qt client whose IM
        // module was connected to it (the "Linux app crashes when switching to
        // Chinese" symptom). Just release the contexts and our sockets.
        for (auto& client : m_clients) {
            client->read_event.reset();
            client->context.reset();
            if (client->fd >= 0) {
                ::close(client->fd);
                client->fd = -1;
            }
        }
        m_clients.clear();
        if (m_server_fd >= 0) {
            ::close(m_server_fd);
            ::unlink(socket_path());
        }
    }

    void open_server()
    {
        auto path = std::string { socket_path() };
        ::unlink(path.c_str());

        m_server_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_server_fd < 0) {
            fprintf(stderr, "serenityime: socket() failed: %s\n", strerror(errno));
            return;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        // Leave room for the terminating NUL.
        if (path.size() >= sizeof(address.sun_path)) {
            fprintf(stderr, "serenityime: socket path too long: %s\n", path.c_str());
            ::close(m_server_fd);
            m_server_fd = -1;
            return;
        }
        memcpy(address.sun_path, path.c_str(), path.size());

        if (::bind(m_server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            fprintf(stderr, "serenityime: bind(%s) failed: %s\n", path.c_str(), strerror(errno));
            ::close(m_server_fd);
            m_server_fd = -1;
            return;
        }
        ::chmod(path.c_str(), 0600);
        ::listen(m_server_fd, 8);

        m_accept_event = m_manager->instance()->eventLoop().addIOEvent(
            m_server_fd, fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                accept_client();
                return true;
            });
        fprintf(stderr, "serenityime: listening on %s\n", path.c_str());
    }

    void accept_client()
    {
        // Reap connections that died earlier; their event sources are already
        // reset, so this is the only safe point to actually free them.
        m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
                            [](std::unique_ptr<ImeClient> const& client) { return client->dead; }),
            m_clients.end());

        auto fd = ::accept4(m_server_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            return;

        auto client = std::make_unique<ImeClient>();
        client->ime = this;
        client->fd = fd;
        auto* raw = client.get();
        m_clients.push_back(std::move(client));
        fprintf(stderr, "serenityime: client connected (fd=%d, %zu total)\n", fd, m_clients.size());

        raw->read_event = m_manager->instance()->eventLoop().addIOEvent(
            fd, fcitx::IOEventFlag::In,
            [this, raw](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                return this->read_client(raw);
            });
    }

    void drop_client(ImeClient* client)
    {
        if (!client || client->dead)
            return;
        client->dead = true;
        client->read_event.reset();
        if (client->context) {
            client->context->focusOut();
            client->context.reset();
        }
        if (client->fd >= 0) {
            ::close(client->fd);
            client->fd = -1;
        }
        publish_status();
        fprintf(stderr, "serenityime: client gone (%zu remaining)\n", m_clients.size());
    }

    bool read_client(ImeClient* client)
    {
        char buffer[4096];
        auto count = ::read(client->fd, buffer, sizeof(buffer));
        if (count <= 0) {
            drop_client(client);
            return true;
        }
        client->read_buffer.append(buffer, static_cast<size_t>(count));

        size_t newline;
        while ((newline = client->read_buffer.find('\n')) != std::string::npos) {
            auto line = client->read_buffer.substr(0, newline);
            client->read_buffer.erase(0, newline + 1);
            handle_command(client, line);
        }
        return true;
    }

    void handle_command(ImeClient* client, std::string const& line)
    {
        auto& m_context = client->context;
        auto space = line.find(' ');
        auto command = line.substr(0, space);
        auto rest = space == std::string::npos ? std::string {} : line.substr(space + 1);

        if (command == "create") {
            if (!m_context) {
                m_context = std::make_unique<SerenityInputContext>(
                    m_manager->instance()->inputContextManager(), "serenity", *client);
                // An input context only picks up the current input method once
                // it belongs to a focus group. Without this fcitx reports no
                // current input method and keyEvent() always returns false,
                // even when pinyin is configured as the default.
                m_context->setFocusGroup(m_manager->instance()->defaultFocusGroup("serenity:"));
                // ClientSideUI tells fcitx not to draw anything itself: we get
                // the preedit and candidate list through updateClientSideUIImpl
                // and paint them with LibGUI on the Serenity side.
                m_context->setCapabilityFlags(
                    fcitx::CapabilityFlags { fcitx::CapabilityFlag::ClientSideUI }
                    | fcitx::CapabilityFlag::Preedit
                    | fcitx::CapabilityFlag::FormattedPreedit
                    | fcitx::CapabilityFlag::SurroundingText);
                // Activate the group's default input method for this context.
                // setFocusGroup() alone does not switch the context's IM in
                // fcitx5 5.1, so key events would otherwise go to "keyboard"
                // and never compose.
                auto& imm = m_manager->instance()->inputMethodManager();
                auto const im = imm.currentGroup().defaultInputMethod();
                if (!im.empty())
                    m_manager->instance()->setCurrentInputMethod(m_context.get(), im, false);
            }
            // A client is connected now, so fcitx5 is fully up and the input
            // method manager is loaded: publish the method list and current
            // status so the taskbar indicator has real data.
            publish_methods();
            publish_status();
            send(client, "ok");
            return;
        }

        if (command == "destroy") {
            if (m_context) {
                m_context->focusOut();
                m_context.reset();
            }
            publish_status();
            send(client, "ok");
            return;
        }

        if (!m_context) {
            send(client, "err no-context");
            return;
        }

        if (command == "cap") {
            m_context->setCapabilityFlags(
                fcitx::CapabilityFlags { fcitx::CapabilityFlag::ClientSideUI }
                | fcitx::CapabilityFlag::Preedit
                | fcitx::CapabilityFlag::FormattedPreedit
                | fcitx::CapabilityFlag::SurroundingText);
            return;
        }

        if (command == "focus") {
            if (rest == "1")
                m_context->focusIn();
            else
                m_context->focusOut();
            publish_status();
            return;
        }

        if (command == "cursor") {
            int x = 0, y = 0, w = 0, h = 0;
            if (sscanf(rest.c_str(), "%d %d %d %d", &x, &y, &w, &h) == 4)
                m_context->setCursorRect(fcitx::Rect { x, y, w, h });
            return;
        }

        if (command == "surround") {
            // rest = "<cursor> <anchor> <hex text>"
            auto first_space = rest.find(' ');
            if (first_space == std::string::npos)
                return;
            auto second_space = rest.find(' ', first_space + 1);
            if (second_space == std::string::npos)
                return;

            auto cursor = static_cast<unsigned int>(
                std::strtoul(rest.substr(0, first_space).c_str(), nullptr, 10));
            auto anchor = static_cast<unsigned int>(
                std::strtoul(rest.substr(first_space + 1, second_space - first_space - 1).c_str(), nullptr, 10));
            auto decoded = hex_decode(rest.substr(second_space + 1));

            // SurroundingText stores cursor/anchor as character offsets, the
            // same units the client sends and the same units
            // deleteSurroundingText() uses.
            m_context->surroundingText().setText(decoded, cursor, anchor);
            m_context->updateSurroundingText();
            return;
        }

        if (command == "reset") {
            m_context->reset();
            return;
        }

        if (command == "key") {
            unsigned sym = 0, states = 0;
            int release = 0;
            if (sscanf(rest.c_str(), "%u %u %d", &sym, &states, &release) != 3) {
                send(client, "err bad-key");
                return;
            }
            fcitx::Key key { static_cast<fcitx::KeySym>(sym), fcitx::KeyStates { states } };
            fcitx::KeyEvent event { m_context.get(), key, release != 0, 0 };
            auto handled = m_context->keyEvent(event);
            send(client, handled ? "handled 1" : "handled 0");
            return;
        }

        // Selecting a candidate by mouse click: fcitx5 exposes this through
        // InputContext::selectCandidate(). It triggers the same panel/commit
        // signals as pressing the candidate's key, so we re-push the panel
        // afterwards and always answer with `handled` so the client's read
        // loop terminates.
        if (command == "select") {
            int idx = std::atoi(rest.c_str());
            if (m_context && idx >= 0) {
                // fcitx5 5.1 has no InputContext::selectCandidate(); the
                // panel's CandidateList exposes each CandidateWord::select(),
                // which commits it exactly as a key press would.
                auto& panel_ = m_context->inputPanel();
                if (auto candidates = panel_.candidateList(); candidates && idx < candidates->size())
                    candidates->candidate(idx).select(m_context.get());
                emit_panel(client);
                publish_status();
            }
            send(client, "handled 0");
            return;
        }

        send(client, "err unknown-command");
    }

    void send(ImeClient* client, std::string const& line)
    {
        if (!client || client->fd < 0 || client->dead)
            return;
        auto payload = line + "\n";
        size_t written = 0;
        while (written < payload.size()) {
            // MSG_NOSIGNAL: the client may disappear between two updates.
            // A plain write() would raise SIGPIPE and take fcitx5 down with it.
            auto result = ::send(client->fd, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
            if (result < 0) {
                if (errno == EINTR)
                    continue;
                // Leave the actual teardown to the read event; doing it here
                // would pull the input context out from under the callback
                // that is currently walking it.
                return;
            }
            written += static_cast<size_t>(result);
        }
    }

    void open_control_server()
    {
        auto path = control_socket_path();
        ::unlink(path.c_str());

        m_ctl_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_ctl_fd < 0) {
            fprintf(stderr, "serenityime: control socket() failed: %s\n", strerror(errno));
            return;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            ::close(m_ctl_fd);
            m_ctl_fd = -1;
            return;
        }
        memcpy(address.sun_path, path.c_str(), path.size());

        if (::bind(m_ctl_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            fprintf(stderr, "serenityime: control bind(%s) failed: %s\n", path.c_str(), strerror(errno));
            ::close(m_ctl_fd);
            m_ctl_fd = -1;
            return;
        }
        ::listen(m_ctl_fd, 1);

        m_ctl_accept_event = m_manager->instance()->eventLoop().addIOEvent(
            m_ctl_fd, fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                accept_control_client();
                return true;
            });
        fprintf(stderr, "serenityime: control socket on %s\n", path.c_str());
    }

    void accept_control_client()
    {
        auto fd = ::accept4(m_ctl_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            return;

        // The indicator writes a single short command then closes; drain until
        // EOF (or a newline) and act on it.
        std::string buffer;
        char chunk[256];
        while (true) {
            auto count = ::read(fd, chunk, sizeof(chunk));
            if (count <= 0)
                break;
            buffer.append(chunk, static_cast<size_t>(count));
            if (buffer.find('\n') != std::string::npos)
                break;
        }
        ::close(fd);

        auto newline = buffer.find('\n');
        if (newline != std::string::npos)
            buffer.erase(newline);
        if (!buffer.empty())
            handle_control_command(buffer);
    }

    void handle_control_command(std::string const& command)
    {
        auto space = command.find(' ');
        auto verb = command.substr(0, space);
        auto rest = space == std::string::npos ? std::string {} : command.substr(space + 1);

        if (verb == "set") {
            switch_to(rest);
        } else if (verb == "toggle") {
            if (rest == "off") {
                switch_to("keyboard-us");
            } else {
                // Toggle: if a CJK method is active, drop to the keyboard; if the
                // keyboard is active, jump to the first CJK-capable method.
                auto current = current_unique_name();
                if (is_keyboard_layout(current))
                    switch_to(first_cjk_method());
                else
                    switch_to("keyboard-us");
            }
        } else if (verb == "reload") {
            // The Control Panel applet (serenity-ime-settings) rewrote rime's
            // custom YAML and ran rime_deployer. Reload the rime addon so the
            // new speller algebra / page_size is picked up without restarting
            // fcitx5 (which would tear down this socket and the whole session).
            // "reload 1" re-activates rime, "reload 0" drops to the keyboard.
            m_manager->instance()->reloadAddonConfig("rime");
            switch_to(rest == "0" ? "keyboard-us" : "rime");
        } else if (verb == "next" || verb == "prev") {
            // Page through the current group's input method list.
            auto& imm = m_manager->instance()->inputMethodManager();
            auto const& list = imm.currentGroup().inputMethodList();
            auto const current = current_unique_name();
            if (list.size() > 1) {
                size_t index = 0;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].name() == current) {
                        index = i;
                        break;
                    }
                }
                size_t const target = verb == "next"
                    ? (index + 1) % list.size()
                    : (index + list.size() - 1) % list.size();
                switch_to(list[target].name());
            }
        }

        publish_status();
    }

    // fcitx5 5.1 dropped FocusGroup's input-method accessors; the active input
    // method now lives on the current InputMethodGroup (defaultInputMethod),
    // and switching is setDefaultInputMethod(). Go through the manager for all
    // of it so the indicator can read and drive the state.
    std::string current_unique_name()
    {
        // Prefer the actual current IM of the focused context; fall back to the
        // current group's default when nothing is focused yet.
        auto name = m_manager->instance()->currentInputMethod();
        if (name.empty()) {
            auto& imm = m_manager->instance()->inputMethodManager();
            name = imm.currentGroup().defaultInputMethod();
        }
        return name.empty() ? std::string { "keyboard-us" } : name;
    }

    std::string first_cjk_method()
    {
        auto& imm = m_manager->instance()->inputMethodManager();
        for (auto const& item : imm.currentGroup().inputMethodList()) {
            if (!is_keyboard_layout(item.name()))
                return item.name();
        }
        return "keyboard-us";
    }

    void switch_to(std::string const& unique_name)
    {
        if (auto* context = focused_context())
            m_manager->instance()->setCurrentInputMethod(context, unique_name, false);
        else
            m_manager->instance()->setCurrentInputMethod(unique_name);
    }

    void publish_methods()
    {
        auto& imm = m_manager->instance()->inputMethodManager();
        std::string out;
        for (auto const& item : imm.currentGroup().inputMethodList()) {
            out += item.name();
            out += '\t';
            if (auto const* entry = imm.entry(item.name()))
                out += entry->name();
            else
                out += item.name();
            out += '\n';
        }
        write_file(methods_path(), out);
    }

    void publish_status()
    {
        std::string unique = current_unique_name();
        std::string display = unique;
        auto& imm = m_manager->instance()->inputMethodManager();
        if (auto const* entry = imm.entry(unique))
            display = entry->name();
        bool cjk = !is_keyboard_layout(unique);
        std::string line = std::to_string(cjk ? 1 : 0);
        line += '\t';
        line += unique;
        line += '\t';
        line += display;
        line += '\n';
        write_file(status_path(), line);
    }

    // --- pushed from the input context -------------------------------------

    void emit_commit(ImeClient* client, std::string const& text)
    {
        send(client, "commit " + hex_encode(text));
    }

    void emit_delete(ImeClient* client, int offset, unsigned int size)
    {
        send(client, "delete " + std::to_string(offset) + " " + std::to_string(size));
    }

    void emit_forward(ImeClient* client, fcitx::ForwardKeyEvent const& key)
    {
        send(client, "forward " + std::to_string(static_cast<unsigned>(key.rawKey().sym()))
            + " " + std::to_string(key.rawKey().states().toInteger())
            + " " + std::to_string(key.isRelease() ? 1 : 0));
    }

    void emit_panel(ImeClient* client)
    {
        if (!client || !client->context)
            return;
        auto const& panel = client->context->inputPanel();
        auto preedit = panel.clientPreedit();
        if (preedit.empty())
            preedit = panel.preedit();

        std::string line = "panel ";
        line += std::to_string(preedit.cursor());
        line += " " + hex_encode(panel.auxUp().toString());
        line += " " + hex_encode(preedit.toString());

        auto candidates = panel.candidateList();
        if (!candidates) {
            line += " -1 0 0 0";
            send(client, line);
            return;
        }

        line += " " + std::to_string(candidates->cursorIndex());
        line += " " + std::to_string(candidates->size());
        auto count = candidates->size();
        for (int index = 0; index < count; ++index) {
            line += " " + hex_encode(candidates->label(index).toString());
            line += " " + hex_encode(candidates->candidate(index).text().toString());
        }
        // Two trailing flags tell the client whether paging is possible. In
        // fcitx5 5.1 hasPrev/hasNext live on PageableCandidateList, not on the
        // base CandidateList; a non-pageable list simply has no extra pages.
        auto* pageable = candidates->toPageable();
        line += " " + std::to_string(pageable && pageable->hasPrev() ? 1 : 0);
        line += " " + std::to_string(pageable && pageable->hasNext() ? 1 : 0);
        send(client, line);
    }

private:
    fcitx::AddonManager* m_manager { nullptr };
    int m_server_fd { -1 };
    std::unique_ptr<fcitx::EventSourceIO> m_accept_event;
    std::vector<std::unique_ptr<ImeClient>> m_clients;

    // The input context that currently has focus, if any. The control socket
    // (taskbar indicator) needs it to switch input method per context.
    fcitx::InputContext* focused_context()
    {
        for (auto& client : m_clients) {
            if (client->context && client->context->hasFocus())
                return client->context.get();
        }
        return nullptr;
    }

    // --- status publishing + control socket ------------------------------
    //
    // The taskbar indicator is a separate process that cannot talk to fcitx5
    // directly, so this addon publishes the current input method to a status
    // file and serves a tiny control socket for "switch / toggle" requests.
    // These fcitx5 APIs (FocusGroup::setInputMethod/nextInputMethod,
    // InputMethod::uniqueName/displayName, InputMethodManager::listInputMethods)
    // are matched against the image's fcitx5 version and verified on the VM
    // build - they are not compiled in this sandbox.

    int m_ctl_fd { -1 };
    std::unique_ptr<fcitx::EventSourceIO> m_ctl_accept_event;

    // open_control_server / accept_control_client / handle_control_command /
    // publish_status / publish_methods / switch_to are all defined inline above;
    // only the path helpers remain to be declared here.

    std::string status_path() const { return std::string { status_dir() } + "/serenity-ime-status"; }
    std::string methods_path() const { return std::string { status_dir() } + "/serenity-ime-methods"; }
    std::string control_socket_path() const { return std::string { status_dir() } + "/serenity-ime-ctl.sock"; }
};

void SerenityInputContext::commitStringImpl(std::string const& text)
{
    m_client.ime->emit_commit(&m_client, text);
}

void SerenityInputContext::deleteSurroundingTextImpl(int offset, unsigned int size)
{
    m_client.ime->emit_delete(&m_client, offset, size);
}

void SerenityInputContext::forwardKeyImpl(fcitx::ForwardKeyEvent const& key)
{
    m_client.ime->emit_forward(&m_client, key);
}

void SerenityInputContext::updatePreeditImpl()
{
    m_client.ime->emit_panel(&m_client);
}

void SerenityInputContext::updateClientSideUIImpl()
{
    m_client.ime->emit_panel(&m_client);
}

class SerenityIMEFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override
    {
        return new SerenityIME(manager);
    }
};

FCITX_ADDON_FACTORY_V2(serenityime, SerenityIMEFactory)
