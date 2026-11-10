/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * serenity-engine —— 拼音引擎进程（librime 直链，不再需要 fcitx5）。
 *
 *   Wayland 客户端 ─ text_input_v3 ─┐
 *   X11 客户端     ─ XIM ───────────┤→ serenity-wlroots-host (ImeService)
 *   原生 Serenity  ─ LibGUI ────────┘        │
 *                                            ↓ unix socket 行协议
 *                                     serenity-engine → librime → rime 词库
 *
 * 协议和原来的 fcitx5 addon 逐字兼容（客户端不用改）：
 *
 *   client → engine
 *     create                          -> "ok"
 *     cap <flags>
 *     focus <0|1>                     -> "ok"
 *     cursor <x> <y> <w> <h>
 *     surround <cursor> <anchor> <hex>
 *     key <keysym> <states> <release> -> "handled <0|1>"（前面可能先有 commit/panel 行）
 *     select <index>                  -> "handled 0"
 *     reset / destroy
 *     page <0|1>                      翻页（0=上一页 1=下一页）
 *
 *   engine → client
 *     commit <hex>
 *     panel <cursor> <aux_hex> <preedit_hex> <cand_cursor> <n> [<label> <text>]* <prev> <next>
 *     无候选时：panel <cursor> <aux> <preedit> -1 0 0 0
 *     handled <0|1>
 *
 *   Ctrl+Space（keysym 0x20 + Ctrl 位）＝开启/关闭中文输入法，由本进程在把键
 *   交给 rime **之前**拦截并吞掉（press 和 release 都吞）。关闭期间所有按键一律
 *   回 "handled 0"，完全不进 rime，等于英文直输。开关是全局状态，三条前端
 *   （原生 LibGUI / Wayland text-input / XIM）共用，切窗口也保持。
 *
 *   控制 socket（任务栏指示器 / 输入法设置界面）：
 *     status                       -> "ok active <0|1> <name>"
 *     set rime|keyboard-us         -> 开启 / 关闭
 *     toggle [on|off]              -> 翻转 / 指定状态
 *   状态写到 $SERENITY_IME_STATUS_DIR/serenity-ime-status（和旧 addon 同格式），
 *   托盘 IMEIndicator 每 500ms 读一次，显示「中 / EN」。
 *
 * 为什么要自己当宿主：fcitx5 在这套架构里只干「加载 rime」这一件事，却带来
 * 34 个包、一个必须靠 shell 脚本守护的进程、以及 unload 时的析构崩溃
 * （InputContext double destroy）。直链 librime 以后引擎就是一个普通进程，
 * 掉线由 SessionManager 的 always_restart 拉起。
 */

#include <rime_api.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

RimeApi* g_rime { nullptr };
bool g_running { true };

// 中文输入法总开关。Ctrl+Space / 托盘 / 设置界面都改这一个变量；关闭时按键
// 完全不进 rime。全局（而非 per-client）是 Windows / fcitx 的 Ctrl+Space 语义：
// 切到另一个窗口也保持英文，直到再次按 Ctrl+Space。
bool g_active { true };

std::string g_socket_path;
std::string g_ctl_path;
std::string g_user_data_dir;

struct Client {
    int fd { -1 };
    RimeSessionId session { 0 };
    std::string buffer;
    bool focused { false };
    /* release 事件要回「这次按键是否被吃掉」，但 rime 只对 press 有意义，
     * 所以记住每个 keysym 上次 press 的结果，release 直接复用。 */
    std::map<unsigned, bool> last_press_handled;
};

std::vector<Client*> g_clients;

// --- 编码 ---------------------------------------------------------------

std::string hex_encode(std::string const& text)
{
    static char const digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        out += digits[c >> 4];
        out += digits[c & 0xf];
    }
    return out;
}

std::string hex_decode(std::string const& hex)
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
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            break;
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

// --- socket 发送 --------------------------------------------------------

