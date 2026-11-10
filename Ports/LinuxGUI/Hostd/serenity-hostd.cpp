/*
 * serenity-hostd —— Linux 宿主访问代理（root 守护进程），C++ 版本。
 *
 * 为什么要有这层：SerenityOS 的设置类应用是按 Serenity 内核语义写的 —— 直接重写
 * /etc/passwd、调用 /bin/useradd、用 /bin/Escalator 提权。这些在 Linux 宿主上全都
 * 不成立，而 Core::Account::sync() 会用 Serenity 的格式重刷 passwd/group/shadow，
 * 足以破坏系统登录。于是把所有「碰 Linux 账户」的操作收敛到一个 root 守护：
 * GUI 以普通用户运行，通过 Unix socket 发能力化请求，守护做校验后调用 Linux 原生命令。
 *
 * 相对旧的 Python 实现（serenity-hostd.py）的差异：
 *   1. 请求行不再 strip() —— Python 的 str.strip() 会把「尾随的空字段」一起吃掉，
 *      于是空密码的 users.add 从 5 个字段变 4 个，GUI 上表现为
 *      "Failed to add user: 参数个数不对"。这里只裁掉行尾的 \r\n。
 *   2. add_user 在 chpasswd 失败时回滚刚建的账户，不留「无密码的半成品用户」。
 *   3. 不依赖 python3 解释器，部署成单一 ELF。
 *
 * 协议（一行请求，字段用 \t 分隔，可能含特殊字符的字段用 hex 编码）：
 *    ping
 *    users.list
 *    users.add\t<username>\t<full_name_hex>\t<admin 0|1>\t<password_hex>
 *    users.delete\t<username>
 *    users.modify\t<username>\t<full_name_hex>\t<shell_hex>\t<admin 0|1>
 *    users.passwd\t<username>\t<password_hex>
 *
 * 应答：
 *    ok\n                       无返回值
 *    ok\n<row>\n<row>\n.\n      users.list，每行 username\tuid\tgecos\tshell\tgroups
 *    err <原因>\n
 *
 * 安全边界：socket 只对桌面用户组可写，且用 SO_PEERCRED 校验对端 uid；
 * 所有参数走 argv 数组（不经过 shell），密码走 chpasswd 的 stdin（不进 argv）。
 * 这是「单用户桌面上的本机管理员」模型，不是多用户提权通道；若以后要放开，
 * 应在这里接 Polkit 而不是放宽 uid 校验。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

char const* DEFAULT_SOCKET_DIR = "/run/serenity-hostd";

char const* USERADD = "/usr/sbin/useradd";
char const* USERDEL = "/usr/sbin/userdel";
char const* USERMOD = "/usr/sbin/usermod";
char const* CHPASSWD = "/usr/sbin/chpasswd";
char const* GPASSWD = "/usr/bin/gpasswd";

char const* DEFAULT_SHELL = "/bin/bash";

// Debian 普通用户从 1000 起，低于此值一律视为系统账户；
// 60000 是 /etc/login.defs 的 UID_MAX，之上的 nobody(65534) 等不是可管理账户。
constexpr unsigned MIN_UID = 1000;
constexpr unsigned MAX_UID = 60000;

// 新用户默认加入的组：只挑宿主机上真实存在的，避免 useradd -G 因缺组失败。
char const* CANDIDATE_GROUPS[] = { "users", "audio", "video", "render", "input", "lpadmin" };
// Debian 用 sudo，老发行版用 wheel。
char const* ADMIN_GROUPS[] = { "sudo", "wheel" };

constexpr size_t MAX_REQUEST_BYTES = 8192;
constexpr int COMMAND_TIMEOUT_MS = 30000;

volatile sig_atomic_t s_running = 1;

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

// 可以直接回给 GUI 的错误（不含敏感信息）。
struct HostdError {
    std::string message;
};

[[noreturn]] void fail(std::string message)
{
    throw HostdError { move(message) };
}

void audit(char const* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("hostd: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    fflush(stderr);
}

// 只裁掉行尾的 CR/LF。绝对不能像 Python 那样用 strip()：tab 也是空白，
// 尾随的空字段（例如空密码）会被一并吃掉，字段数就错了。
std::string_view trim_line_end(std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);
    return line;
}

std::vector<std::string_view> split_on_tabs(std::string_view line)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        auto position = line.find('\t', start);
        if (position == std::string_view::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, position - start));
        start = position + 1;
    }
}

bool is_hex_digit(char character)
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
}

unsigned hex_value(char character)
{
    if (character >= '0' && character <= '9')
        return static_cast<unsigned>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<unsigned>(character - 'a') + 10;
    return static_cast<unsigned>(character - 'A') + 10;
}

bool valid_utf8(std::string_view text)
{
    size_t index = 0;
    while (index < text.size()) {
        auto byte = static_cast<unsigned char>(text[index]);
        size_t extra = 0;
        unsigned minimum_code_point = 0;
        unsigned code_point = 0;
        if (byte < 0x80) {
            ++index;
            continue;
        } else if ((byte & 0xE0) == 0xC0) {
            extra = 1;
            minimum_code_point = 0x80;
            code_point = byte & 0x1F;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
            minimum_code_point = 0x800;
            code_point = byte & 0x0F;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
            minimum_code_point = 0x10000;
            code_point = byte & 0x07;
        } else {
            return false;
        }
        if (index + extra >= text.size())
            return false;
        for (size_t offset = 1; offset <= extra; ++offset) {
            auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80)
                return false;
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if (code_point < minimum_code_point || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
            return false;
        index += extra + 1;
    }
    return true;
}

std::string decode_hex(std::string_view value, char const* field)
{
    if (value.size() % 2 != 0)
        fail(std::string(field) + " 编码不合法");
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2) {
        if (!is_hex_digit(value[index]) || !is_hex_digit(value[index + 1]))
            fail(std::string(field) + " 编码不合法");
        decoded.push_back(static_cast<char>((hex_value(value[index]) << 4) | hex_value(value[index + 1])));
    }
    if (!valid_utf8(decoded))
        fail(std::string(field) + " 编码不合法");
    return decoded;
}

// 列表应答是 \t 分隔的纯文本，字段里的控制字符会把行结构撑坏。
std::string sanitize_field(std::string_view value)
{
    std::string result(value);
    for (auto& character : result) {
        if (character == '\t' || character == '\n' || character == '\r')
            character = ' ';
    }
    return result;
}

void validate_username(std::string_view username)
{
    // 等价于 \A[a-z_][a-z0-9_-]{0,31}\Z
    bool ok = !username.empty() && username.size() <= 32;
    if (ok) {
        auto first = username.front();
        ok = (first >= 'a' && first <= 'z') || first == '_';
    }
    for (size_t index = ok ? 1 : username.size(); index < username.size(); ++index) {
        auto character = username[index];
        bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (!allowed) {
            ok = false;
            break;
        }
    }
    if (!ok)
        fail("用户名必须由小写字母/数字/下划线/连字符组成，且以字母或下划线开头");

    static char const* reserved[] = { "root", "daemon", "bin", "sys", "sync", "nobody" };
    for (auto name : reserved) {
        if (username == name)
            fail("不允许操作系统保留账户");
    }
}

bool is_manageable_uid(unsigned uid)
{
    return uid >= MIN_UID && uid <= MAX_UID;
}

passwd const& require_manageable_account(std::string const& username)
{
    validate_username(username);
    auto* entry = getpwnam(username.c_str());
    if (!entry)
        fail("用户不存在");
    if (!is_manageable_uid(entry->pw_uid))
        fail("不允许修改系统账户");
    return *entry;
}

void validate_password(std::string_view password)
{
    if (password.empty())
        fail("密码不能为空");
    if (password.find('\n') != std::string_view::npos || password.find('\r') != std::string_view::npos || password.find('\0') != std::string_view::npos)
        fail("密码含非法字符");
}

void validate_full_name(std::string_view full_name)
{
    for (auto character : full_name) {
        if (character == '\n' || character == '\r' || character == '\0' || character == ':')
            fail("全名含非法字符");
    }
}

bool group_exists(std::string const& name)
{
    return getgrnam(name.c_str()) != nullptr;
}

std::vector<std::string> existing_groups(std::vector<char const*> const& names)
{
    std::vector<std::string> result;
    for (auto name : names) {
        if (group_exists(name))
            result.push_back(name);
    }
    return result;
}

std::string first_admin_group()
{
    auto groups = existing_groups({ ADMIN_GROUPS[0], ADMIN_GROUPS[1] });
    if (groups.empty())
        fail("宿主机上没有 sudo/wheel 管理员组");
    return groups.front();
}

bool user_in_group(std::string const& username, std::string const& group)
{
    auto* entry = getgrnam(group.c_str());
    if (!entry)
        return false;
    for (auto* member = entry->gr_mem; member && *member; ++member) {
        if (username == *member)
            return true;
    }
    return false;
}

bool shell_is_valid(std::string const& shell)
{
    FILE* file = fopen("/etc/shells", "r");
    if (!file)
        return shell == DEFAULT_SHELL;
    char buffer[512];
    bool found = false;
    while (fgets(buffer, sizeof(buffer), file)) {
        std::string_view line(buffer);
        line = trim_line_end(line);
        if (line.empty() || line.front() == '#')
            continue;
        if (line == shell) {
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

std::string last_non_empty_line(std::string const& text)
{
    std::string result;
    size_t position = 0;
    while (position < text.size()) {
        auto end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        std::string_view line(text.data() + position, end - position);
        line = trim_line_end(line);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (!line.empty())
            result = std::string(line);
        position = end + 1;
    }
    return result;
}

// ---------------------------------------------------------------------------
// 子进程执行：argv 数组，绝不经过 shell
// ---------------------------------------------------------------------------

std::string run(std::vector<std::string> const& arguments, std::optional<std::string> const& input = {})
{
    if (arguments.empty())
        fail("内部错误: 空命令");

    std::string const program = arguments.front();
    char const* base_name = strrchr(program.c_str(), '/');
    base_name = base_name ? base_name + 1 : program.c_str();

    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    int in_pipe[2] = { -1, -1 };
    if (pipe2(out_pipe, O_CLOEXEC) < 0)
        fail(std::string(base_name) + " 无法执行: " + strerror(errno));
    if (pipe2(err_pipe, O_CLOEXEC) < 0) {
        int saved = errno;
        close(out_pipe[0]);
        close(out_pipe[1]);
        fail(std::string(base_name) + " 无法执行: " + strerror(saved));
    }
    if (input.has_value() && pipe2(in_pipe, O_CLOEXEC) < 0) {
        int saved = errno;
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        fail(std::string(base_name) + " 无法执行: " + strerror(saved));
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto const& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        for (int fd : { out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1] }) {
            if (fd >= 0)
                close(fd);
        }
        fail(std::string(base_name) + " 无法执行: " + strerror(saved));
    }

    if (pid == 0) {
        // 子进程：接管 0/1/2 后只留 exec。这里不能抛异常，出错直接 _exit。
        if (in_pipe[0] >= 0) {
            if (dup2(in_pipe[0], STDIN_FILENO) < 0)
                _exit(127);
        } else {
            // 没有 stdin 输入时接 /dev/null，别让命令等着继承来的终端。
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                if (dup2(devnull, STDIN_FILENO) < 0)
                    _exit(127);
                if (devnull > STDERR_FILENO)
                    close(devnull);
            }
        }
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (dup2(err_pipe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(out_pipe[0]);
        close(err_pipe[0]);
        if (in_pipe[1] >= 0)
            close(in_pipe[1]);
        execv(program.c_str(), argv.data());
        fprintf(stderr, "execv %s: %s\n", program.c_str(), strerror(errno));
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    // 父进程只往子进程的 stdin 里写：读端已经 dup2 到子进程，这里必须立刻关掉
    // 自己的副本，否则（比如 chpasswd）读端永远不会 EOF，子进程会一直等下去。
    if (in_pipe[0] >= 0)
        close(in_pipe[0]);
    in_pipe[0] = -1;

    if (input.has_value() && in_pipe[1] >= 0) {
        // 输入很小（一行 chpasswd），管道缓冲区足够，写不进去说明子进程已死。
        std::string_view payload = *input;
        while (!payload.empty()) {
            auto written = write(in_pipe[1], payload.data(), payload.size());
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            payload.remove_prefix(static_cast<size_t>(written));
        }
        close(in_pipe[1]);
        in_pipe[1] = -1;
    }

    auto monotonic_ms = [] {
        timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    };
    auto const started = monotonic_ms();
    bool timed_out = false;
    std::string out;
    std::string err;
    bool out_open = true;
    bool err_open = true;
    int status = 0;
    bool reaped = false;

    for (;;) {
        // 1) 收数据：非阻塞读，读空即视为该管道结束。
        pollfd descriptors[2];
        int count = 0;
        if (out_open)
            descriptors[count++] = { out_pipe[0], POLLIN, 0 };
        if (err_open)
            descriptors[count++] = { err_pipe[0], POLLIN, 0 };

        if (count > 0) {
            int ready = poll(descriptors, static_cast<nfds_t>(count), 0);
            if (ready > 0) {
                for (int index = 0; index < count; ++index) {
                    if (!(descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)))
                        continue;
                    char buffer[4096];
                    auto got = read(descriptors[index].fd, buffer, sizeof(buffer));
                    if (got < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                            continue;
                        got = 0;
                    }
                    if (got == 0) {
                        if (descriptors[index].fd == out_pipe[0])
                            out_open = false;
                        else
                            err_open = false;
                        continue;
                    }
                    auto& target = descriptors[index].fd == out_pipe[0] ? out : err;
                    if (target.size() < (1 << 20))
                        target.append(buffer, static_cast<size_t>(got));
                }
            }
        }

        // 2) 收割子进程。注意：两个管道都关了但子进程还没退时，
        //    count == 0，不能拿 poll(0 个 fd, 剩余时间) 去睡，那会白等满 30 秒。
        if (!reaped) {
            auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno != EINTR))
                reaped = true;
        }
        if (reaped && !out_open && !err_open)
            break;

        // 3) 超时保护：卡住的命令不能把整个守护（以及所有设置页）拖死。
        if (monotonic_ms() - started > COMMAND_TIMEOUT_MS) {
            timed_out = true;
            kill(pid, SIGKILL);
            break;
        }

        // 4) 等下一轮事件。仍有管道可读就阻塞在 poll 上，否则小睡一下等 waitpid。
        if (count > 0) {
            poll(descriptors, static_cast<nfds_t>(count), 50);
        } else {
            usleep(2000);
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    if (!reaped) {
        int ignored = 0;
        waitpid(pid, &ignored, 0);
        if (timed_out)
            fail(std::string(base_name) + " 执行超时");
        return out;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        auto detail = last_non_empty_line(err);
        if (detail.empty())
            detail = last_non_empty_line(out);
        if (detail.empty())
            detail = std::string(base_name) + " 返回 " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        fail(detail);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 能力实现
// ---------------------------------------------------------------------------

unsigned allowed_uid()
{
    auto* value = getenv("SERENITY_HOSTD_ALLOWED_UID");
    if (!value || !*value)
        return 1000;
    return static_cast<unsigned>(strtoul(value, nullptr, 10));
}

std::vector<std::string> list_users()
{
    struct Row {
        unsigned uid;
        std::string line;
    };
    std::vector<Row> rows;

    setpwent();
    while (auto* entry = getpwent()) {
        if (!is_manageable_uid(entry->pw_uid))
            continue;

        std::vector<std::string> groups;
        setgrent();
        while (auto* group = getgrent()) {
            for (auto* member = group->gr_mem; member && *member; ++member) {
                if (entry->pw_name && std::string_view(*member) == entry->pw_name) {
                    groups.push_back(sanitize_field(group->gr_name));
                    break;
                }
            }
        }
        endgrent();

        std::string line = sanitize_field(entry->pw_name ? entry->pw_name : "");
        line += '\t';
        line += std::to_string(entry->pw_uid);
        line += '\t';
        line += sanitize_field(entry->pw_gecos ? entry->pw_gecos : "");
        line += '\t';
        line += sanitize_field(entry->pw_shell ? entry->pw_shell : "");
        line += '\t';
        for (size_t index = 0; index < groups.size(); ++index) {
            if (index)
                line += ',';
            line += groups[index];
        }
        rows.push_back({ entry->pw_uid, move(line) });
    }
    endpwent();

    std::stable_sort(rows.begin(), rows.end(), [](Row const& left, Row const& right) { return left.uid < right.uid; });

    std::vector<std::string> result;
    result.reserve(rows.size());
    for (auto& row : rows)
        result.push_back(move(row.line));
    return result;
}

void add_user(std::string const& username, std::string_view full_name_hex, std::string const& admin, std::string_view password_hex)
{
    validate_username(username);
    if (getpwnam(username.c_str()))
        fail("用户已存在");

    auto full_name = decode_hex(full_name_hex, "全名");
    auto password = decode_hex(password_hex, "密码");
    validate_password(password);
    validate_full_name(full_name);
    if (admin != "0" && admin != "1")
        fail("账户类型不合法");

    auto groups = existing_groups({ CANDIDATE_GROUPS[0], CANDIDATE_GROUPS[1], CANDIDATE_GROUPS[2],
        CANDIDATE_GROUPS[3], CANDIDATE_GROUPS[4], CANDIDATE_GROUPS[5] });
    if (admin == "1") {
        auto admin_group = first_admin_group();
        if (std::find(groups.begin(), groups.end(), admin_group) == groups.end())
            groups.push_back(admin_group);
    }

    std::vector<std::string> arguments { USERADD, "--create-home", "--shell", DEFAULT_SHELL };
    if (!full_name.empty()) {
        arguments.push_back("--comment");
        arguments.push_back(full_name);
    }
    if (!groups.empty()) {
        std::string joined;
        for (size_t index = 0; index < groups.size(); ++index) {
            if (index)
                joined += ',';
            joined += groups[index];
        }
        arguments.push_back("--groups");
        arguments.push_back(move(joined));
    }
    arguments.push_back(username);
    run(arguments);

    // 密码只走 stdin，避免出现在 argv / ps 里。
    try {
        run({ CHPASSWD }, username + ":" + password + "\n");
    } catch (HostdError const& error) {
        // 建号成功但设密码失败: 不能留一个没有密码的半成品账户。
        audit("users.add %s 设密码失败，回滚: %s", username.c_str(), error.message.c_str());
        try {
            run({ USERDEL, "--remove", username });
        } catch (HostdError const& rollback_error) {
            audit("users.add 回滚失败: %s", rollback_error.message.c_str());
        }
        throw;
    }

    std::string joined_groups;
    for (size_t index = 0; index < groups.size(); ++index) {
        if (index)
            joined_groups += ',';
        joined_groups += groups[index];
    }
    audit("users.add %s admin=%s groups=%s", username.c_str(), admin.c_str(), joined_groups.c_str());
}

void delete_user(std::string const& username)
{
    auto const& entry = require_manageable_account(username);
    if (entry.pw_uid == allowed_uid())
        fail("不能删除当前桌面会话用户");
    run({ USERDEL, "--remove", username });
    audit("users.delete %s", username.c_str());
}

void modify_user(std::string const& username, std::string_view full_name_hex, std::string_view shell_hex, std::string const& admin)
{
    auto const& entry = require_manageable_account(username);
    auto full_name = decode_hex(full_name_hex, "全名");
    auto shell = decode_hex(shell_hex, "登录 Shell");
    if (shell.empty() && entry.pw_shell)
        shell = entry.pw_shell;
    validate_full_name(full_name);
    if (!shell_is_valid(shell))
        fail("登录 Shell 不在 /etc/shells 中");
    if (admin != "0" && admin != "1")
        fail("账户类型不合法");

    run({ USERMOD, "--comment", full_name, "--shell", shell, username });

    // 管理员资格用 gpasswd 增删：不动用户原有的补充组，避免 usermod -G 覆盖。
    auto admin_group = first_admin_group();
    bool is_member = user_in_group(username, admin_group);
    if (admin == "1" && !is_member)
        run({ GPASSWD, "--add", username, admin_group });
    else if (admin == "0" && is_member)
        run({ GPASSWD, "--delete", username, admin_group });

    audit("users.modify %s shell=%s admin=%s", username.c_str(), shell.c_str(), admin.c_str());
}

void set_password(std::string const& username, std::string_view password_hex)
{
    require_manageable_account(username);
    auto password = decode_hex(password_hex, "密码");
    validate_password(password);
    run({ CHPASSWD }, username + ":" + password + "\n");
    audit("users.passwd %s", username.c_str());
}

// ---------------------------------------------------------------------------
// 协议分发
// ---------------------------------------------------------------------------

std::vector<std::string> dispatch(std::string_view line)
{
    auto parts = split_on_tabs(line);
    auto command = parts.front();

    static char const* known[] = { "ping", "users.list", "users.add", "users.delete", "users.modify", "users.passwd" };
    bool recognized = false;
    for (auto name : known) {
        if (command == name) {
            recognized = true;
            break;
        }
    }
    if (!recognized)
        fail("未知能力: " + std::string(command));

    if (command == "ping" && parts.size() == 1)
        return {};
    if (command == "users.list" && parts.size() == 1)
        return list_users();
    if (command == "users.add" && parts.size() == 5) {
        add_user(std::string(parts[1]), parts[2], std::string(parts[3]), parts[4]);
        return {};
    }
    if (command == "users.delete" && parts.size() == 2) {
        delete_user(std::string(parts[1]));
        return {};
    }
    if (command == "users.modify" && parts.size() == 5) {
        modify_user(std::string(parts[1]), parts[2], parts[3], std::string(parts[4]));
        return {};
    }
    if (command == "users.passwd" && parts.size() == 3) {
        set_password(std::string(parts[1]), parts[2]);
        return {};
    }
    fail("参数个数不对");
}

bool send_all(int fd, std::string_view payload)
{
    while (!payload.empty()) {
        auto written = send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        payload.remove_prefix(static_cast<size_t>(written));
    }
    return true;
}

void reply(int fd, std::vector<std::string> const* rows, char const* error)
{
    if (error) {
        send_all(fd, std::string("err ") + error + "\n");
        return;
    }
    if (rows && !rows->empty()) {
        std::string body = "ok\n";
        for (auto const& row : *rows) {
            body += row;
            body += '\n';
        }
        body += ".\n";
        send_all(fd, body);
        return;
    }
    send_all(fd, "ok\n");
}

void handle_connection(int fd)
{
    // 只有桌面会话用户（和 root 自己）能调用；否则等价于本地提权。
    unsigned uid = static_cast<unsigned>(-1);
    ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0)
        uid = credentials.uid;
    if (uid != 0 && uid != allowed_uid()) {
        audit("拒绝 uid=%u 的连接", uid);
        reply(fd, nullptr, "无权访问");
        return;
    }

    std::string buffer;
    while (buffer.find('\n') == std::string::npos && buffer.size() < MAX_REQUEST_BYTES) {
        char chunk[1024];
        auto got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (got == 0)
            break;
        buffer.append(chunk, static_cast<size_t>(got));
    }

    auto line = trim_line_end(std::string_view(buffer).substr(0, buffer.find('\n')));
    if (line.empty())
        return;

    std::string command(line.substr(0, line.find('\t')));
    audit("请求 %s uid=%u", command.c_str(), uid);

    try {
        auto rows = dispatch(line);
        reply(fd, &rows, nullptr);
    } catch (HostdError const& error) {
        audit("%s 失败: %s", command.c_str(), error.message.c_str());
        reply(fd, nullptr, error.message.c_str());
    } catch (std::exception const& error) {
        // 不能让一个坏请求把守护打死。
        audit("内部错误: %s", error.what());
        reply(fd, nullptr, "代理内部错误");
    }
}

void on_signal(int)
{
    s_running = 0;
}

char const* socket_directory()
{
    auto* value = getenv("SERENITY_HOSTD_SOCKET_DIR");
    return (value && *value) ? value : DEFAULT_SOCKET_DIR;
}

} // namespace

int main()
{
    if (geteuid() != 0) {
        fputs("serenity-hostd 必须以 root 运行\n", stderr);
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    std::string directory = socket_directory();
    std::string path = directory + "/socket";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        fputs("serenity-hostd socket 路径过长\n", stderr);
        return 1;
    }

    // 不留在调用者的工作目录里。
    if (chdir("/") < 0) {
        audit("chdir 失败: %s", strerror(errno));
    }

    if (mkdir(directory.c_str(), 0750) < 0 && errno != EEXIST) {
        audit("无法创建 %s: %s", directory.c_str(), strerror(errno));
        return 1;
    }
    auto desktop_uid = allowed_uid();
    gid_t desktop_gid = 0;
    if (auto* entry = getpwuid(desktop_uid))
        desktop_gid = entry->pw_gid;
    if (chown(directory.c_str(), 0, desktop_gid) < 0)
        audit("chown %s 失败: %s", directory.c_str(), strerror(errno));
    if (chmod(directory.c_str(), 0750) < 0)
        audit("chmod %s 失败: %s", directory.c_str(), strerror(errno));

    unlink(path.c_str());

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        audit("socket 失败: %s", strerror(errno));
        return 1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.data(), path.size());
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        audit("bind %s 失败: %s", path.c_str(), strerror(errno));
        close(server);
        return 1;
    }
    if (chown(path.c_str(), 0, desktop_gid) < 0)
        audit("chown %s 失败: %s", path.c_str(), strerror(errno));
    if (chmod(path.c_str(), 0660) < 0)
        audit("chmod %s 失败: %s", path.c_str(), strerror(errno));
    if (listen(server, 8) < 0) {
        audit("listen 失败: %s", strerror(errno));
        close(server);
        unlink(path.c_str());
        return 1;
    }

    timeval accept_timeout { 1, 0 };
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));

    audit("就绪 uid=%u socket=%s", desktop_uid, path.c_str());

    while (s_running) {
        int connection = accept(server, nullptr, nullptr);
        if (connection < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            audit("accept 失败: %s", strerror(errno));
            break;
        }
        timeval receive_timeout { 10, 0 };
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        handle_connection(connection);
        close(connection);
    }

    close(server);
    unlink(path.c_str());
    audit("退出");
    return 0;
}
