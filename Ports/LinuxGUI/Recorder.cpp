/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Recorder.h"
#include "Notifications.h"

#include <AK/StringBuilder.h>
#include <LibCore/DateTime.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <WindowServer/Compositor.h>
#include <WindowServer/Overlays.h>
#include <WindowServer/Screen.h>
#include <WindowServer/WindowManager.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace WindowServer {

static constexpr char const* control_socket_path = "/tmp/serenity-recorder.sock";
static constexpr u16 default_host_port = 9099;
// Long-edge cap for the stream; see on_frame_tick().
static int s_max_capture_width = 1152;

static u64 monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}

// The port's LibFileSystem has no recursive mkdir; the recorder only needs it
// for two fixed directories, so a plain loop is enough.
static void ensure_directory_exists(ByteString const& path)
{
    if (path.is_empty())
        return;
    StringBuilder builder;
    for (auto part : path.split_view('/')) {
        if (part.is_empty()) {
            builder.append('/');
            continue;
        }
        builder.append(part);
        auto current = builder.to_byte_string();
        if (mkdir(current.characters(), 0755) < 0 && errno != EEXIST)
            warnln("Recorder: cannot create {}: {}", current, strerror(errno));
        builder.append('/');
    }
}

static Gfx::Color pill_background() { return Gfx::Color::from_rgb(0x14161a); }
static Gfx::Color pill_text() { return Gfx::Color::from_rgb(0xf2f4f8); }
static Gfx::Color rec_dot() { return Gfx::Color::from_rgb(0xff3b30); }
static Gfx::Color paused_dot() { return Gfx::Color::from_rgb(0xffb020); }

// ─────────────────────────────────────────────────────────────────────────────
// Overlays
// ─────────────────────────────────────────────────────────────────────────────

// The "REC 00:12" pill gpurec draws in the corner, done as a compositor
// overlay instead of a foreign window: it is always on top, it costs one
// invalidated rect per second, and it cannot be clicked by accident.
class RecordingPillOverlay final : public Overlay {
public:
    RecordingPillOverlay()
    {
        m_font = &Gfx::FontDatabase::default_font();
    }

    virtual ZOrder zorder() const override { return ZOrder::ScreenNumber; }

    void set_state(bool paused, unsigned elapsed_seconds)
    {
        if (m_paused == paused && m_elapsed == elapsed_seconds)
            return;
        m_paused = paused;
        m_elapsed = elapsed_seconds;
        invalidate();
    }

    void place(Screen& screen)
    {
        constexpr int width = 132;
        constexpr int height = 34;
        constexpr int margin = 18;
        // Top centre: the top-right corner belongs to the notification stack.
        auto rect = screen.rect();
        set_rect(Gfx::IntRect { rect.center().x() - width / 2, rect.top() + margin, width, height });
    }

    virtual void render(Gfx::Painter& painter, Screen const&) override
    {
        auto bounds = Gfx::IntRect { {}, rect().size() };
        painter.fill_rect_with_rounded_corners(bounds, pill_background().with_alpha(0xd0), 17);

        auto dot_rect = Gfx::IntRect { 13, bounds.height() / 2 - 5, 10, 10 };
        painter.fill_rect_with_rounded_corners(dot_rect, m_paused ? paused_dot() : rec_dot(), 5);

        auto seconds = m_elapsed % 60;
        auto minutes = (m_elapsed / 60) % 60;
        auto hours = m_elapsed / 3600;
        ByteString label;
        if (hours > 0)
            label = ByteString::formatted("REC {:02}:{:02}:{:02}", hours, minutes, seconds);
        else
            label = ByteString::formatted("REC {:02}:{:02}", minutes, seconds);
        if (m_paused)
            label = ByteString::formatted("PAUSE {:02}:{:02}", minutes, seconds);

        painter.draw_text(Gfx::IntRect { 30, 0, bounds.width() - 38, bounds.height() }, label, *m_font, Gfx::TextAlignment::CenterLeft, pill_text());
    }

private:
    Gfx::Font const* m_font { nullptr };
    bool m_paused { false };
    unsigned m_elapsed { 0 };
};

