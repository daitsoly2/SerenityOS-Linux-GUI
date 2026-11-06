#!/usr/bin/env python3
"""Automated protocol test for the serenityime client<->engine contract.

Spawns the fake engine (no fcitx5 needed) and drives the exact line sequence
GUI::InputMethod would send, asserting the responses. This validates the wire
contract between the LibGUI client (InputMethod.cpp) and the engine addon
(serenityime.cpp), including the candidate-window mouse interaction:

  * connect (create -> ok), cap / focus / cursor / surround are fire-and-forget
  * every printable key returns a panel + "handled 1"
  * `select <index>` commits the chosen candidate and answers "handled 0"
  * PageUp / PageDown keys answer "handled 0" (no hang, even on a single page)
  * destroy -> ok

The test exercises the *command format and engine behavior*. The actual C++
client code path (InputMethod::select_candidate / request_page, the widget's
mousedown_event) and the real fcitx5 addon still require a deployed VM - see
Ports/LinuxGUI/IMEFrontend/README.md.

Usage: scripts/port/ime-protocol-test.py
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE_ENGINE = os.path.join(HERE, "ime-fake-engine.py")

# Must mirror CANDIDATES in ime-fake-engine.py.
EXPECTED_CANDIDATES = ["你好", "尼豪", "你"]


def decode_hex(token: str) -> str:
    return bytes.fromhex(token).decode("utf-8", "replace") if token else ""


class ProtocolError(Exception):
    pass


def main() -> int:
    sock_path = os.path.join(tempfile.gettempdir(), f"ime-protocol-test-{os.getpid()}.sock")
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass

    engine = subprocess.Popen(
        [sys.executable, FAKE_ENGINE, "--quiet", "--socket", sock_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(0.6)

    sock: socket.socket | None = None
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(sock_path)
        sock.settimeout(2.0)

        buf = b""

        def recv_line() -> str:
            nonlocal buf
            while b"\n" not in buf:
                chunk = sock.recv(4096)
                if not chunk:
                    raise ProtocolError("engine closed the connection")
                buf += chunk
            line, buf = buf.split(b"\n", 1)
            return line.decode("utf-8", "replace")

        def send(cmd: str) -> None:
            sock.sendall((cmd + "\n").encode())

        def expect(expected: str) -> str:
            line = recv_line()
            if line != expected:
                raise ProtocolError(f"expected {expected!r}, got {line!r}")
            return line

        def drain_until_handled() -> list[str]:
            seen: list[str] = []
            while True:
                line = recv_line()
                seen.append(line)
                if line.startswith("handled"):
                    return seen

        checks: list[str] = []

        # 1. Handshake.
        send("create")
        expect("ok")
        checks.append("create -> ok")

        # 2. Fire-and-forget housekeeping the client sends; no reply expected.
        send("cap 0")
        send("focus 1")
        send("cursor 100 200 2 20")
        send("surround 0 0 " + "00")
        checks.append("cap/focus/cursor/surround accepted (no reply)")

        # 3. Type "ni": each printable key must return a panel + handled 1.
        for ch in "ni":
            send(f"key {ord(ch)} 0 0")
            seen = drain_until_handled()
            if not any(line.startswith("panel ") for line in seen):
                raise ProtocolError(f"no panel after key {ch!r}: {seen}")
            if seen[-1] != "handled 1":
                raise ProtocolError(f"key {ch!r} not handled: {seen}")
        checks.append("typing 'ni' returns panel + handled 1 each key")

        # 4. Mouse click on candidate 0 -> commit first candidate + handled 0.
        send("select 0")
        seen = drain_until_handled()
        commits = [line for line in seen if line.startswith("commit ")]
        if not commits:
            raise ProtocolError(f"select 0 produced no commit: {seen}")
        committed = decode_hex(commits[0].split()[1])
        if committed != EXPECTED_CANDIDATES[0]:
            raise ProtocolError(
                f"select 0 committed {committed!r}, want {EXPECTED_CANDIDATES[0]!r}")
        if seen[-1] != "handled 0":
            raise ProtocolError(f"select 0 not answered with handled 0: {seen}")
        checks.append(f"select 0 commits {committed!r} + handled 0")

        # 5. Build a fresh preedit, then page up / down (single page here, but the
        #    round-trip must complete without hanging).
        send("key 110 0 0")  # 'n'
        drain_until_handled()
        for page_key in (0xFF55, 0xFF56):  # PageUp, PageDown
            send(f"key {page_key} 0 0")
            seen = drain_until_handled()
            if seen[-1] != "handled 0":
                raise ProtocolError(f"page key {hex(page_key)} not answered: {seen}")
        checks.append("PageUp/PageDown answered handled 0 (no hang)")

        # 6. Tear down.
        send("destroy")
        expect("ok")
        checks.append("destroy -> ok")

        print("PASS - serenityime protocol contract")
        for c in checks:
            print(f"  [x] {c}")
        return 0

    except ProtocolError as err:
        print(f"FAIL - {err}", file=sys.stderr)
        return 1
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        if engine.poll() is None:
            engine.terminate()
            try:
                engine.wait(timeout=2)
            except Exception:
                engine.kill()
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