void send_line(int fd, std::string const& line)
{
    if (fd < 0)
        return;
    auto payload = line + "\n";
    size_t written = 0;
    while (written < payload.size()) {
        /* MSG_NOSIGNAL：客户端可能在任何一次写之间消失，没有它就 SIGPIPE 自杀。 */
        auto result = ::send(fd, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        written += static_cast<size_t>(result);
    }
}

// --- rime 状态推送 ------------------------------------------------------

void emit_commit(Client* client)
{
    RIME_STRUCT(RimeCommit, commit);
    if (!g_rime->get_commit(client->session, &commit))
        return;
    if (commit.text)
        send_line(client->fd, "commit " + hex_encode(commit.text));
    g_rime->free_commit(&commit);
}

/* 候选标签：rime 会给出 select_keys（"1234567890"），没有就自己排。 */
std::string label_for(RimeContext const& ctx, int index)
{
    if (ctx.menu.select_keys) {
        auto const* keys = ctx.menu.select_keys;
        if (index < static_cast<int>(strlen(keys)))
            return std::string(1, keys[index]);
    }
    static char const digits[] = "1234567890";
    return std::string(1, digits[index % 10]);
}

void emit_panel(Client* client)
{
    RIME_STRUCT(RimeContext, ctx);
    if (!g_rime->get_context(client->session, &ctx)) {
        send_line(client->fd, "panel 0   -1 0 0 0");
        return;
    }

    std::string preedit = ctx.composition.preedit ? ctx.composition.preedit : "";
    std::string aux = ctx.commit_text_preview ? ctx.commit_text_preview : "";
    /* cursor_pos 是字节偏移，客户端按字符画光标；这里换算成字符数。 */
    int cursor = 0;
    if (ctx.composition.preedit && ctx.composition.cursor_pos > 0) {
        for (int i = 0; i < ctx.composition.cursor_pos && preedit[i]; ++i) {
            if ((static_cast<unsigned char>(preedit[i]) & 0xc0) != 0x80)
                ++cursor;
        }
    }

    std::string line = "panel " + std::to_string(cursor) + " " + hex_encode(aux) + " " + hex_encode(preedit);

    if (ctx.menu.num_candidates <= 0) {
        line += " -1 0 0 0";
        send_line(client->fd, line);
        g_rime->free_context(&ctx);
        return;
    }

    line += " " + std::to_string(ctx.menu.highlighted_candidate_index);
    line += " " + std::to_string(ctx.menu.num_candidates);
    for (int i = 0; i < ctx.menu.num_candidates; ++i) {
        auto const* text = ctx.menu.candidates[i].text ? ctx.menu.candidates[i].text : "";
        line += " " + hex_encode(label_for(ctx, i)) + " " + hex_encode(text);
    }
    line += " " + std::to_string(ctx.menu.page_no > 0 ? 1 : 0);
    line += " " + std::to_string(ctx.menu.is_last_page ? 0 : 1);
    fprintf(stderr, "serenity-engine: panel cursor=%d aux=%zuB preedit=%zuB n=%d\n",
        cursor, aux.size(), preedit.size(), ctx.menu.num_candidates);
    send_line(client->fd, line);
    g_rime->free_context(&ctx);
}

// --- 输入法开关（Ctrl+Space / 托盘 / 设置界面共用） ------------------------

std::string status_dir()
{
    if (auto const* dir = getenv("SERENITY_IME_STATUS_DIR"); dir && *dir)
        return dir;
    return "/tmp";
}

// 原子写（tmp + rename）：托盘的 500ms 定时器不能读到半行状态。
void write_status_file(std::string const& path, std::string const& content)
{
    auto tmp = path + ".tmp";
    FILE* file = fopen(tmp.c_str(), "wb");
    if (!file)
        return;
    bool ok = fwrite(content.data(), 1, content.size(), file) == content.size();
    fclose(file);
    if (!ok) {
        unlink(tmp.c_str());
        return;
    }
    rename(tmp.c_str(), path.c_str());
}

// 格式与旧 serenityime addon 完全一致，IMEIndicator::refresh() 按 \t 解析：
//   "<active 0|1>\t<unique_name>\t<display_name>"
// methods 文件让托盘的切换菜单在有 rime / keyboard-us 两项可用。
void publish_status()
{
    auto dir = status_dir();
    write_status_file(dir + "/serenity-ime-status",
        g_active ? "1\trime\t中文拼音\n" : "0\tkeyboard-us\tEnglish (US)\n");
    write_status_file(dir + "/serenity-ime-methods",
        "rime\t中文拼音\nkeyboard-us\tEnglish (US)\n");
}

// 引擎被 serenitid 重启后保持上次的开关，避免用户关掉输入法、重启又变回中文。
// 读不到文件就默认开启。
bool initial_active()
{
    auto path = status_dir() + "/serenity-ime-status";
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return true;
    char first = 0;
    size_t count = fread(&first, 1, 1, file);
    fclose(file);
    return count == 0 || first != '0';
}

void set_active(bool active)
{
    bool changed = g_active != active;
    g_active = active;

    // 切换瞬间清掉在飞的组字，并给每个客户端推一个空 panel：内联预编辑和
    // 候选窗都由客户端按 panel 行擦除，否则「关闭」后拼音串还挂在编辑器里。
    if (changed) {
        for (auto* client : g_clients) {
            if (client->session)
                g_rime->clear_composition(client->session);
            emit_panel(client);
        }
        fprintf(stderr, "serenity-engine: IME %s\n", g_active ? "on" : "off");
    }
    publish_status();
}

// --- 会话 ---------------------------------------------------------------

void ensure_session(Client* client)
{
    if (client->session)
        return;
    client->session = g_rime->create_session();
    if (!client->session) {
        fprintf(stderr, "serenity-engine: create_session failed\n");
        return;
    }
    if (!g_rime->select_schema(client->session, "luna_pinyin"))
        fprintf(stderr, "serenity-engine: select_schema(luna_pinyin) failed\n");
    /* 默认简体 + 中文标点：面向中文桌面，繁体候选（妳好/逆號）不该是第一屏。 */
    g_rime->set_option(client->session, "simplification", True);
}

void destroy_session(Client* client)
{
    if (!client->session)
        return;
    g_rime->destroy_session(client->session);
    client->session = 0;
}

// --- 命令处理 -----------------------------------------------------------

std::string take_token(std::string const& text, size_t& pos)
{
    while (pos < text.size() && text[pos] == ' ')
        ++pos;
    auto start = pos;
    while (pos < text.size() && text[pos] != ' ')
        ++pos;
    return text.substr(start, pos - start);
}

void handle_key(Client* client, std::string const& rest)
{
    unsigned keysym = 0, states = 0;
    int release = 0;
    if (sscanf(rest.c_str(), "%u %u %d", &keysym, &states, &release) != 3) {
        send_line(client->fd, "err bad-key");
        return;
    }

    // Ctrl+Space：开启 / 关闭中文输入法（Windows、fcitx 用户习惯的快捷键）。
    // 必须在 rime 看到这个键之前拦掉 —— rime 会把 Ctrl+Space 当成普通空格，
    // 永远不会改状态；也必须在 ensure_session() 之前，因为关闭状态下引擎
    // 可能根本没有可用 session，而这时恰恰最需要能把输入法重新打开。
    //
    // states 是 X11 修饰位：Ctrl = 1<<2。带 Alt / Mod3 / Super / AltGr 的一律
    // 不认，留给客户端当自己的快捷键；Shift / Lock / NumLock 不影响判定。
    constexpr unsigned ctrl_bit = 1u << 2;
    constexpr unsigned other_bits = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7);
    if (keysym == 0x20 && !release && (states & ctrl_bit) && !(states & other_bits)) {
        set_active(!g_active);
        // press 被吞，配套的 release 也要吞。下面的通用 release 分支按 keysym
        // 查这条记录，所以即使 keyup 时 Ctrl 已经松开、states 不再带 Ctrl，
        // 也不会把半个空格事件漏给编辑器。
        client->last_press_handled[keysym] = true;
        send_line(client->fd, "handled 1");
        return;
    }

    if (release) {
        auto it = client->last_press_handled.find(keysym);
        send_line(client->fd, (it != client->last_press_handled.end() && it->second) ? "handled 1" : "handled 0");
        return;
    }

    // 输入法关闭：连 session 都不用碰，所有按键原样放行给客户端（英文直输）。
    if (!g_active) {
        client->last_press_handled[keysym] = false;
        send_line(client->fd, "handled 0");
        return;
    }

    ensure_session(client);
    if (!client->session) {
        client->last_press_handled[keysym] = false;
        send_line(client->fd, "handled 0");
        return;
    }

    /* X11 修饰位正好是 rime 的约定（Shift=1 Lock=2 Control=4 Mod1=8 Mod4=64）。 */
    unsigned mask = states;
    bool handled = keysym != 0 && g_rime->process_key(client->session, keysym, mask);

    emit_commit(client);
    emit_panel(client);
    client->last_press_handled[keysym] = handled;
    send_line(client->fd, handled ? "handled 1" : "handled 0");
}

