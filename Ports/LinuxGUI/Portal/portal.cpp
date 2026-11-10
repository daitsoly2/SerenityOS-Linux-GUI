/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-portal - the portal interfaces this desktop actually needs.
//
// Two things live here:
//   1. Settings  —— GTK blocks 25 s on org.freedesktop.portal.Settings when no
//      service owns that name, which is why every GTK app used to hang on
//      startup.
//   2. FileChooser —— so GtkFileChooserNative / Firefox / Electron show the
//      *native* Serenity file picker instead of their own dialog.
//
// On Wayland GTK asks org.freedesktop.portal.Settings for
// org.freedesktop.appearance/color-scheme and /accent-color at startup.
//
// The real xdg-desktop-portal cannot serve this desktop: it blocks inside
// g_initable_new waiting for a bus call that never returns here (no document
// portal, no session manager it recognises). So we own
// org.freedesktop.portal.Desktop ourselves and implement the *frontend*
// interface (org.freedesktop.portal.FileChooser, including the Request object
// dance) rather than the impl.portal one - there is no xdg-desktop-portal
// process in the middle to talk to.
//
// Answers for Settings are static: one theme, one accent colour, no dark-mode
// switch. If a theme switch is wired up later, emit SettingChanged from here.

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Serenity's Default theme: light-grey faces, blue selection.
constexpr unsigned APPEARANCE_COLOR_SCHEME = 0; // 0 = no preference, 1 = prefer dark
constexpr unsigned APPEARANCE_CONTRAST = 0;     // 0 = no preference
constexpr double ACCENT_R = 0.129;
constexpr double ACCENT_G = 0.400;
constexpr double ACCENT_B = 0.674;

char const* const PORTAL_PATH = "/org/freedesktop/portal/desktop";
char const* const SETTINGS_INTERFACE = "org.freedesktop.portal.Settings";
char const* const PORTAL_NAME = "org.freedesktop.portal.Desktop";
char const* const APPEARANCE_NAMESPACE = "org.freedesktop.appearance";

char const* signature_for_key(char const* key)
{
    if (strcmp(key, "color-scheme") == 0 || strcmp(key, "contrast") == 0)
        return "u";
    if (strcmp(key, "accent-color") == 0)
        return "(ddd)";
    return nullptr;
}

int append_value(sd_bus_message* message, char const* key, char const* signature)
{
    if (strcmp(signature, "u") == 0) {
        auto value = strcmp(key, "contrast") == 0 ? APPEARANCE_CONTRAST : APPEARANCE_COLOR_SCHEME;
        return sd_bus_message_append(message, "u", value);
    }
    return sd_bus_message_append(message, "(ddd)", ACCENT_R, ACCENT_G, ACCENT_B);
}

int method_read(sd_bus_message* call, void*, sd_bus_error* error)
{
    char const* ns = nullptr;
    char const* key = nullptr;
    if (sd_bus_message_read(call, "ss", &ns, &key) < 0)
        return -EINVAL;

    if (!ns || strcmp(ns, APPEARANCE_NAMESPACE) != 0)
        return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Unknown namespace '%s'", ns ? ns : "");

    auto const* signature = key ? signature_for_key(key) : nullptr;
    if (!signature)
        return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Unknown key '%s'", key ? key : "");

    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    // The reference portal double-wraps: xdg-desktop-portal builds the value as
    // g_variant_new_variant(inner) and then passes it to g_variant_new("(v)", ...),
    // which wraps it again. libadwaita unwraps twice (adw-settings-impl-portal.c:
    // g_variant_get(ret, "(v)", &child); g_variant_get(child, "v", &child2)) and
    // crashes on a single layer with "format 'v' but value has type 'u'".
    ret = sd_bus_message_open_container(reply, 'v', "v");
    if (ret >= 0)
        ret = sd_bus_message_open_container(reply, 'v', signature);
    if (ret >= 0)
        ret = append_value(reply, key, signature);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

// ReadAll answers an empty dict.
//
// libadwaita 1.7 walks the returned a{sa{sv}} assuming the dict value is itself
// a variant and crashes in g_variant_type_equal() when it is not - with a real
// portal it never sees that shape because the portal filters by namespace and
// answers from gsettings. Rather than reproduce gsettings, answer "nothing
// here": libadwaita then keeps its built-in defaults (light scheme), and the
// colour scheme still comes from Read/ReadOne, which GTK3 uses.
int method_read_all(sd_bus_message* call, void*, sd_bus_error*)
{
    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    ret = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

// org.freedesktop.portal.Desktop.GetVersion: cheap, and it makes the name look
// like a real portal to `busctl introspect`.
int method_get_version(sd_bus_message* call, void*, sd_bus_error*)
{
    sd_bus_message* reply = nullptr;
    int ret = sd_bus_message_new_method_return(call, &reply);
    if (ret < 0)
        return ret;

    ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret >= 0)
        ret = sd_bus_message_open_container(reply, 'e', "sv");
    if (ret >= 0)
        ret = sd_bus_message_append(reply, "su", "version", 5u);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        sd_bus_message_unref(reply);
        return ret;
    }

    ret = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return ret;
}