// ─────────────────────────────────────────────────────────────────────────────
// Recorder
// ─────────────────────────────────────────────────────────────────────────────

Recorder& Recorder::the()
{
    static Recorder* s_recorder = nullptr;
    if (!s_recorder)
        s_recorder = new Recorder();
    return *s_recorder;
}

void Recorder::initialize()
{
    // Frames go into an ffmpeg child's stdin on the local path. If ffmpeg dies,
    // write() raises SIGPIPE and the default action would kill the compositor;
    // ignore it so write() just returns EPIPE (which becomes a dropped frame).
    signal(SIGPIPE, SIG_IGN);

    auto& recorder = the();
    recorder.m_output_directory = ByteString::formatted("{}/Videos", Core::StandardPaths::home_directory());
    if (char const* dir = getenv("SERENITY_RECORDER_DIR"); dir && *dir)
        recorder.m_output_directory = ByteString { dir };
    if (char const* capture_width = getenv("SERENITY_RECORDER_WIDTH"); capture_width && *capture_width) {
        auto parsed = ByteString { capture_width }.to_number<int>();
        if (parsed.has_value() && *parsed >= 320 && *parsed <= 4096)
            s_max_capture_width = *parsed;
    }
    if (char const* fps = getenv("SERENITY_RECORDER_FPS"); fps && *fps) {
        auto parsed = ByteString { fps }.to_number<int>();
        if (parsed.has_value() && *parsed >= 1 && *parsed <= 120)
            recorder.m_fps = *parsed;
    }
    recorder.setup_control_server();
}

Recorder::Recorder() = default;
Recorder::~Recorder() = default;

unsigned Recorder::elapsed_seconds() const
{
    if (m_state == State::Idle)
        return (unsigned)(m_accumulated_ms / 1000);
    return (unsigned)((m_accumulated_ms + (monotonic_ms() - m_started_at_ms)) / 1000);
}

// ── control channel ─────────────────────────────────────────────────────────

void Recorder::setup_control_server()
{
    unlink(control_socket_path);
    auto server = Core::LocalServer::construct();
    if (!server->listen(control_socket_path)) {
        warnln("Recorder: cannot listen on {} ({})", control_socket_path, strerror(errno));
        return;
    }

    server->on_accept = [this](NonnullOwnPtr<Core::LocalSocket> socket) {
        // Non-blocking is mandatory: this handler runs on the WindowServer
        // event loop, and a blocking read on an idle client would freeze the
        // whole desktop (every app's sync IPC would time out).
        if (auto result = socket->set_blocking(false); result.is_error()) {
            warnln("Recorder: cannot make control client non-blocking: {}", result.error());
            return;
        }
        auto client = adopt_own(*new ControlClient { move(socket), {} });
        auto* raw = client.ptr();
        raw->socket->on_ready_to_read = [this, raw] {
            auto buffer = ByteBuffer::create_uninitialized(1024).release_value_but_fixme_should_propagate_errors();
            for (;;) {
                auto result = raw->socket->read_some(buffer.bytes());
                if (result.is_error()) {
                    // EAGAIN just means "nothing more right now".
                    break;
                }
                auto bytes = result.release_value();
                if (bytes.is_empty()) {
                    // The peer closed. Removing the client here would destroy
                    // the very Function we are running inside, which AK
                    // rejects with "may_defer || !called_from_inside_function"
                    // and takes the whole compositor down. Defer the removal
                    // to the next event-loop turn.
                    auto fd = raw->socket->fd().value_or(-1);
                    Core::deferred_invoke([this, fd] { drop_client(fd); });
                    return;
                }
                raw->pending = ByteString::formatted("{}{}", raw->pending, StringView { reinterpret_cast<char const*>(bytes.data()), bytes.size() });
                while (true) {
                    auto index = raw->pending.find('\n');
                    if (!index.has_value())
                        break;
                    auto line = raw->pending.substring(0, *index).trim_whitespace();
                    raw->pending = raw->pending.substring(*index + 1);
                    handle_control_command(raw->socket->fd().value_or(-1), line);
                }
            }
        };
        m_clients.append(move(client));
    };

    m_control_server = move(server);
    outln("Recorder: control socket at {}", control_socket_path);
}

