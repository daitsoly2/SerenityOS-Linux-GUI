/*
 * serenity-hostd 客户端（纯头文件，方便在不改 CMake 的前提下给 UsersSettings 复用）。
 *
 * GUI 应用以普通用户运行，所有会改动 Linux 账户的操作都通过 Unix socket 交给
 * root 守护 serenity-hostd 执行。这里只负责组包、收发与错误翻译，不包含任何
 * 系统账户写入逻辑 —— Core::Account::sync() 那套 Serenity 格式重刷
 * /etc/passwd|group|shadow 的做法在 Linux 上必须彻底停用。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace UsersSettings {

constexpr StringView HOSTD_SOCKET_PATH = "/run/serenity-hostd/socket"sv;

struct HostdReply {
    bool ok { false };
    ByteString error;
    Vector<ByteString> rows;
};

// 字段里可能有中文、制表符或换行，统一 hex 编码后走协议。
inline ByteString hostd_hex(StringView value)
{
    StringBuilder builder;
    for (size_t i = 0; i < value.length(); ++i) {
        auto byte = static_cast<unsigned char>(value.characters_without_null_termination()[i]);
        builder.appendff("{:02x}", byte);
    }
    return builder.to_byte_string();
}

// AK::Error::from_string_view() 只保存视图，不拷贝字符串。代理返回的错误是
// 局部变量，这里放进进程级缓冲，保证 Error 引用的内存在整个生命周期有效。
inline Error hostd_failure(StringView message)
{
    static ByteString s_last_error;
    s_last_error = message;
    return Error::from_string_view(s_last_error.view());
}

inline ErrorOr<HostdReply> hostd_call(StringView request)
{
    if (HOSTD_SOCKET_PATH.length() >= sizeof(sockaddr_un::sun_path))
        return Error::from_string_literal("hostd socket 路径过长");

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return Error::from_syscall("socket"sv, -errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, HOSTD_SOCKET_PATH.characters_without_null_termination(), HOSTD_SOCKET_PATH.length());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        int saved_errno = errno;
        ::close(fd);
        return Error::from_syscall("connect hostd"sv, -saved_errno);
    }

    auto line = ByteString::formatted("{}\n", request);
    auto written = ::send(fd, line.characters(), line.length(), MSG_NOSIGNAL);
    if (written < 0 || static_cast<size_t>(written) != line.length()) {
        ::close(fd);
        return Error::from_string_literal("向 hostd 发送请求失败");
    }

    // 守护处理完一个请求就关连接，所以读到 EOF 即为完整应答。
    StringBuilder builder;
    char chunk[1024];
    for (;;) {
        auto count = ::recv(fd, chunk, sizeof(chunk), 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return Error::from_string_literal("读取 hostd 应答失败");
        }
        if (count == 0)
            break;
        builder.append(StringView { chunk, static_cast<size_t>(count) });
        if (builder.length() > (1 << 20)) {
            ::close(fd);
            return Error::from_string_literal("hostd 应答过大");
        }
    }
    ::close(fd);

    auto lines = builder.string_view().split_view('\n');
    if (lines.is_empty())
        return Error::from_string_literal("hostd 应答为空");

    HostdReply reply;
    if (lines[0] == "ok"sv) {
        reply.ok = true;
        for (size_t i = 1; i < lines.size(); ++i) {
            if (lines[i] == "."sv)
                break;
            if (!lines[i].is_empty())
                reply.rows.append(ByteString { lines[i] });
        }
        return reply;
    }
    if (lines[0].starts_with("err "sv)) {
        reply.error = lines[0].substring_view(4);
        return reply;
    }
    return Error::from_string_literal("hostd 应答格式不合法");
}

inline ErrorOr<Vector<ByteString>> hostd_list_users()
{
    auto reply = TRY(hostd_call("users.list"sv));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return reply.rows;
}

inline ErrorOr<void> hostd_ping()
{
    auto reply = TRY(hostd_call("ping"sv));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return {};
}

inline ErrorOr<void> hostd_add_user(StringView username, StringView full_name, bool administrator, StringView password)
{
    auto request = ByteString::formatted("users.add\t{}\t{}\t{}\t{}",
        username, hostd_hex(full_name), administrator ? 1 : 0, hostd_hex(password));
    auto reply = TRY(hostd_call(request));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return {};
}

inline ErrorOr<void> hostd_delete_user(StringView username)
{
    auto request = ByteString::formatted("users.delete\t{}", username);
    auto reply = TRY(hostd_call(request));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return {};
}

inline ErrorOr<void> hostd_modify_user(StringView username, StringView full_name, StringView shell, bool administrator)
{
    auto request = ByteString::formatted("users.modify\t{}\t{}\t{}\t{}",
        username, hostd_hex(full_name), hostd_hex(shell), administrator ? 1 : 0);
    auto reply = TRY(hostd_call(request));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return {};
}

inline ErrorOr<void> hostd_set_password(StringView username, StringView password)
{
    auto request = ByteString::formatted("users.passwd\t{}\t{}", username, hostd_hex(password));
    auto reply = TRY(hostd_call(request));
    if (!reply.ok)
        return hostd_failure(reply.error);
    return {};
}

}
