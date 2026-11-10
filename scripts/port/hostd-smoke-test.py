#!/usr/bin/env python3
"""serenity-hostd 冒烟测试：协议、输入校验、真实增删用户。

在 VM 里以桌面用户执行；会创建一个一次性账户 testu1 然后删除。
用法: python3 hostd-smoke-test.py
"""

import socket

SOCKET = "/run/serenity-hostd/socket"


def call(request):
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(10)
    connection.connect(SOCKET)
    connection.sendall((request + "\n").encode())
    buffer = bytearray()
    while True:
        chunk = connection.recv(4096)
        if not chunk:
            break
        buffer += chunk
    connection.close()
    return buffer.decode("utf-8", "replace").strip()


def hex_of(text):
    return text.encode("utf-8").hex()


def show(label, response):
    print(f"{label:<22} -> {response.replace(chr(10), ' | ')}")


def main():
    show("ping", call("ping"))
    show("users.list", call("users.list"))

    # 回归用例：空密码时请求行末尾是一个空字段（"…\t0\t"）。旧 Python 守护用
    # str.strip() 裁行，tab 也算空白，于是 5 个字段被吃成 4 个，回 "参数个数不对"。
    # 正确行为是回 "密码不能为空"。
    show("add(空密码)", call("users.add\ttestu0\t{}\t0\t".format(hex_of("测试"))))

    add = "users.add\ttestu1\t{}\t0\t{}".format(hex_of("测试用户"), hex_of("Test@12345"))
    show("add(testu1)", call(add))
    show("add(重复)", call("users.add\ttestu1\t{}\t0\t{}".format(hex_of("x"), hex_of("x"))))
    show("add(非法用户名)", call("users.add\tBAD NAME\t{}\t0\t{}".format(hex_of("x"), hex_of("x"))))
    show("delete(root)", call("users.delete\troot"))
    show("delete(会话用户)", call("users.delete\tserenity"))
    show("未知能力", call("bogus.cmd"))
    show("list(含新用户)", call("users.list"))
    show("passwd(testu1)", call("users.passwd\ttestu1\t{}".format(hex_of("Changed@123"))))
    show("modify(testu1)", call("users.modify\ttestu1\t{}\t{}\t1".format(hex_of("改名后"), hex_of("/bin/bash"))))
    show("delete(testu1)", call("users.delete\ttestu1"))
    show("list(清理后)", call("users.list"))


if __name__ == "__main__":
    main()