// ---------------------------------------------------------------------------
// FileChooser
// ---------------------------------------------------------------------------
//
// 链路：GTK/Firefox 调 org.freedesktop.portal.FileChooser.OpenFile（前端接口，
// 不是 impl.portal）→ 我们返回一个 Request 对象路径 → fork
// /serenity/bin/serenity-file-picker 弹原生对话框 → 在 Request 上 emit Response。
//
// 为什么要绕一层 helper：GUI::FilePicker 是 GUI::Dialog，需要 WindowServer 连接，
// 而本进程只是个 D-Bus 守护（还可能被 D-Bus 激活，环境变量都不全），自己弹不了窗口。

sd_bus* s_bus { nullptr };
sd_event* s_event { nullptr };

char const* const FILE_CHOOSER_INTERFACE = "org.freedesktop.portal.FileChooser";
char const* const HOST_REGISTRY_INTERFACE = "org.freedesktop.host.portal.Registry";
char const* const REQUEST_INTERFACE = "org.freedesktop.portal.Request";
char const* const PICKER_BINARY = "/serenity/bin/serenity-file-picker";

// 调试用：把 helper 换成任意脚本（打印一行路径即视为选中），这样不点鼠标也能
// 验证「请求 → 起进程 → 读 stdout → emit Response」整条链路。
char const* picker_binary()
{
    auto* override = getenv("SERENITY_PORTAL_PICKER");
    return (override && *override) ? override : PICKER_BINARY;
}
char const* const PICKER_LOG = "/tmp/serenity-portal-picker.log";

struct FileChooserOptions {
    std::string handle_token;
    std::string current_name;
    std::string current_folder;
    std::vector<std::string> filters; // 已拼成 "名称|glob,glob"
    bool multiple { false };
    bool directory { false };
};

struct PickerRequest {
    std::string path;
    std::string sender; // 调用者的 unique name，Response 必须单播给它（见 emit_response）
    sd_bus_slot* slot { nullptr };
    sd_event_source* child_source { nullptr };
    sd_event_source* io_source { nullptr };
    sd_event_source* defer_source { nullptr };
    pid_t pid { -1 };
    int output_fd { -1 };
    std::string output;
    std::string uri;
    bool exited { false };
    bool responded { false };
    bool normal_exit { false };
    int exit_status { 0 };
    uint32_t response { 2 };
};