void Recorder::send_to_client(int client_fd, ByteString const& line)
{
    auto payload = ByteString::formatted("{}\n", line);
    for (auto& client : m_clients) {
        if (client->socket->fd().value_or(-1) != client_fd)
            continue;
        (void)client->socket->write_some(payload.bytes());
        return;
    }
}

void Recorder::drop_client(int client_fd)
{
    for (size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i]->socket->fd().value_or(-1) != client_fd)
            continue;
        m_clients.remove(i);
        return;
    }
}

ByteString Recorder::status_line() const
{
    auto state = "idle"sv;
    if (m_state == State::Recording)
        state = "recording"sv;
    else if (m_state == State::Paused)
        state = "paused"sv;
    return ByteString::formatted("status {} {} {} {}", state, elapsed_seconds(), m_fps, m_last_output_path);
}

void Recorder::handle_control_command(int client_fd, ByteString const& line)
{
    auto parts = line.split(' ');
    if (parts.is_empty())
        return;
    auto command = parts[0];

    if (command == "ping"sv) {
        send_to_client(client_fd, "ok pong"sv);
        return;
    }
    if (command == "status"sv) {
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "start"sv) {
        start_recording();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "stop"sv) {
        stop_recording();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "toggle"sv) {
        toggle_recording();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "pause"sv) {
        if (m_state == State::Recording)
            toggle_paused();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "resume"sv) {
        if (m_state == State::Paused)
            toggle_paused();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "screenshot"sv) {
        take_screenshot();
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "set"sv && parts.size() >= 2) {
        auto key = parts[1];
        auto value = line.substring(key.length() + 5).trim_whitespace();
        if (key == "fps"sv) {
            if (auto fps = value.to_number<int>(); fps.has_value())
                set_fps(*fps);
        } else if (key == "dir"sv && !value.is_empty()) {
            set_output_directory(value);
        }
        send_to_client(client_fd, status_line());
        return;
    }
    if (command == "toast"sv) {
        show_toast(line.substring(6).trim_whitespace());
        send_to_client(client_fd, "ok toast"sv);
        return;
    }

    send_to_client(client_fd, ByteString::formatted("err unknown command '{}'", command));
}

// ── settings ────────────────────────────────────────────────────────────────

void Recorder::set_fps(int fps)
{
    if (fps < 1)
        fps = 1;
    if (fps > 120)
        fps = 120;
    if (m_fps == fps)
        return;
    m_fps = fps;
    // A running capture keeps its negotiated rate; the new one applies next time.
}

void Recorder::set_output_directory(ByteString path)
{
    m_output_directory = move(path);
}

// ── capture stream ──────────────────────────────────────────────────────────

static ByteString pick_local_encoder(ByteString& vaapi_device)
{
    if (char const* value = getenv("SERENITY_RECORDER_VAAPI_DEVICE"); value && *value)
        vaapi_device = ByteString { value };
    else
        vaapi_device = ByteString { "/dev/dri/renderD128" };

    if (char const* value = getenv("SERENITY_RECORDER_ENCODER"); value && *value)
        return ByteString { value };

    // This machine's only usable H.264 encoder is Intel Quick Sync (Haswell,
    // i965 VAAPI driver), so VAAPI comes first. NVENC is kept for hosts that
    // have a discrete GPU that can actually encode, x264 is the last resort.
    if (access(vaapi_device.characters(), R_OK | W_OK) == 0 && access("/usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so", R_OK) == 0)
        return ByteString { "h264_vaapi" };

    if (FILE* pipe = popen("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_nvenc && echo nvenc", "r")) {
        char buffer[32] {};
        auto* line = fgets(buffer, sizeof(buffer), pipe);
        pclose(pipe);
        if (line && !strncmp(buffer, "nvenc", 5))
            return ByteString { "h264_nvenc" };
    }
    return ByteString { "libx264" };
}

