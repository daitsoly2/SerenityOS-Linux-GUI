#!/usr/bin/env python3
"""Regression test for serenity-engine's IME on/off switch (Ctrl+Space).

Starts a real serenity-engine on private sockets, then drives the line protocol
the same way LibGUI / the Wayland bridge / the XIM frontend do. Verifies:

  * Ctrl+Space toggles the engine without the key ever reaching librime;
  * while off, every key is passed through (plain English typing);
  * the matching key release is swallowed even when Ctrl is already up;
  * the status file the taskbar indicator reads is updated;
  * control-socket `status` / `toggle` / `set` share the same state;
  * the state survives an engine restart.

Usage:
    scripts/port/ime-toggle-test.py
    scripts/port/ime-toggle-test.py --engine Build/lagom/bin/serenity-engine
    scripts/port/ime-toggle-test.py --engine /tmp/ime-stub/serenity-engine-test

The engine needs librime at runtime; the build tree is not required.

Host caveat: librime's start_maintenance(True) SIGBUSes inside this container
(rc=135), so the production binary from Build/lagom/bin will not boot here.
Point --engine at the VM's /serenity/bin/serenity-engine, or build a test copy
of serenity-engine.cpp with the start_maintenance/join_maintenance_thread pair
skipped. The protocol logic under test is identical either way.
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ENGINE = os.path.join(ROOT, "Build", "lagom", "bin", "serenity-engine")

CTRL_SPACE_PRESS = (0x20, 0x4)  # XK_space, X11 Control bit
KEY_N = (0x6E, 0x0)


class Failure(Exception):
    pass


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.sock.connect(path)
        self.buffer = b""

    def send(self, line):
        self.sock.sendall(line.encode() + b"\n")

    def _read_line(self):
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise Failure("engine closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("utf-8", "replace")

    def lines_until_handled(self):
        lines = []
        while True:
            line = self._read_line()
            lines.append(line)
            if line.startswith("handled "):
                return lines

    def key(self, keysym, states, release=0):
        self.send(f"key {keysym} {states} {release}")
        lines = self.lines_until_handled()
        return lines, lines[-1]

    def close(self):
        self.sock.close()


class Engine:
    def __init__(self, binary, workdir):
        self.binary = binary
        self.workdir = workdir
        self.status_dir = os.path.join(workdir, "status")
        self.sock = os.path.join(workdir, "ime.sock")
        self.ctl = os.path.join(workdir, "ime-ctl.sock")
        os.makedirs(self.status_dir, exist_ok=True)
        self.proc = None
        self.log = None

    def start(self):
        env = os.environ.copy()
        env["SERENITY_IME_STATUS_DIR"] = self.status_dir
        env["RIME_USER_DATA_DIR"] = os.path.join(self.workdir, "rime-data")
        self.log = open(os.path.join(self.workdir, "engine.log"), "ab", buffering=0)
        self.proc = subprocess.Popen(
            [self.binary, "--socket", self.sock, "--ctl-socket", self.ctl],
            env=env, stdout=self.log, stderr=subprocess.STDOUT,
        )
        deadline = time.time() + 20
        while time.time() < deadline:
            if os.path.exists(self.sock) and os.path.exists(self.ctl):
                return
            if self.proc.poll() is not None:
                raise Failure(f"engine exited early (code {self.proc.returncode}); see {self.log.name}")
            time.sleep(0.05)
        raise Failure("engine did not create its sockets in time")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.log:
            self.log.close()

    def status(self):
        with open(os.path.join(self.status_dir, "serenity-ime-status"), "rb") as f:
            return f.read().decode("utf-8", "replace")

    def methods(self):
        with open(os.path.join(self.status_dir, "serenity-ime-methods"), "rb") as f:
            return f.read().decode("utf-8", "replace")

    def control(self, command):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(self.ctl)
        s.sendall(command.encode() + b"\n")
        data = b""
        while b"\n" not in data:
            chunk = s.recv(1024)
            if not chunk:
                break
            data += chunk
        s.close()
        return data.decode("utf-8", "replace").strip()


class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name, condition, detail=""):
        if condition:
            self.passed += 1
            print(f"  PASS  {name}")
        else:
            self.failed += 1
            print(f"  FAIL  {name}  {detail}")
            raise Failure(f"check failed: {name} {detail}")


def run(engine_path, workdir):
    c = Checks()
    engine = Engine(engine_path, workdir)
    engine.start()
    try:
        client = Client(engine.sock)
        client.send("create")
        c.check("create replies ok", client._read_line() == "ok")

        print("\n[1] initial state")
        c.check("status file says on", engine.status().startswith("1\trime"), repr(engine.status()))
        c.check("methods file lists both", "rime" in engine.methods() and "keyboard-us" in engine.methods())

        print("\n[2] typing while on goes to the engine")
        lines, handled = client.key(*KEY_N)
        c.check("letter consumed by engine", handled == "handled 1", handled)
        c.check("engine pushes a panel", any(x.startswith("panel ") for x in lines), repr(lines))

        print("\n[3] Ctrl+Space turns IME off")
        lines, handled = client.key(*CTRL_SPACE_PRESS)
        c.check("toggle key swallowed", handled == "handled 1", handled)
        c.check("status file says off", engine.status().startswith("0\tkeyboard-us"), repr(engine.status()))
        c.check("composition cleared via empty panel", any(x.startswith("panel ") for x in lines), repr(lines))

        print("\n[4] matching release is swallowed")
        _, handled = client.key(0x20, 0x4, release=1)
        c.check("Ctrl+Space release swallowed", handled == "handled 1", handled)

        print("\n[5] letters pass through while off (plain English)")
        lines, handled = client.key(*KEY_N)
        c.check("letter passed to client", handled == "handled 0", handled)
        c.check("no panel pushed while off", not any(x.startswith("panel ") for x in lines), repr(lines))
        c.check("still off", engine.status().startswith("0\t"))

        print("\n[6] Ctrl+Alt+Space is not treated as a toggle")
        lines, handled = client.key(0x20, 0x4 | 0x8)
        c.check("shortcut left alone", handled == "handled 0", handled)
        c.check("still off", engine.status().startswith("0\t"))

        print("\n[7] release without Ctrl is still swallowed")
        _, handled = client.key(*CTRL_SPACE_PRESS)
        c.check("Ctrl+Space turns IME back on", handled == "handled 1" and engine.status().startswith("1\t"), repr(engine.status()))
        _, handled = client.key(0x20, 0x0, release=1)
        c.check("bare space release swallowed", handled == "handled 1", handled)

        print("\n[8] typing works again")
        _, handled = client.key(*KEY_N)
        c.check("letter consumed again", handled == "handled 1", handled)

        print("\n[9] control socket shares the same state")
        reply = engine.control("status")
        c.check("status reports on", reply == "ok active 1 rime", repr(reply))
        reply = engine.control("toggle off")
        c.check("toggle off reply", reply == "ok active 0", repr(reply))
        c.check("status file off", engine.status().startswith("0\t"), repr(engine.status()))
        _, handled = client.key(*KEY_N)
        c.check("letter passes through after toggle off", handled == "handled 0", handled)
        reply = engine.control("toggle")
        c.check("bare toggle flips back on", reply == "ok active 1", repr(reply))
        c.check("set keyboard-us turns off", engine.control("set keyboard-us") == "ok" and engine.status().startswith("0\t"))
        c.check("set rime turns on", engine.control("set rime") == "ok" and engine.status().startswith("1\t"))

        print("\n[10] state survives an engine restart")
        engine.control("toggle off")
        c.check("off before restart", engine.status().startswith("0\t"))
        client.close()
        engine.stop()
        engine.start()
        client = Client(engine.sock)
        client.send("create")
        client._read_line()
        c.check("still off after restart", engine.status().startswith("0\t"), repr(engine.status()))
        _, handled = client.key(*KEY_N)
        c.check("letters still pass through", handled == "handled 0", handled)
        _, handled = client.key(*CTRL_SPACE_PRESS)
        c.check("Ctrl+Space still turns it on", handled == "handled 1" and engine.status().startswith("1\t"), repr(engine.status()))
        client.close()
    finally:
        engine.stop()

    print(f"\n{'-' * 52}\n{c.passed} passed, {c.failed} failed")
    return 1 if c.failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default=DEFAULT_ENGINE, help="path to serenity-engine binary")
    parser.add_argument("--keep", action="store_true", help="keep the temp working directory")
    args = parser.parse_args()

    if not os.path.isfile(args.engine) or not os.access(args.engine, os.X_OK):
        print(f"engine binary not found or not executable: {args.engine}", file=sys.stderr)
        print("Hint: ninja -C Build/lagom serenity-engine", file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="serenity-ime-toggle-")
    print(f"engine : {args.engine}\nworkdir: {workdir}")
    try:
        return run(os.path.abspath(args.engine), workdir)
    except Failure as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        with open(os.path.join(workdir, "engine.log"), "rb") as f:
            print("--- engine log ---", file=sys.stderr)
            print(f.read().decode("utf-8", "replace")[-4000:], file=sys.stderr)
        return 1
    finally:
        if not args.keep:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