void handle_command(Client* client, std::string const& line)
{
    size_t pos = 0;
    auto command = take_token(line, pos);
    auto rest = pos < line.size() ? line.substr(pos) : std::string {};

    if (command == "create") {
        ensure_session(client);
        send_line(client->fd, "ok");
        return;
    }
    if (command == "cap")
        return;
    if (command == "focus") {
        client->focused = (rest == "1" || rest == " 1");
        if (client->focused) {
            ensure_session(client);
            /* 重新拿到焦点时清掉残留组字，否则上一窗口的拼音会跟过来。 */
            if (client->session)
                g_rime->clear_composition(client->session);
        } else if (client->session) {
            g_rime->clear_composition(client->session);
        }
        send_line(client->fd, "ok");
        return;
    }
    if (command == "cursor")
        return;
    if (command == "surround") {
        /* 引擎用不到上下文文本（拼音不需要 deleteSurroundingText），但
         * 「surround 命令被接收」这个事实要静默吞掉，不要回 err。 */
        return;
    }
    if (command == "key") {
        handle_key(client, rest);
        return;
    }
    if (command == "select") {
        ensure_session(client);
        int index = atoi(rest.c_str());
        if (client->session && index >= 0) {
            g_rime->select_candidate(client->session, index);
            emit_commit(client);
            emit_panel(client);
        }
        send_line(client->fd, "handled 0");
        return;
    }
    if (command == "page") {
        ensure_session(client);
        if (client->session) {
            bool next = (rest == "1" || rest == " 1");
            /* Page_Up/Page_Down 的 X keysym：rime 自己认这两个键翻页。 */
            g_rime->process_key(client->session, next ? 0xff56u : 0xff55u, 0);
            emit_commit(client);
            emit_panel(client);
        }
        send_line(client->fd, "handled 0");
        return;
    }
    if (command == "reset") {
        if (client->session)
            g_rime->clear_composition(client->session);
        emit_panel(client);
        return;
    }
    if (command == "destroy") {
        destroy_session(client);
        client->focused = false;
        return;
    }
    send_line(client->fd, "err unknown-command");
}