// Local transport (the gsr model): ffmpeg runs as a child of WindowServer and
// encodes the raw frames we write into its stdin. No network hop, no host-side
// daemon, and the output file lands directly in the session's ~/Videos.
bool Recorder::open_local_encoder(int width, int height)
{
    ByteString vaapi_device;
    auto encoder = pick_local_encoder(vaapi_device);

    Vector<ByteString> arguments;
    arguments.append(ByteString { "ffmpeg" });
    arguments.append(ByteString { "-hide_banner" });
    arguments.append(ByteString { "-loglevel" });
    arguments.append(ByteString { "error" });
    arguments.append(ByteString { "-y" });
    if (encoder == "h264_vaapi"sv) {
        // Global option: must appear before the input.
        arguments.append(ByteString { "-vaapi_device" });
        arguments.append(vaapi_device);
    }
    arguments.append(ByteString { "-f" });
    arguments.append(ByteString { "rawvideo" });
    arguments.append(ByteString { "-pix_fmt" });
    arguments.append(ByteString { "bgr0" });
    arguments.append(ByteString { "-s" });
    arguments.append(ByteString::formatted("{}x{}", width, height));
    arguments.append(ByteString { "-framerate" });
    arguments.append(ByteString::formatted("{}", m_fps));
    arguments.append(ByteString { "-use_wallclock_as_timestamps" });
    arguments.append(ByteString { "1" });
    arguments.append(ByteString { "-i" });
    arguments.append(ByteString { "pipe:0" });
    arguments.append(ByteString { "-c:v" });
    arguments.append(encoder);
    if (encoder == "h264_vaapi"sv) {
        // Upload to the GPU as NV12; VAAPI frames go straight into the muxer,
        // so no software -pix_fmt yuv420p afterwards.
        arguments.append(ByteString { "-vf" });
        arguments.append(ByteString { "format=nv12,hwupload" });
        arguments.append(ByteString { "-qp" });
        arguments.append(ByteString { "23" });
    } else if (encoder == "h264_nvenc"sv) {
        arguments.append(ByteString { "-preset" });
        arguments.append(ByteString { "medium" });
        arguments.append(ByteString { "-cq" });
        arguments.append(ByteString { "23" });
        arguments.append(ByteString { "-pix_fmt" });
        arguments.append(ByteString { "yuv420p" });
    } else {
        arguments.append(ByteString { "-preset" });
        arguments.append(ByteString { "veryfast" });
        arguments.append(ByteString { "-crf" });
        arguments.append(ByteString { "20" });
        arguments.append(ByteString { "-pix_fmt" });
        arguments.append(ByteString { "yuv420p" });
    }
    arguments.append(ByteString { "-movflags" });
    arguments.append(ByteString { "+faststart" });
    arguments.append(m_last_output_path);

    Vector<char const*> argv;
    for (auto& argument : arguments)
        argv.append(argument.characters());
    argv.append(nullptr);

    int pipe_fds[2] {};
    if (pipe(pipe_fds) < 0) {
        warnln("Recorder: pipe failed: {}", strerror(errno));
        return false;
    }
    (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

    auto pid = fork();
    if (pid < 0) {
        warnln("Recorder: fork failed: {}", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        // Haswell is Gen7.5: the iHD driver does not support it, i965 does.
        if (encoder == "h264_vaapi"sv)
            setenv("LIBVA_DRIVER_NAME", "i965", 1);
        execvp("ffmpeg", const_cast<char**>(argv.data()));
        _exit(127);
    }

    close(pipe_fds[0]);
    // Non-blocking writes: a stalled encoder costs a dropped frame, never a
    // frozen compositor.
    (void)fcntl(pipe_fds[1], F_SETFL, fcntl(pipe_fds[1], F_GETFL, 0) | O_NONBLOCK);
    // One 1080p frame is 8.3 MB; a larger pipe means fewer write() round-trips
    // through the event loop.
    (void)fcntl(pipe_fds[1], F_SETPIPE_SZ, 1 << 20);

    m_capture_fd = pipe_fds[1];
    m_encoder_pid = pid;
    m_local_encoder = true;
    m_stream_open = true;
    outln("Recorder: local encoder {} (pid {}) -> {}", encoder, pid, m_last_output_path);
    return true;
}

bool Recorder::open_capture_stream(int width, int height)
{
    // Transport selection:
    //   SERENITY_RECORDER_HOST set -> raw frames over TCP to a host-side encoder
    //     (the QEMU setup: no ffmpeg inside the guest, 10.0.2.2 is the host).
    //   otherwise -> local ffmpeg child over a pipe (the gsr model, and the
    //     only option on real hardware where 10.0.2.2 does not exist).
    char const* host_value = getenv("SERENITY_RECORDER_HOST");
    if (!host_value || !*host_value || !strcmp(host_value, "local"))
        return open_local_encoder(width, height);

    auto host = ByteString { host_value };
    u16 port = default_host_port;
    if (char const* value = getenv("SERENITY_RECORDER_PORT"); value && *value) {
        if (auto parsed = ByteString { value }.to_number<u16>(); parsed.has_value())
            port = *parsed;
    }

    auto fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        warnln("Recorder: socket() failed: {}", strerror(errno));
        return false;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.characters(), &address.sin_addr) != 1) {
        warnln("Recorder: invalid host '{}'", host);
        close(fd);
        return false;
    }

    // connect() must never block the compositor: the default encoder address
    // (10.0.2.2) is QEMU's host alias and does not exist on a real machine, so
    // a blocking connect waits for the kernel's SYN retries (~2 minutes) on the
    // frame path - the desktop appears to hard-freeze as soon as recording
    // starts. Connect non-blocking with a 500ms deadline, then return to
    // blocking sends (SO_SNDTIMEO below is the safety net for a stalled
    // encoder).
    int socket_flags = fcntl(fd, F_GETFL, 0);
    if (socket_flags < 0 || fcntl(fd, F_SETFL, socket_flags | O_NONBLOCK) < 0) {
        warnln("Recorder: cannot switch socket to non-blocking: {}", strerror(errno));
        close(fd);
        return false;
    }
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
            warnln("Recorder: cannot reach the encoder at {}:{} ({})", host, port, strerror(errno));
            close(fd);
            return false;
        }
        struct pollfd poll_fd {
            fd, POLLOUT, 0
        };
        int ready = poll(&poll_fd, 1, 500);
        int socket_error = 0;
        socklen_t socket_error_length = sizeof(socket_error);
        if (ready <= 0
            || getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_length) < 0
            || socket_error != 0) {
            warnln("Recorder: encoder at {}:{} did not accept within 500ms ({})",
                host, port, ready <= 0 ? "timeout" : strerror(socket_error));
            close(fd);
            return false;
        }
    }
    (void)fcntl(fd, F_SETFL, socket_flags);

    // Nagle would add 40ms of latency to every frame; there is exactly one
    // writer and the frames are already large, so it buys us nothing.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Frames are written with a blocking send plus a short send timeout.
    // A non-blocking socket only advances one kernel buffer per event-loop
    // wakeup, which caps the stream at ~10 fps for a 1080p frame; blocking
    // lets the kernel push the whole frame as the encoder drains it. The
    // timeout is the safety net: a dead encoder must not freeze the desktop.
    struct timeval timeout {
        0, 250 * 1000
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    m_capture_fd = fd;
    m_stream_open = true;
    return true;
}

