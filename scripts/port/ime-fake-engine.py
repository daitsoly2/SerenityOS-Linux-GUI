#!/usr/bin/env python3
"""A stand-in for the fcitx5 serenityime addon.

It speaks the same line protocol on the same unix socket, so a LibGUI app can
be exercised end to end without installing fcitx5:

    scripts/port/ime-fake-engine.py &        # listens on /tmp/serenity-ime.sock
    <start an app with a text box, focus it, type "nihao", press Enter>

What it does:
  * printable keys build a preedit and show three fake candidates;
  * Backspace edits the preedit;
  * Enter commits the first candidate ("你好"), Escape cancels;
  * any other key is reported as not handled, so the app types it normally.

This is a development aid, not an input method: it does no pinyin lookup.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys

DEFAULT_SOCKET = "/tmp/serenity-ime.sock"
CANDIDATES = ["你好", "尼豪", "你"]


def hex_encode(text: str) -> str:
    return text.encode("utf-8").hex()


def panel_line(preedit: str, cursor: int, selected: int, candidates: list[str]) -> str:
    line = f"panel {cursor} {hex_encode('')} {hex_encode(preedit)} {selected} {len(candidates)}"
    for index, candidate in enumerate(candidates, start=1):
        line += f" {hex_encode(str(index) + '.')} {hex_encode(candidate)}"
    return line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", default=os.environ.get("SERENITY_IME_SOCKET", DEFAULT_SOCKET))
    parser.add_argument("--quiet", action="store_true", help="do not log every command")
    args = parser.parse_args()

    try:
        os.unlink(args.socket)
    except FileNotFoundError:
        pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(args.socket)
    os.chmod(args.socket, 0o600)
    server.listen(1)
    print(f"fake engine listening on {args.socket}", flush=True)

    preedit = ""

    while True:
        connection, _ = server.accept()
        print("client connected", flush=True)
        buffer = b""

        def send(line: str) -> None:
            connection.sendall((line + "\n").encode())

        try:
            while True:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace")
                    if not args.quiet:
                        print(f"<- {line}", flush=True)

                    if line == "create":
                        send("ok")
                    elif (line.startswith("cap") or line.startswith("focus")
                          or line.startswith("cursor") or line.startswith("surround")
                          or line == "reset"):
                        # These are fire-and-forget from the client; the real
                        # addon acts on them but never replies, so the client
                        # does not wait. Stay silent to match.
                        pass
                    elif line == "destroy":
                        send("ok")
                    elif line.startswith("select "):
                        # Mouse click on a candidate: commit that 0-based entry.
                        try:
                            idx = int(line.split()[1])
                        except (IndexError, ValueError):
                            idx = 0
                        if 0 <= idx < len(CANDIDATES) and preedit:
                            send(f"commit {hex_encode(CANDIDATES[idx])}")
                            preedit = ""
                            send(panel_line("", 0, -1, []))
                        send("handled 0")
                    elif line.startswith("key "):
                        _, symbol, _states, _release = line.split()
                        symbol = int(symbol)
                        if 0x20 <= symbol < 0x7F:
                            preedit += chr(symbol)
                            send(panel_line(preedit, len(preedit), 0, CANDIDATES))
                            send("handled 1")
                        elif symbol == 0xFF0D and preedit:  # Return
                            send(f"commit {hex_encode(CANDIDATES[0])}")
                            preedit = ""
                            send(panel_line("", 0, -1, []))
                            send("handled 1")
                        elif symbol == 0xFF1B:  # Escape
                            preedit = ""
                            send(panel_line("", 0, -1, []))
                            send("handled 1")
                        elif symbol == 0xFF08 and preedit:  # Backspace
                            preedit = preedit[:-1]
                            send(panel_line(preedit, len(preedit), 0, CANDIDATES if preedit else []))
                            send("handled 1")
                        else:
                            send("handled 0")
                    else:
                        send("err unknown-command")
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            connection.close()
            preedit = ""
            print("client gone", flush=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
