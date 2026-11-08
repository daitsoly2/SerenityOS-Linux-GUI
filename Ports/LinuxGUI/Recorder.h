/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>

namespace Gfx {
class Bitmap;
}

namespace Core {
class LocalServer;
class LocalSocket;
class Notifier;
class Timer;
}

namespace WindowServer {

class RecordingPillOverlay;

// In-compositor screen recorder for the Serenity desktop.
//
// This is the port of gpurec's core idea: gpu-screen-recorder lives inside the
// compositor and grabs the composed frame, so it never pays for an extra
// round-trip through the display server. Here the frames come straight out of
// Compositor's front bitmap, which is exactly the buffer the screen was just
// presented from.
//
// Encoding happens on the host: the guest image has no ffmpeg, and the user
// asked for host-side encoding, so the raw frames are streamed over TCP to
// scripts/port/serenity-record-host.py (reachable at the QEMU user-net gateway
// 10.0.2.2). That daemon pipes them into ffmpeg and copies the finished file
// back into the guest's ~/Videos.
//
// Screenshots need no host help: they are encoded as PNG in-process.
//
// Everything is driven from the WindowServer event loop. Frame writes are
// non-blocking and driven by a Core::Notifier, so a slow or stalled host can
// only cost us dropped frames, never a frozen desktop.
class Recorder {
public:
    enum class State {
        Idle,
        Recording,
        Paused,
    };

    static Recorder& the();

    // Called once the compositor and the window manager exist.
    static void initialize();

    State state() const { return m_state; }
    bool is_recording() const { return m_state == State::Recording; }
    unsigned elapsed_seconds() const;

    void start_recording();
    void stop_recording();
    void toggle_recording();
    void toggle_paused();

    void take_screenshot();

    void set_fps(int fps);
    void set_output_directory(ByteString path);

    int fps() const { return m_fps; }
    ByteString const& output_directory() const { return m_output_directory; }

    void show_toast(ByteString const& text);
    void notify(ByteString const& app_name, ByteString const& summary, ByteString const& body, int timeout_ms = 3500);

private:
    Recorder();
    ~Recorder();

    // ── control channel (/tmp/serenity-recorder.sock) ───────────────────────
    void setup_control_server();
    void handle_control_command(int client_fd, ByteString const& line);
    void send_to_client(int client_fd, ByteString const& line);
    void drop_client(int client_fd);
    ByteString status_line() const;

    // ── capture ─────────────────────────────────────────────────────────────
    bool open_capture_stream(int width, int height);
    bool open_local_encoder(int width, int height);
    void close_capture_stream();
    void on_frame_tick();
    void pump_pending_frame();

    // ── overlays ────────────────────────────────────────────────────────────
    void update_pill();
    void hide_pill();
    void start_saved_file_poll(ByteString const& path);

    struct ControlClient {
        NonnullOwnPtr<Core::LocalSocket> socket;
        ByteString pending;
    };
    State m_state { State::Idle };
    int m_fps { 30 };
    ByteString m_output_directory;
    ByteString m_current_filename;
    ByteString m_last_output_path;

    // Monotonic milliseconds. Paused time is not counted.
    u64 m_started_at_ms { 0 };
    u64 m_accumulated_ms { 0 };

    int m_capture_fd { -1 };
    // Local transport: ffmpeg child fed through a pipe (the gsr model).
    int m_encoder_pid { -1 };
    bool m_local_encoder { false };
    Vector<u8> m_frame_buffer;
    RefPtr<Gfx::Bitmap> m_scaled_frame;
    size_t m_frame_size { 0 };
    size_t m_frame_offset { 0 };
    bool m_frame_in_flight { false };
    RefPtr<Core::Notifier> m_write_notifier;
    bool m_capture_failed { false };
    bool m_stream_open { false };

    RefPtr<Core::Timer> m_frame_timer;
    RefPtr<Core::Timer> m_clock_timer;

    RefPtr<Core::LocalServer> m_control_server;
    // Owned by pointer so the address handed to each socket's read callback
    // stays valid when the vector grows.
    Vector<NonnullOwnPtr<ControlClient>> m_clients;

    OwnPtr<RecordingPillOverlay> m_pill;

    RefPtr<Core::Timer> m_saved_poll_timer;
    ByteString m_saved_poll_path;
    unsigned m_saved_poll_attempts { 0 };
};

}