// portal 的 uris 是 file:// URI，路径里的空格/中文必须百分号编码，否则客户端
// 解析出来是错的（GTK 会直接判成非法 URI 而不打开文件）。
std::string file_uri(std::string const& path)
{
    static char const digits[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (auto character : path) {
        auto byte = static_cast<unsigned char>(character);
        if (isalnum(byte) || byte == '/' || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            uri += static_cast<char>(byte);
        } else {
            uri += '%';
            uri += digits[byte >> 4];
            uri += digits[byte & 0x0F];
        }
    }
    return uri;
}

int parse_filters(sd_bus_message* message, std::vector<std::string>& filters)
{
    int ret = sd_bus_message_enter_container(message, 'a', "(sa(us))");
    if (ret < 0)
        return ret;

    for (;;) {
        ret = sd_bus_message_enter_container(message, 'r', "sa(us)");
        if (ret <= 0)
            break;

        char const* name = nullptr;
        if ((ret = sd_bus_message_read(message, "s", &name)) < 0)
            return ret;

        std::string spec = name ? name : "";
        spec += '|';
        bool first = true;

        if ((ret = sd_bus_message_enter_container(message, 'a', "(us)")) < 0)
            return ret;
        for (;;) {
            ret = sd_bus_message_enter_container(message, 'r', "us");
            if (ret <= 0)
                break;
            uint32_t type = 0;
            char const* pattern = nullptr;
            if ((ret = sd_bus_message_read(message, "us", &type, &pattern)) < 0)
                return ret;
            // type 0 = glob；helper 会剥掉 "*." 只留扩展名给 LibGUI。
            if (type == 0 && pattern) {
                if (!first)
                    spec += ',';
                spec += pattern;
                first = false;
            }
            if ((ret = sd_bus_message_exit_container(message)) < 0)
                return ret;
        }
        if ((ret = sd_bus_message_exit_container(message)) < 0)
            return ret;
        if ((ret = sd_bus_message_exit_container(message)) < 0)
            return ret;
        if (!first)
            filters.push_back(spec);
    }

    return sd_bus_message_exit_container(message);
}

int parse_options(sd_bus_message* message, FileChooserOptions& options)
{
    int ret = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (ret < 0)
        return ret;

    for (;;) {
        ret = sd_bus_message_enter_container(message, 'e', "sv");
        if (ret <= 0)
            break;

        char const* key = nullptr;
        if ((ret = sd_bus_message_read(message, "s", &key)) < 0)
            return ret;

        if ((ret = sd_bus_message_enter_container(message, 'v', nullptr)) < 0)
            return ret;

        char type = 0;
        char const* signature = nullptr;
        if ((ret = sd_bus_message_peek_type(message, &type, &signature)) < 0)
            return ret;

        if (strcmp(key, "handle_token") == 0 && type == 's') {
            char const* value = nullptr;
            if ((ret = sd_bus_message_read(message, "s", &value)) < 0)
                return ret;
            options.handle_token = value ? value : "";
        } else if (strcmp(key, "current_name") == 0 && type == 's') {
            char const* value = nullptr;
            if ((ret = sd_bus_message_read(message, "s", &value)) < 0)
                return ret;
            options.current_name = value ? value : "";
        } else if (strcmp(key, "multiple") == 0 && type == 'b') {
            int value = 0;
            if ((ret = sd_bus_message_read(message, "b", &value)) < 0)
                return ret;
            options.multiple = value != 0;
        } else if (strcmp(key, "directory") == 0 && type == 'b') {
            int value = 0;
            if ((ret = sd_bus_message_read(message, "b", &value)) < 0)
                return ret;
            options.directory = value != 0;
        } else if (strcmp(key, "current_folder") == 0 && type == 'a') {
            // a{sv} 里是 NUL 结尾的字节数组，不是字符串。
            void const* data = nullptr;
            size_t size = 0;
            if ((ret = sd_bus_message_read_array(message, 'y', &data, &size)) < 0)
                return ret;
            options.current_folder.assign(static_cast<char const*>(data), size > 0 ? size - 1 : 0);
        } else if (strcmp(key, "filters") == 0 && type == 'a') {
            if ((ret = parse_filters(message, options.filters)) < 0)
                return ret;
        } else {
            if ((ret = sd_bus_message_skip(message, signature)) < 0)
                return ret;
        }

        if ((ret = sd_bus_message_exit_container(message)) < 0)
            return ret;
        if ((ret = sd_bus_message_exit_container(message)) < 0)
            return ret;
    }

    return sd_bus_message_exit_container(message);
}

// helper 需要的环境变量：D-Bus 激活出来的进程没有桌面环境（dbus-daemon 只带
// XDG_RUNTIME_DIR），缺 LD_LIBRARY_PATH 会导致 helper 连 liblagom 都找不到。
std::vector<std::string> helper_environment()
{
    struct Override {
        char const* key;
        char const* fallback;
    };
    static Override const overrides[] = {
        { "SERENITY_PREFIX", "/serenity" },
        { "SERENITY_BIN", "/serenity/bin" },
        { "SERENITY_RESOURCE_ROOT", "/res" },
        { "LD_LIBRARY_PATH", "/serenity/lib" },
        { "DISPLAY", ":100" },
        { "WAYLAND_DISPLAY", "wayland-serenity-0" },
        { "XDG_RUNTIME_DIR", "/run/user/1000" },
    };

    std::vector<std::string> environment;
    for (char** entry = environ; entry && *entry; ++entry) {
        bool shadowed = false;
        for (auto const& item : overrides) {
            auto length = strlen(item.key);
            if (strncmp(*entry, item.key, length) == 0 && (*entry)[length] == '=') {
                shadowed = true;
                break;
            }
        }
        if (!shadowed)
            environment.push_back(*entry);
    }
    for (auto const& item : overrides) {
        auto* existing = getenv(item.key);
        environment.push_back(std::string(item.key) + "=" + ((existing && *existing) ? existing : item.fallback));
    }
    return environment;
}

int spawn_picker(std::vector<std::string> const& arguments, pid_t& pid, int& output_fd)
{
    int pipe_fds[2] = { -1, -1 };
    if (pipe2(pipe_fds, O_CLOEXEC) < 0)
        return -errno;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    // helper 的 stderr 进日志，否则“对话框没弹出来”时无从下手。
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, PICKER_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto const& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    auto environment = helper_environment();
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1);
    for (auto& entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    int ret = posix_spawn(&pid, arguments.front().c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);

    if (ret != 0) {
        close(pipe_fds[0]);
        return -ret;
    }
    output_fd = pipe_fds[0];
    return 0;
}

void emit_response(PickerRequest* request, uint32_t code, std::string const& uri)
{
    sd_bus_message* signal_message = nullptr;
    if (sd_bus_message_new_signal(s_bus, &signal_message, request->path.c_str(), REQUEST_INTERFACE, "Response") < 0)
        return;

    // 关键：GTK 不给 Response 装 AddMatch 规则（实测只装了 portal 名字的
    // NameOwnerChanged），它靠 xdg-desktop-portal 的**单播**信号收结果。
    // 广播信号（destination=NULL）没有 match rule 就不会被投递，
    // 表现是「对话框关掉了，应用却永远在等」。
    if (!request->sender.empty())
        sd_bus_message_set_destination(signal_message, request->sender.c_str());

    sd_bus_message_append(signal_message, "u", code);
    sd_bus_message_open_container(signal_message, 'a', "{sv}");
    if (code == 0 && !uri.empty()) {
        sd_bus_message_open_container(signal_message, 'e', "sv");
        sd_bus_message_append(signal_message, "s", "uris");
        sd_bus_message_open_container(signal_message, 'v', "as");
        char const* uris[2] = { uri.c_str(), nullptr };
        sd_bus_message_append_strv(signal_message, const_cast<char**>(uris));
        sd_bus_message_close_container(signal_message);
        sd_bus_message_close_container(signal_message);
    }
    sd_bus_message_close_container(signal_message);

    sd_bus_send(s_bus, signal_message, nullptr);
    sd_bus_message_unref(signal_message);
}

int on_emit_response(sd_event_source* source, uint64_t, void* userdata)
{
    auto* request = static_cast<PickerRequest*>(userdata);
    request->defer_source = sd_event_source_unref(source);

    emit_response(request, request->response, request->uri);

    if (request->slot)
        request->slot = sd_bus_slot_unref(request->slot);
    delete request;
    return 0;
}

void finish_request(PickerRequest* request, uint32_t code, std::string const& uri)
{
    if (request->responded)
        return;
    request->responded = true;
    request->response = code;
    request->uri = uri;

    if (request->pid > 0 && !request->exited) {
        // 用户点了关闭 / 客户端发了 Close：把 helper 一起收掉，别留一个没人管的对话框。
        kill(request->pid, SIGTERM);
        request->pid = -1;
    }
    if (request->io_source)
        request->io_source = sd_event_source_unref(request->io_source);
    if (request->output_fd >= 0) {
        close(request->output_fd);
        request->output_fd = -1;
    }
    if (request->child_source)
        request->child_source = sd_event_source_unref(request->child_source);

    // 延迟 150ms 再发信号。真人操作时这个延迟根本不存在，但 helper 秒退时
    // （脚本化 helper、或文件选择器立刻失败）Response 会和 OpenFile 的应答抢在同
    // 一个主循环里出去，客户端还没来得及挂上 Response 的 match，信号就直接丢了，
    // 表现是“对话框关掉了但应用永远在等”。150ms 足够客户端处理完应答。
    uint64_t now = 0;
    sd_event_now(s_event, CLOCK_MONOTONIC, &now);
    sd_event_add_time(s_event, &request->defer_source, CLOCK_MONOTONIC, now + 300 * 1000, 0, on_emit_response, request);
}

void maybe_finish(PickerRequest* request)
{
    if (!request->exited || request->output_fd >= 0)
        return;

    std::string path = request->output;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();

    if (request->normal_exit && request->exit_status == 0 && !path.empty()) {
        finish_request(request, 0, file_uri(path));
        return;
    }
    if (request->normal_exit && request->exit_status == 1) {
        finish_request(request, 1, {}); // 用户取消
        return;
    }
    fprintf(stderr, "serenity-portal: file picker exited with status %d\n", request->exit_status);
    finish_request(request, 2, {});
}

int on_picker_output(sd_event_source* source, int fd, uint32_t, void* userdata)
{
    auto* request = static_cast<PickerRequest*>(userdata);
    char buffer[512];
    for (;;) {
        auto count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            if (request->output.size() < 4096)
                request->output.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        request->io_source = sd_event_source_unref(source);
        close(fd);
        request->output_fd = -1;
        maybe_finish(request);
        return 0;
    }
}

// 每 100ms 用 waitpid(WNOHANG) 看一眼。为什么不直接 sd_event_add_child：helper
// 可能在我们注册事件源之前就已经退出（脚本化 helper，或者 exec 失败立刻退出 127），
// 那时 SIGCHLD 早已过去，子进程事件源不会触发，请求会永远挂着。
int on_picker_poll(sd_event_source* source, uint64_t, void* userdata)
{
    auto* request = static_cast<PickerRequest*>(userdata);
    int status = 0;
    auto waited = waitpid(request->pid, &status, WNOHANG);
    if (waited == 0) {
        uint64_t now = 0;
        sd_event_now(s_event, CLOCK_MONOTONIC, &now);
        sd_event_source_set_time(source, now + 100 * 1000);
        return 1; // 保持启用，等下一轮
    }

    request->exited = true;
    if (waited == request->pid && WIFEXITED(status)) {
        request->normal_exit = true;
        request->exit_status = WEXITSTATUS(status);
    } else if (waited == request->pid) {
        request->normal_exit = false;
        request->exit_status = 1;
    } else {
        // ECHILD：已经被别处回收，当作失败处理。
        request->normal_exit = false;
        request->exit_status = 1;
    }
    request->child_source = sd_event_source_unref(source);
    maybe_finish(request);
    return 0;
}

int method_request_close(sd_bus_message* call, void* userdata, sd_bus_error*)
{
    auto* request = static_cast<PickerRequest*>(userdata);
    finish_request(request, 1, {});
    sd_bus_reply_method_return(call, nullptr);
    return 1;
}

sd_bus_vtable const request_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", method_request_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

// handle 路径必须和客户端自己算出来的一致，否则 it 会等一个永远不响的响应：
// /org/freedesktop/portal/desktop/request/<SENDER 去掉冒号、点换下划线>/<token>
std::string request_path_for(sd_bus_message* call, FileChooserOptions const& options)
{
    static unsigned s_counter = 0;
    char const* sender = sd_bus_message_get_sender(call);
    std::string cleaned = sender ? sender : ":0.0";
    if (!cleaned.empty() && cleaned.front() == ':')
        cleaned.erase(cleaned.begin());
    std::replace(cleaned.begin(), cleaned.end(), '.', '_');

    std::string token = options.handle_token;
    if (token.empty())
        token = "t" + std::to_string(++s_counter);

    return std::string(PORTAL_PATH) + "/request/" + cleaned + "/" + token;
}

int method_file_chooser(sd_bus_message* call, void*, sd_bus_error* error)
{
    char const* parent_window = nullptr;
    char const* title = nullptr;
    if (sd_bus_message_read(call, "ss", &parent_window, &title) < 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "malformed FileChooser call");
    (void)parent_window; // xdg-foreign 还没实现，先居中显示

    FileChooserOptions options;
    if (auto ret = parse_options(call, options); ret < 0)
        return ret;

    bool const save = strcmp(sd_bus_message_get_member(call), "SaveFile") == 0;

    std::vector<std::string> arguments { picker_binary(), save ? "save" : "open" };
    if (title && *title) {
        arguments.push_back("--title");
        arguments.push_back(title);
    }
    if (!options.current_folder.empty()) {
        arguments.push_back("--start");
        arguments.push_back(options.current_folder);
    }
    if (save) {
        if (!options.current_name.empty()) {
            arguments.push_back("--name");
            arguments.push_back(options.current_name);
        }
    } else if (options.directory) {
        arguments.push_back("--folder");
    }
    for (auto const& filter : options.filters) {
        arguments.push_back("--filter");
        arguments.push_back(filter);
    }

    auto* request = new PickerRequest;
    request->path = request_path_for(call, options);
    if (auto* sender = sd_bus_message_get_sender(call))
        request->sender = sender;

    if (auto ret = sd_bus_add_object_vtable(s_bus, &request->slot, request->path.c_str(), REQUEST_INTERFACE, request_vtable, request); ret < 0) {
        delete request;
        return ret;
    }

    pid_t pid = -1;
    int output_fd = -1;
    if (auto ret = spawn_picker(arguments, pid, output_fd); ret < 0) {
        fprintf(stderr, "serenity-portal: cannot spawn %s: %s\n", arguments.front().c_str(), strerror(-ret));
        finish_request(request, 2, {});
    } else {
        request->pid = pid;
        request->output_fd = output_fd;
        sd_event_add_io(s_event, &request->io_source, output_fd, EPOLLIN, on_picker_output, request);
        uint64_t now = 0;
        sd_event_now(s_event, CLOCK_MONOTONIC, &now);
        sd_event_add_time(s_event, &request->child_source, CLOCK_MONOTONIC, now + 100 * 1000, 0, on_picker_poll, request);
    }

    sd_bus_message* reply = nullptr;
    if (auto ret = sd_bus_message_new_method_return(call, &reply); ret < 0)
        return ret;
    sd_bus_message_append(reply, "o", request->path.c_str());
    sd_bus_send(s_bus, reply, nullptr);
    sd_bus_message_unref(reply);
    return 1;
}

// 版本号必须是 3：GTK 4.18 的 GtkFileDialog 会先读这个属性，小于 3 就直接
// 打回自带的对话框（连 OpenFile 都不发）。我们实现的 OpenFile/SaveFile 语义与
// v3 兼容，多选场景退化成单选（返回一个 uri），所以报 3 是诚实的。
int property_get_file_chooser_version(sd_bus*, char const*, char const*, char const*, sd_bus_message* reply, void*, sd_bus_error*)
{
    return sd_bus_message_append(reply, "u", 3u);
}

// GTK4（4.18+）在第一次用 portal 之前会调 org.freedesktop.host.portal.Registry.Register
// 上报自己的 app id。这一步失败它就**整个放弃 portal**，退回自带的文件对话框，
// 而且只留一行 warning —— 现象就是“配好了 portal 却还是弹 GTK 自己的窗口”。
// 我们没有沙箱、也不需要按 app 区分权限，登记一个空成功即可。
int method_registry_register(sd_bus_message* call, void*, sd_bus_error*)
{
    char const* app_id = nullptr;
    if (sd_bus_message_read(call, "s", &app_id) < 0)
        return sd_bus_error_set(nullptr, SD_BUS_ERROR_INVALID_ARGS, "malformed Register call");
    return sd_bus_reply_method_return(call, nullptr);
}

sd_bus_vtable const registry_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Register", "sa{sv}", "", method_registry_register, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

sd_bus_vtable const file_chooser_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", property_get_file_chooser_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("OpenFile", "ssa{sv}", "o", method_file_chooser, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile", "ssa{sv}", "o", method_file_chooser, SD_BUS_VTABLE_UNPRIVILEGED),
    // v3 的复数版本：同样是单选落地，先接受请求，别让客户端因为“方法不存在”而退回。
    SD_BUS_METHOD("OpenFiles", "ssa{sv}", "o", method_file_chooser, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "ssa{sv}", "o", method_file_chooser, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

sd_bus_vtable const settings_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReadOne", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReadAll", "as", "a{sa{sv}}", method_read_all, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

sd_bus_vtable const desktop_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetVersion", "", "a{sv}", method_get_version, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

}