// --- 控制 socket（任务栏输入法指示器 / IMESettings） --------------------

void handle_control(int fd)
{
    std::string line;
    char chunk[512];
    for (;;) {
        auto count = ::read(fd, chunk, sizeof(chunk));
        if (count <= 0)
            break;
        line.append(chunk, static_cast<size_t>(count));
        if (line.find('\n') != std::string::npos)
            break;
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();

    size_t pos = 0;
    auto command = take_token(line, pos);
    auto rest = pos < line.size() ? line.substr(pos) : std::string {};
    /* take_token 把 pos 停在命令后的空格上，rest 会带前导空格。 */
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
        rest.erase(rest.begin());

    if (command == "status") {
        send_line(fd, g_active ? "ok active 1 rime" : "ok active 0 keyboard-us");
        return;
    }
    if (command == "reload") {
        /* 设置界面写完 custom YAML 后发来。让本进程优雅退出，由 serenitid 的
         * always_restart 拉起新进程：启动时 initialize_rime() 会强制重新部署，
         * 新配置才会真正生效。开关状态已经由 publish_status() 落盘，
         * 重启后 initial_active() 会恢复，不会偷偷变回中文。 */
        send_line(fd, "ok");
        g_running = false;
        return;
    }
    if (command == "set") {
        // 托盘菜单发 "set <unique>"，设置界面发 "set rime"。只有明确切到键盘
        // 布局才关闭，其余一律当作开启中文输入法。
        bool enable = rest != "keyboard-us" && rest != "off" && rest != "0";
        set_active(enable);
        if (enable) {
            /* 每个活着的会话重选一次 schema，保证简体 + 中文标点。 */
            for (auto* client : g_clients) {
                if (client->session && g_rime->select_schema(client->session, "luna_pinyin"))
                    g_rime->set_option(client->session, "simplification", True);
            }
        }
        send_line(fd, "ok");
        return;
    }
    if (command == "toggle") {
        // "toggle off" 来自托盘的 Turn IME off / 设置界面的「启用中文输入法」
        // 复选框；裸 "toggle" 翻转，与 Ctrl+Space 完全等价。
        bool enable;
        if (rest == "off" || rest == "0")
            enable = false;
        else if (rest == "on" || rest == "1")
            enable = true;
        else
            enable = !g_active;
        set_active(enable);
        send_line(fd, g_active ? "ok active 1" : "ok active 0");
        return;
    }
    send_line(fd, "ok");
}

// --- socket 建立 --------------------------------------------------------

int make_listen_socket(std::string const& path)
{
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        fprintf(stderr, "serenity-engine: bind(%s) failed: %s\n", path.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(fd, 8) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void drop_client(Client* client, size_t index)
{
    destroy_session(client);
    if (client->fd >= 0)
        ::close(client->fd);
    delete client;
    g_clients.erase(g_clients.begin() + static_cast<long>(index));
}

void on_signal(int)
{
    g_running = false;
}

std::string default_user_data_dir()
{
    if (auto const* env = getenv("RIME_USER_DATA_DIR"); env && *env)
        return env;
    if (auto const* home = getenv("HOME"); home && *home)
        return std::string(home) + "/.local/share/rime";
    return "/tmp/rime";
}

void initialize_rime()
{
    g_rime = rime_get_api();
    if (!g_rime) {
        fprintf(stderr, "serenity-engine: rime_get_api failed\n");
        exit(1);
    }

    /* 必须用 RIME_STRUCT 初始化：data_size=0 时 librime 会静默忽略所有字段，
     * user_data_dir 退化成 "./"，表现是「配置文件不存在 + 一个候选都没有」。 */
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = "/usr/share/rime-data";
    traits.user_data_dir = g_user_data_dir.c_str();
    traits.distribution_name = "Serenity";
    traits.distribution_code_name = "serenity";
    traits.distribution_version = "1.0";
    traits.app_name = "serenity-engine";
    traits.min_log_level = 2;

    g_rime->setup(&traits);
    g_rime->initialize(&traits);

    /* 首次启动（或词库缺失）要建 luna_pinyin.table.bin，必须等它跑完，
     * 否则第一屏候选会是空的 —— fcitx5 时代那个「候选全是阿」就是这么来的。 */
    if (g_rime->start_maintenance(True))
        g_rime->join_maintenance_thread();
    fprintf(stderr, "serenity-engine: rime ready (user_data=%s)\n", g_user_data_dir.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    /* 数据面 socket 保持固定路径（LibGUI::InputMethod 与桥默认连它）；
     * 控制面 socket 必须和托盘、设置界面一样从 SERENITY_IME_STATUS_DIR 派生，
     * 否则一旦会话注入了专用状态目录，托盘发命令会连到一个没人监听的路径。 */
    g_socket_path = "/tmp/serenity-ime.sock";
    g_ctl_path = status_dir() + "/serenity-ime-ctl.sock";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            g_socket_path = argv[++i];
        else if (!strcmp(argv[i], "--ctl-socket") && i + 1 < argc)
            g_ctl_path = argv[++i];
    }
    g_user_data_dir = default_user_data_dir();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    initialize_rime();

    /* 恢复上次的开关状态（引擎被 serenitid 拉起时不丢失「已关闭」），并先把
     * 托盘要读的 status / methods 文件写好，避免托盘在第一次按键前一直显示 EN。 */
    g_active = initial_active();
    publish_status();
    fprintf(stderr, "serenity-engine: IME initially %s\n", g_active ? "on" : "off");

    int listen_fd = make_listen_socket(g_socket_path);
    if (listen_fd < 0)
        return 1;
    int ctl_fd = make_listen_socket(g_ctl_path);

    fprintf(stderr, "serenity-engine: serving %s\n", g_socket_path.c_str());

    while (g_running) {
        std::vector<pollfd> fds;
        fds.push_back({ listen_fd, POLLIN, 0 });
        if (ctl_fd >= 0)
            fds.push_back({ ctl_fd, POLLIN, 0 });
        std::vector<Client*> order;
        for (auto* client : g_clients) {
            fds.push_back({ client->fd, POLLIN, 0 });
            order.push_back(client);
        }

        auto ready = ::poll(fds.data(), fds.size(), 500);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;

        if (fds[0].revents & POLLIN) {
            auto fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) {
                auto* client = new Client();
                client->fd = fd;
                g_clients.push_back(client);
                fprintf(stderr, "serenity-engine: client connected (fd=%d)\n", fd);
            }
        }
        size_t base = ctl_fd >= 0 ? 2 : 1;
        if (ctl_fd >= 0 && (fds[1].revents & POLLIN)) {
            auto fd = ::accept4(ctl_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) {
                handle_control(fd);
                ::close(fd);
            }
        }

        /* 倒序遍历：读过程中可能把客户端摘掉。 */
        for (size_t i = order.size(); i-- > 0;) {
            auto* client = order[i];
            short re = fds[base + i].revents;
            if (!(re & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char chunk[4096];
            auto count = ::read(client->fd, chunk, sizeof(chunk));
            if (count <= 0) {
                auto it = std::find(g_clients.begin(), g_clients.end(), client);
                if (it != g_clients.end())
                    drop_client(client, static_cast<size_t>(it - g_clients.begin()));
                continue;
            }
            client->buffer.append(chunk, static_cast<size_t>(count));
            for (;;) {
                auto newline = client->buffer.find('\n');
                if (newline == std::string::npos)
                    break;
                auto line = client->buffer.substr(0, newline);
                client->buffer.erase(0, newline + 1);
                if (!line.empty())
                    handle_command(client, line);
            }
        }
    }

    for (size_t i = g_clients.size(); i-- > 0;)
        drop_client(g_clients[i], i);
    ::close(listen_fd);
    if (ctl_fd >= 0)
        ::close(ctl_fd);
    ::unlink(g_socket_path.c_str());
    ::unlink(g_ctl_path.c_str());
    g_rime->finalize();
    return 0;
}
