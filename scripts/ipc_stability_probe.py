#!/usr/bin/env python3
"""WindowServer 稳定性探针：用裸 IPC 向 /tmp/portal/window 发送恶意参数。

用途：验证 ConnectionFromClient 的「不信任客户端数字」加固是否生效。
预期：服务端打印 "misbehaved (...)"，断开本连接，WindowServer 进程保持存活。

帧格式（LibIPC/Message.cpp + WindowServerEndpoint.h）：
    u32 payload_size | u32 endpoint_magic | i32 message_id | 参数...
参数按 IPC::Encoder 的顺序小端裸编码；IPC::File 不占字节，通过 SCM_RIGHTS 传 fd。

用法：
    python3 ipc_stability_probe.py drag        # 无拖放时发 SetAcceptsDrag
    python3 ipc_stability_probe.py huge_rect   # CreateWindow 带 INT_MAX 矩形
    python3 ipc_stability_probe.py huge_min    # SetWindowMinimumSize 带 INT_MAX
    python3 ipc_stability_probe.py bad_pitch   # 后备存储 pitch < width*4
    python3 ipc_stability_probe.py short_fd    # 后备存储 fd 小于 pitch*height
    python3 ipc_stability_probe.py all         # 依次跑全部用例
"""

import os
import socket
import struct
import sys
import time

SOCKET_PATH = "/tmp/portal/window"
ENDPOINT_MAGIC = 2938215075

# 生成头 Build/lagom/Userland/Services/WindowServer/WindowServerEndpoint.h 里的 MessageID
MSG = {
    "CreateWindow": 11,
    "SetWindowMinimumSize": 27,
    "SetWindowBackingStore": 43,
    "SetAcceptsDrag": 74,
}


def i32(value):
    return struct.pack("<i", value)


def boolean(value):
    return struct.pack("<?", bool(value))


def flt(value):
    return struct.pack("<f", value)


def size(width, height):
    return i32(width) + i32(height)


def rect(x, y, width, height):
    return i32(x) + i32(y) + i32(width) + i32(height)


def byte_string(value):
    if isinstance(value, str):
        value = value.encode()
    return struct.pack("<I", len(value)) + value


def optional_size(value=None):
    if value is None:
        return boolean(False)
    return boolean(True) + size(*value)


def frame(message_id, payload):
    body = struct.pack("<I", ENDPOINT_MAGIC) + i32(message_id) + payload
    return struct.pack("<I", len(body)) + body


def connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(2)
    sock.connect(SOCKET_PATH)
    return sock


def send(sock, message_id, payload, fd=None):
    data = frame(message_id, payload)
    if fd is None:
        sock.sendall(data)
    else:
        sock.sendmsg(
            [data],
            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))],
        )


def create_window_payload(window_id, window_rect=(100, 100, 200, 120), minimum=(0, 0)):
    """构造一个合法的 CreateWindow，作为后续用例的前置窗口。"""
    flags = [
        0,  # auto_position
        0,  # has_alpha_channel
        1,  # minimizable
        1,  # closeable
        1,  # resizable
        0,  # fullscreen
        0,  # frameless
        0,  # forced_shadow
    ]
    return b"".join([
        i32(window_id),
        i32(os.getpid()),
        rect(*window_rect),
        *(boolean(f) for f in flags),
        flt(0.0),           # alpha_hit_threshold
        size(0, 0),         # base_size
        size(0, 0),         # size_increment
        size(*minimum),     # minimum_size
        optional_size(),    # resize_aspect_ratio
        i32(0),             # type = Normal
        i32(0),             # mode = NonBlocking
        byte_string("stability-probe"),
        i32(0),             # parent_window_id = 无
        rect(0, 0, 0, 0),  # launch_origin_rect
    ])


def backing_store_payload(window_id, pitch, bitmap_size=(100, 100), fd_bytes=65536):
    """构造 SetWindowBackingStore，并返回一个 ftruncate 过的 memfd。"""
    fd = os.memfd_create("stability-probe", 0)
    os.ftruncate(fd, fd_bytes)
    payload = b"".join([
        i32(window_id),
        i32(32),                 # bpp
        i32(pitch),
        # IPC::File 在这里不占字节，fd 走 SCM_RIGHTS
        i32(0),                  # serial
        boolean(False),          # has_alpha_channel
        size(*bitmap_size),
        size(*bitmap_size),      # visible_size
        boolean(False),          # flush_immediately
    ])
    return payload, fd


def case(name, build_payload, message_id, setup_window=False, window_id=9001):
    sock = connect()
    try:
        if setup_window:
            send(sock, MSG["CreateWindow"], create_window_payload(window_id))
            time.sleep(0.15)
        result = build_payload(window_id)
        if isinstance(result, tuple):
            payload, fd = result
        else:
            payload, fd = result, None
        send(sock, message_id, payload, fd)
        if fd is not None:
            os.close(fd)
        time.sleep(0.2)
        # 读一下 socket：服务端若立刻断开，这里会拿到 EOF；否则超时视为仍连接
        try:
            reply = sock.recv(64)
            state = "服务端已关闭连接" if reply == b"" else "收到响应"
        except socket.timeout:
            state = "连接仍存活（服务端未断开）"
        print(f"[{name}] 已发送 -> {state}")
        return state
    except OSError as error:
        print(f"[{name}] 发送失败: {error}")
        return f"error: {error}"
    finally:
        sock.close()


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    cases = {
        "drag": (
            "无拖放时发 SetAcceptsDrag（修复前 VERIFY 必崩）",
            lambda wid: boolean(True),
            MSG["SetAcceptsDrag"],
            False,
        ),
        "huge_rect": (
            "CreateWindow 带 INT_MAX 矩形（修复前窗口框位图分配 abort）",
            lambda wid: create_window_payload(wid, (0, 0, 2**31 - 1, 2**31 - 1)),
            MSG["CreateWindow"],
            False,
        ),
        "huge_min": (
            "SetWindowMinimumSize 带 INT_MAX（修复前窗口框位图分配 abort）",
            lambda wid: i32(wid) + size(2**31 - 1, 2**31 - 1),
            MSG["SetWindowMinimumSize"],
            True,
        ),
        "bad_pitch": (
            "后备存储 pitch=1 < width*4（修复前位图越界 → SIGBUS）",
            lambda wid: backing_store_payload(wid, 1),
            MSG["SetWindowBackingStore"],
            True,
        ),
        "short_fd": (
            "后备存储 fd 只有 4KB，却声明 40000 字节（修复前 SIGBUS）",
            lambda wid: backing_store_payload(wid, 400, (100, 100), 4096),
            MSG["SetWindowBackingStore"],
            True,
        ),
    }
    if which not in (*cases, "all"):
        print("未知用例:", which, file=sys.stderr)
        print("可选:", ", ".join((*cases, "all")), file=sys.stderr)
        return 2

    selected = cases.items() if which == "all" else [(which, cases[which])]
    for name, (description, payload_builder, message_id, setup) in selected:
        print(f"=== {name}: {description}")
        case(name, payload_builder, message_id, setup)
        time.sleep(0.3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