int main()
{
    sd_bus* bus = nullptr;
    int ret = sd_bus_open_user(&bus);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot connect to the session bus: %s\n", strerror(-ret));
        return 1;
    }
    s_bus = bus;

    // FileChooser 需要等子进程（helper）、等它的管道，还要延迟发 Response，
    // 所以总线挂在 sd-event 上跑，而不是手写 process/wait 循环。
    if ((ret = sd_event_default(&s_event)) < 0) {
        fprintf(stderr, "serenity-portal: cannot create event loop: %s\n", strerror(-ret));
        return 1;
    }
    if ((ret = sd_bus_attach_event(bus, s_event, SD_EVENT_PRIORITY_NORMAL)) < 0) {
        fprintf(stderr, "serenity-portal: cannot attach bus to event loop: %s\n", strerror(-ret));
        return 1;
    }

    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, SETTINGS_INTERFACE, settings_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register Settings: %s\n", strerror(-ret));
        return 1;
    }
    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, HOST_REGISTRY_INTERFACE, registry_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register host portal Registry: %s\n", strerror(-ret));
        return 1;
    }
    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, FILE_CHOOSER_INTERFACE, file_chooser_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register FileChooser: %s\n", strerror(-ret));
        return 1;
    }
    ret = sd_bus_add_object_vtable(bus, nullptr, PORTAL_PATH, "org.freedesktop.portal.Desktop", desktop_vtable, nullptr);
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot register Desktop: %s\n", strerror(-ret));
        return 1;
    }

    // Claim the name before any client asks. xdg-desktop-portal is still
    // dbus-activatable in the distro, and it is not usable here, so whoever gets
    // here first has to win. If the system portal briefly holds the name before
    // our mask takes effect, retry for a bounded time instead of dying, so a
    // transient race does not spin the restart loop. If the name is genuinely
    // taken (mask failed), bow out quietly (exit 0) rather than crash.
    int attempts = 0;
    while ((ret = sd_bus_request_name(bus, PORTAL_NAME, 0)) == -EEXIST && attempts++ < 20) {
        // Another portal owns the name (normally prevented by our user-level
        // D-Bus .service file; otherwise a stale system portal). Retry briefly
        // but log only once so a losing race does not flood the session log.
        if (attempts == 1)
            fprintf(stderr, "serenity-portal: %s already owned, retrying quietly...\n", PORTAL_NAME);
        sleep(1);
    }
    if (ret == -EEXIST) {
        fprintf(stderr, "serenity-portal: %s owned by another portal, exiting\n", PORTAL_NAME);
        return 0;
    }
    if (ret < 0) {
        fprintf(stderr, "serenity-portal: cannot own %s: %s\n", PORTAL_NAME, strerror(-ret));
        return 1;
    }
    fprintf(stderr, "serenity-portal: serving %s + %s on %s\n", SETTINGS_INTERFACE, FILE_CHOOSER_INTERFACE, PORTAL_PATH);

    // 事件循环同时驱动总线消息、helper 的子进程退出和它的 stdout 管道。
    for (;;) {
        ret = sd_event_run(s_event, (uint64_t)-1);
        if (ret < 0) {
            fprintf(stderr, "serenity-portal: event loop error: %s\n", strerror(-ret));
            break;
        }
    }

    sd_event_unref(s_event);
    sd_bus_unref(bus);
    return 0;
}