void Recorder::close_capture_stream()
{
    if (m_capture_fd >= 0) {
        if (m_local_encoder) {
            // Closing stdin gives ffmpeg EOF; wait (briefly) for it to finish
            // the mp4 so the file is complete when we announce it.
            close(m_capture_fd);
            m_capture_fd = -1;
            if (m_encoder_pid > 0) {
                int status = 0;
                for (int attempt = 0; attempt < 300; ++attempt) {
                    if (waitpid(m_encoder_pid, &status, WNOHANG) != 0)
                        break;
                    usleep(10 * 1000);
                }
                m_encoder_pid = -1;
            }
        } else {
            // Half-close so the host encoder sees a clean EOF and finalises it.
            shutdown(m_capture_fd, SHUT_WR);
            close(m_capture_fd);
            m_capture_fd = -1;
        }
    }
    m_capture_fd = -1;
    m_local_encoder = false;
    m_stream_open = false;
    m_frame_in_flight = false;
    m_frame_offset = 0;
    m_frame_size = 0;
}

void Recorder::on_frame_tick()
{
    if (m_state != State::Recording)
        return;

    auto& screen = Screen::main();
    auto const& front = Compositor::the().front_bitmap_for_recorder({}, screen);
    auto source_width = front.width();
    auto source_height = front.height();
    if (source_width <= 0 || source_height <= 0)
        return;

    // The 1152 cap existed for QEMU's ~130 MB/s user-mode network. A local
    // encoder has no such ceiling: keep full resolution unless the user asked
    // for a specific width, or the TCP transport is in use.
    auto max_capture_width = s_max_capture_width;
    char const* host_value = getenv("SERENITY_RECORDER_HOST");
    bool const local_encoder = !host_value || !*host_value || !strcmp(host_value, "local");
    if (local_encoder && !getenv("SERENITY_RECORDER_WIDTH"))
        max_capture_width = 4096;

    auto width = source_width;
    auto height = source_height;
    if (width > max_capture_width) {
        height = (int)((long)source_height * max_capture_width / source_width);
        width = max_capture_width;
        if (height <= 0)
            height = 1;
    }
    // H.264 4:2:0 needs even dimensions.
    width &= ~1;
    height &= ~1;
    if (width < 2 || height < 2)
        return;

    auto stride = (size_t)width * 4;
    auto needed = stride * (size_t)height;

    if (!m_stream_open) {
        if (!open_capture_stream(width, height)) {
            m_capture_failed = true;
            stop_recording();
            show_toast("Recording backend is not running"sv);
            return;
        }
        // The local ffmpeg child has width/height on its command line; only
        // the TCP protocol needs the extra header line.
        if (!m_local_encoder) {
            auto header = ByteString::formatted("SERENITYREC 1 {} {} {} {}\n", width, height, m_fps, m_current_filename);
            auto written = send(m_capture_fd, header.characters(), header.length(), MSG_NOSIGNAL);
            if (written != (ssize_t)header.length()) {
                warnln("Recorder: failed to send stream header");
                close_capture_stream();
                m_capture_failed = true;
                stop_recording();
                show_toast("Recording backend rejected the stream"sv);
                return;
            }
        }
    }

    if (m_frame_buffer.size() < needed)
        m_frame_buffer.resize(needed);

    if (width == source_width && height == source_height) {
        auto* destination = m_frame_buffer.data();
        for (int y = 0; y < height; ++y)
            memcpy(destination + (size_t)y * stride, front.scanline(y), stride);
    } else {
        if (!m_scaled_frame || m_scaled_frame->size() != Gfx::IntSize { width, height })
            m_scaled_frame = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { width, height }));
        Gfx::Painter painter { *m_scaled_frame };
        painter.draw_scaled_bitmap(m_scaled_frame->rect(), front, front.rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);
        auto* destination = m_frame_buffer.data();
        for (int y = 0; y < height; ++y)
            memcpy(destination + (size_t)y * stride, m_scaled_frame->scanline(y), stride);
    }

    m_frame_size = needed;
    m_frame_offset = 0;
    m_frame_in_flight = true;
    pump_pending_frame();
}

