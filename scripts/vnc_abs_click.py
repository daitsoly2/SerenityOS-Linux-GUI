#!/usr/bin/env python3
"""用 VNC RFB PointerEvent 的绝对坐标点击（不翻转 Y，分步减小 delta）。

vncshot.py 的 vclick() 会额外翻转 Y 轴并归位到左下角，在本项目的 QEMU
相对鼠标 + flat accel 组合下点不准；这里按 RFB 协议直接发目标坐标。

用法: python3 vnc_abs_click.py <x> <y>
"""
import struct
import sys
import time

sys.path.insert(0, "/mnt/user/serenity/Build/vm")
import vncshot  # noqa: E402


def main():
    if len(sys.argv) != 3:
        print("usage: vnc_abs_click.py <x> <y>", file=sys.stderr)
        return 2
    x, y = int(sys.argv[1]), int(sys.argv[2])
    sock = vncshot.vnc_handshake()

    def send(mask, cx, cy):
        sock.sendall(struct.pack(">BBHH", 5, mask, int(cx), int(cy)))

    # 先归位到左上角，再分两步接近目标：QEMU 把绝对位置换成相对 delta，
    # 单次 delta 越小越不容易被 libinput 的加速/裁剪带偏。
    send(0, 0, 0)
    time.sleep(0.3)
    send(0, x // 2, y // 2)
    time.sleep(0.2)
    send(0, x, y)
    time.sleep(0.35)
    send(1, x, y)
    time.sleep(0.15)
    send(0, x, y)
    time.sleep(0.25)
    sock.close()
    print(f"clicked ({x},{y}) on {vncshot.SCREEN[0]}x{vncshot.SCREEN[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
