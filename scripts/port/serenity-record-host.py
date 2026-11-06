#!/usr/bin/env python3
"""
Host-side encoder for the Serenity desktop screen recorder.

The guest image ships no ffmpeg, so the compositor streams raw frames to this
daemon over TCP (QEMU user networking exposes the host at 10.0.2.2) and ffmpeg
does the encoding here. When a recording ends the finished file is copied into
the guest so it shows up in the desktop's ~/Videos like any other capture.

Protocol
--------
The guest connects and sends one header line:

    SERENITYREC 1 <width> <height> <fps> <filename>\n

followed by exactly width*height*4 bytes per frame (bgr0, top-down, no padding).
Closing the connection ends the recording.

Usage
-----
    ./serenity-record-host.py                 # listen on 127.0.0.1:9099
    ./serenity-record-host.py --port 9099 --verbose

Environment
-----------
    SERENITY_REC_PORT        listen port            (default 9099)
    SERENITY_REC_BIND        listen address         (default 127.0.0.1)
    SERENITY_REC_DIR         host staging directory (default ~/Videos)
    SERENITY_REC_SSH_KEY     guest ssh key          (default <repo>/Build/vm/id_ed25519)
    SERENITY_REC_SSH_PORT    guest ssh port         (default 2222)
    SERENITY_REC_SSH_TARGET  guest ssh target       (default serenity@127.0.0.1)
    SERENITY_REC_VM_DIR      guest output directory (default /home/serenity/Videos)
    SERENITY_REC_CRF         x264 quality           (default 20)
    SERENITY_REC_PRESET      x264 preset            (default veryfast)
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def log(message):
    sys.stderr.write(f"[record-host] {message}\n")
    sys.stderr.flush()


class Recording:
    def __init__(self, conn, header, verbose=False):
        fields = header.split()
        if len(fields) < 6 or fields[0] != "SERENITYREC":
            raise ValueError(f"bad header: {header!r}")
        self.width = int(fields[2])
        self.height = int(fields[3])
        self.fps = max(1, int(fields[4]))
        self.filename = os.path.basename(fields[5]) or f"serenity-rec-{int(time.time())}.mp4"
        self.conn = conn
        self.verbose = verbose
        self.frame_size = self.width * self.height * 4

        self.host_dir = os.path.expanduser(os.environ.get("SERENITY_REC_DIR", "~/Videos"))
        os.makedirs(self.host_dir, exist_ok=True)
        self.host_path = os.path.join(self.host_dir, self.filename)

    def ffmpeg_command(self):
        return [
            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
            "-f", "rawvideo",
            "-pix_fmt", "bgr0",
            "-s", f"{self.width}x{self.height}",
            "-framerate", str(self.fps),
            # Arrival time is the only honest clock here: if the guest could
            # not push every frame, the output still lasts as long as the
            # recording did instead of playing back sped up.
            "-use_wallclock_as_timestamps", "1",
            "-i", "pipe:0",
            "-c:v", "libx264",
            "-preset", os.environ.get("SERENITY_REC_PRESET", "veryfast"),
            "-crf", os.environ.get("SERENITY_REC_CRF", "20"),
            "-pix_fmt", "yuv420p",
            "-fps_mode", "cfr",
            "-r", str(self.fps),
            "-movflags", "+faststart",
            self.host_path,
        ]

    def run(self):
        log(f"recording {self.width}x{self.height}@{self.fps} -> {self.host_path}")
        encoder = subprocess.Popen(self.ffmpeg_command(), stdin=subprocess.PIPE)

        frames = 0
        started = time.monotonic()
        buffer = bytearray(self.frame_size)
        view = memoryview(buffer)
        try:
            while True:
                frame = self._read_frame(view)
                if frame is None:
                    break
                try:
                    encoder.stdin.write(frame)
                except BrokenPipeError:
                    log("ffmpeg closed the pipe early")
                    break
                frames += 1
                if self.verbose and frames % 30 == 0:
                    log(f"{frames} frames")
        except Exception as error:  # noqa: BLE001 - the guest may vanish mid-frame
            log(f"stream error: {error}")
        finally:
            try:
                encoder.stdin.close()
            except Exception:  # noqa: BLE001
                pass
            encoder.wait()
            elapsed = max(time.monotonic() - started, 0.001)
            log(f"encoder finished: {frames} frames in {elapsed:.1f}s")

        if frames == 0 or not os.path.exists(self.host_path) or os.path.getsize(self.host_path) == 0:
            log("nothing recorded")
            if os.path.exists(self.host_path):
                os.unlink(self.host_path)
            return

        self._copy_to_guest()

    def _read_frame(self, view):
        # recv_into straight into the frame buffer: no per-frame copy, which
        # matters at 7 MB a frame.
        filled = 0
        total = len(view)
        while filled < total:
            received = self.conn.recv_into(view[filled:])
            if received == 0:
                return None
            filled += received
        return view

    def _copy_to_guest(self):
        key = os.environ.get("SERENITY_REC_SSH_KEY", os.path.join(REPO_ROOT, "Build", "vm", "id_ed25519"))
        port = os.environ.get("SERENITY_REC_SSH_PORT", "2222")
        target = os.environ.get("SERENITY_REC_SSH_TARGET", "serenity@127.0.0.1")
        vm_dir = os.environ.get("SERENITY_REC_VM_DIR", "/home/serenity/Videos")

        if not os.path.exists(key) or shutil.which("scp") is None:
            log(f"no ssh key/scp; the recording stays on the host at {self.host_path}")
            return

        ssh_opts = ["-F", "/dev/null", "-i", key,
                    "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                    "-o", "ConnectTimeout=10", "-o", "LogLevel=ERROR"]

        try:
            subprocess.run(["ssh", *ssh_opts, "-p", port, target, f"mkdir -p {vm_dir}"], check=True)
            subprocess.run(["scp", *ssh_opts, "-P", port, self.host_path, f"{target}:{vm_dir}/"], check=True)
            log(f"copied to the guest: {vm_dir}/{self.filename}")
        except subprocess.CalledProcessError as error:
            log(f"copy to the guest failed ({error}); the file is at {self.host_path}")


def handle_connection(conn, verbose):
    with conn:
        conn.settimeout(None)
        buffer = b""
        while b"\n" not in buffer:
            chunk = conn.recv(256)
            if not chunk:
                return
            buffer += chunk
        header = buffer.split(b"\n", 1)[0].decode("utf-8", "replace").strip()
        try:
            Recording(conn, header, verbose).run()
        except ValueError as error:
            log(str(error))


def main():
    parser = argparse.ArgumentParser(description="Host-side encoder for the Serenity desktop recorder")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SERENITY_REC_PORT", "9099")))
    parser.add_argument("--bind", default=os.environ.get("SERENITY_REC_BIND", "127.0.0.1"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.bind, args.port))
    server.listen(4)
    log(f"listening on {args.bind}:{args.port} (frames arrive from the guest via 10.0.2.2)")

    try:
        while True:
            conn, address = server.accept()
            log(f"connection from {address[0]}:{address[1]}")
            threading.Thread(target=handle_connection, args=(conn, args.verbose), daemon=True).start()
    except KeyboardInterrupt:
        log("stopping")
    finally:
        server.close()


if __name__ == "__main__":
    main()