void Recorder::pump_pending_frame()
{
    while (m_capture_fd >= 0 && m_frame_offset < m_frame_size) {
        auto remaining = m_frame_size - m_frame_offset;
        // A pipe is not a socket: write() instead of send(). MSG_NOSIGNAL has
        // no equivalent, so SIGPIPE is ignored for the whole process below.
        auto sent = m_local_encoder
            ? (ssize_t)::write(m_capture_fd, m_frame_buffer.data() + m_frame_offset, remaining)
            : send(m_capture_fd, m_frame_buffer.data() + m_frame_offset, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_SNDTIMEO expired: the encoder stopped reading. Cut the
                // recording rather than keep stalling the compositor.
                warnln("Recorder: encoder stopped accepting frames, aborting");
                m_capture_failed = true;
                stop_recording();
                show_toast("Recording backend stalled"sv);
                return;
            }
            warnln("Recorder: frame write failed: {}", strerror(errno));
            m_capture_failed = true;
            stop_recording();
            show_toast("Recording backend disconnected"sv);
            return;
        }
        m_frame_offset += (size_t)sent;
    }

    m_frame_in_flight = false;
}

// ── recording control ───────────────────────────────────────────────────────

void Recorder::start_recording()
{
    if (m_state != State::Idle)
        return;

    m_capture_failed = false;
    m_accumulated_ms = 0;
    m_started_at_ms = monotonic_ms();

    auto directory = m_output_directory.is_empty() ? Core::StandardPaths::home_directory() : m_output_directory;
    ensure_directory_exists(directory);

    m_current_filename = ByteString::formatted("serenity-rec-{}.mp4", Core::DateTime::now().to_byte_string("%Y-%m-%d-%H-%M-%S"sv));
    m_last_output_path = ByteString::formatted("{}/{}", directory, m_current_filename);

    m_state = State::Recording;

    if (!m_frame_timer) {
        m_frame_timer = Core::Timer::create_repeating(max(1000 / m_fps, 1), [this] { on_frame_tick(); });
    }
    m_frame_timer->set_interval(max(1000 / m_fps, 1));
    m_frame_timer->start();

    update_pill();
    notify("Screen Recorder"sv, "Recording started"sv, m_current_filename, 2500);
    outln("Recorder: recording to {} ({} fps)", m_last_output_path, m_fps);
}

