#!/usr/bin/env python3
"""Probe the serenityime fcitx5 addon socket protocol.

Usage: ime_probe.py [keysyms...]  (default: n i h a o)
"""
import socket
import sys
import time

SOCKET_PATH = "/tmp/serenity-ime.sock"


def read_lines(sock, timeout=1.5):
    sock.settimeout(timeout)
    out = []
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            chunk = sock.recv(65536)
            if not chunk:
                break
            out.append(chunk.decode("utf-8", "replace"))
    except socket.timeout:
        pass
    return "".join(out)


def decode_panel(line):
    """panel <cursor> <aux> <preedit> <cand_cursor> <n> [label text]*

    An empty aux is sent as two consecutive spaces, so split on a single
    space (not on arbitrary whitespace) to keep the empty field.
    """
    parts = line.split(" ")
    cursor = parts[1]
    aux = bytes.fromhex(parts[2]).decode("utf-8", "replace") if parts[2] != "" else ""
    preedit = bytes.fromhex(parts[3]).decode("utf-8", "replace")
    cand_cursor = parts[4]
    count = int(parts[5])
    candidates = []
    index = 6
    for _ in range(count):
        label = bytes.fromhex(parts[index]).decode("utf-8", "replace")
        text = bytes.fromhex(parts[index + 1]).decode("utf-8", "replace")
        candidates.append((label, text))
        index += 2
    return f"preedit={preedit!r} cursor={cursor} aux={aux!r} sel={cand_cursor}", candidates


def main():
    keys = sys.argv[1:] or ["n", "i", "h", "a", "o"]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCKET_PATH)

    sock.sendall(b"create\n")
    print("create ->", read_lines(sock).strip())

    sock.sendall(b"cap 0\n")
    sock.sendall(b"focus 1\n")
    sock.sendall(b"cursor 100 200 2 20\n")
    time.sleep(0.2)

    for key in keys:
        sym = ord(key)
        sock.sendall(f"key {sym} 0 0\n".encode())
        response = read_lines(sock, 1.0).strip()
        print(f"key {key!r} -> {response}")
        for line in response.splitlines():
            if line.startswith("panel "):
                summary, candidates = decode_panel(line)
                print("   ", summary)
                if candidates:
                    print("    candidates:", " ".join(t for _, t in candidates[:8]))
            elif line.startswith("commit "):
                print("    COMMIT:", bytes.fromhex(line.split()[1]).decode("utf-8", "replace"))

    sock.sendall(b"destroy\n")
    sock.close()


if __name__ == "__main__":
    main()