void Recorder::stop_recording()
{
    if (m_state == State::Idle)
        return;

    if (m_state == State::Recording)
        m_accumulated_ms += monotonic_ms() - m_started_at_ms;

    m_state = State::Idle;
    if (m_frame_timer)
        m_frame_timer->stop();
    close_capture_stream();
    hide_pill();

    if (!m_capture_failed) {
        show_toast(ByteString::formatted("Saving {}...", m_current_filename));
        start_saved_file_poll(m_last_output_path);
    }
    outln("Recorder: stopped ({})", m_last_output_path);
}

void Recorder::toggle_recording()
{
    if (m_state == State::Idle)
        start_recording();
    else
        stop_recording();
}

void Recorder::toggle_paused()
{
    if (m_state == State::Recording) {
        m_accumulated_ms += monotonic_ms() - m_started_at_ms;
        m_state = State::Paused;
        // Not sending frames is the pause: the encoder simply gets no input,
        // so the gap does not end up in the file.
        update_pill();
        notify("Screen Recorder"sv, "Recording paused"sv, {}, 2200);
        return;
    }
    if (m_state == State::Paused) {
        m_started_at_ms = monotonic_ms();
        m_state = State::Recording;
        update_pill();
        notify("Screen Recorder"sv, "Recording resumed"sv, {}, 2200);
    }
}

// ── screenshots ─────────────────────────────────────────────────────────────

void Recorder::take_screenshot()
{
    auto& screen = Screen::main();
    auto const& front = Compositor::the().front_bitmap_for_recorder({}, screen);

    auto directory = Core::StandardPaths::pictures_directory();
    if (directory.is_empty())
        directory = Core::StandardPaths::home_directory();
    ensure_directory_exists(directory);

    auto path = ByteString::formatted("{}/screenshot-{}.png", directory, Core::DateTime::now().to_byte_string("%Y-%m-%d-%H-%M-%S"sv));

    auto encoded = Gfx::PNGWriter::encode(front);
    if (encoded.is_error()) {
        warnln("Recorder: PNG encode failed: {}", encoded.error());
        show_toast("Screenshot failed"sv);
        return;
    }

    auto file = Core::File::open(path, Core::File::OpenMode::Write);
    if (file.is_error()) {
        warnln("Recorder: cannot write {}: {}", path, file.error());
        show_toast("Screenshot failed"sv);
        return;
    }
    auto& handle = *file.value();
    if (auto result = handle.write_until_depleted(encoded.value().bytes()); result.is_error()) {
        warnln("Recorder: short write for {}: {}", path, result.error());
        show_toast("Screenshot failed"sv);
        return;
    }

    m_last_output_path = path;
    notify("Screenshots"sv, "Screenshot saved"sv, path, 4000);
    outln("Recorder: screenshot written to {}", path);
}

// ── overlays ────────────────────────────────────────────────────────────────

void Recorder::update_pill()
{
    if (m_state == State::Idle) {
        hide_pill();
        return;
    }

    if (!m_pill)
        m_pill = make<RecordingPillOverlay>();
    m_pill->place(Screen::main());
    m_pill->set_state(m_state == State::Paused, elapsed_seconds());
    if (!m_pill->is_enabled())
        m_pill->set_enabled(true);

    if (!m_clock_timer) {
        m_clock_timer = Core::Timer::create_repeating(1000, [this] {
            if (m_state == State::Idle) {
                hide_pill();
                return;
            }
            update_pill();
        });
    }
    m_clock_timer->start();
}

void Recorder::hide_pill()
{
    if (m_clock_timer)
        m_clock_timer->stop();
    if (m_pill && m_pill->is_enabled())
        m_pill->set_enabled(false);
}

void Recorder::show_toast(ByteString const& text)
{
    notify("Serenity"sv, text, {}, 3500);
}

void Recorder::notify(ByteString const& app_name, ByteString const& summary, ByteString const& body, int timeout_ms)
{
    outln("Recorder: {}: {}{}", app_name, summary, body.is_empty() ? "" : ByteString::formatted(" ({})", body));
    NotificationCenter::the().post(app_name, summary, body, timeout_ms);
}

void Recorder::start_saved_file_poll(ByteString const& path)
{
    m_saved_poll_path = path;
    m_saved_poll_attempts = 0;
    if (!m_saved_poll_timer) {
        m_saved_poll_timer = Core::Timer::create_repeating(500, [this] {
            ++m_saved_poll_attempts;
            if (FileSystem::exists(m_saved_poll_path)) {
                m_saved_poll_timer->stop();
                notify("Screen Recorder"sv, "Recording saved"sv, m_saved_poll_path, 5000);
                return;
            }
            // The host needs time to finish encoding and copy the file back.
            if (m_saved_poll_attempts > 240) {
                m_saved_poll_timer->stop();
                notify("Screen Recorder"sv, "Recording saved on the host"sv, m_saved_poll_path, 5000);
            }
        });
    }
    m_saved_poll_timer->start();
}

}
