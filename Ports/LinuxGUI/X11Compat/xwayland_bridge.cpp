/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "xwayland_bridge.h"
#include "xwm.h"

#include <AK/Format.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {

// Pick the DRM render node that backs the connected display and hand it to
// Xwayland via -renderfd, so its glamor uses real GL instead of the llvmpipe
// software fallback. Without this, Xwayland auto-probes /dev/dri/card0 first; on
// a machine where card0 is a headless compute GPU (e.g. an NVIDIA without
// outputs) it grabs that node, fails glamor init, and every X11 client (Electron,
// Wine, GTK-on-X) runs on software rendering - both slow and crash-prone.
// Returns an open render-node fd (deliberately NOT O_CLOEXEC so it survives the
// exec into Xwayland), or -1 if none could be opened.
int open_render_node_for_display()
{
    char const* cards[] = { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2" };
    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i) {
        int fd = ::open(cards[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmModeRes* res = drmModeGetResources(fd);
        bool connected = false;
        if (res) {
            for (int c = 0; c < res->count_connectors && !connected; ++c) {
                drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
                if (conn) {
                    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
                        connected = true;
                    drmModeFreeConnector(conn);
                }
            }
            drmModeFreeResources(res);
        }
        ::close(fd);
        if (!connected)
            continue;
        // cardN's render node is renderD(128 + N).
        ByteString render_path = ByteString::formatted("/dev/dri/renderD{}", 128 + static_cast<int>(i));
        int rfd = ::open(render_path.characters(), O_RDWR);
        if (rfd >= 0)
            return rfd; // no O_CLOEXEC: survives the exec into Xwayland
    }
    return -1;
}

// -renderfd is NOT a universal Xwayland option: several distro builds reject it
// ("Unrecognized option: -renderfd") and exit immediately. Probe the installed
// binary's help once and only pass it when advertised, so a machine without it
// falls back to the plain (software-glamor) spawn instead of losing X entirely.
bool xwayland_supports_renderfd()
{
    FILE* p = popen("Xwayland -help 2>&1", "r");
    if (!p)
        return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), p)) {
        if (strstr(line, "-renderfd")) {
            found = true;
            break;
        }
    }
    pclose(p);
    return found;
}

} // namespace

XwaylandBridge::~XwaylandBridge()
{
    if (m_child_pid > 0) {
        int status = 0;
        // Reap the child if it already died; never kill a live X server here -
        // session teardown owns that (the in-process backend will stop its
        // thread instead).
        waitpid(m_child_pid, &status, WNOHANG);
    }
    if (m_ready_fd >= 0)
        close(m_ready_fd);
}

void XwaylandBridge::start(ByteString display, ByteString wayland_socket, bool external)
{
    m_display = move(display);
    m_wayland_socket = move(wayland_socket);
    m_external = external;
    if (!m_external) {
        // Decide once, in the parent, whether -renderfd can be used; Xwayland
        // dies on an option it does not know, which would take all X clients
        // with it.
        m_renderfd_supported = xwayland_supports_renderfd();
        m_ready_fd = launch_child();
        if (m_ready_fd < 0)
            warnln("xwayland: spawn failed ({}), continuing without X clients", errno);
    }
}

int XwaylandBridge::launch_child()
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    // The child's end must survive exec: dup without CLOEXEC.
    int wfd = fcntl(fds[1], F_DUPFD, 3);
    close(fds[1]);
    if (wfd < 0) {
        close(fds[0]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(wfd);
        return -1;
    }
    if (pid == 0) {
        char fdstr[16];
        snprintf(fdstr, sizeof(fdstr), "%d", wfd);
        setenv("WAYLAND_DISPLAY", m_wayland_socket.characters(), 1);
        // Only when this Xwayland advertises -renderfd (checked in the parent):
        // hand it the render node of the GPU that owns the connected display so
        // glamor accelerates instead of falling back to llvmpipe.
        if (m_renderfd_supported) {
            int rfd = open_render_node_for_display();
            if (rfd >= 0) {
                // Keep it open across exec (dup to a non-cloexec fd >= 3).
                int rfd_keep = fcntl(rfd, F_DUPFD, 3);
                close(rfd);
                if (rfd_keep >= 0) {
                    char rfdstr[16];
                    snprintf(rfdstr, sizeof(rfdstr), "%d", rfd_keep);
                    execlp("Xwayland", "Xwayland", m_display.characters(), "-rootless",
                        "-displayfd", fdstr, "-renderfd", rfdstr, static_cast<char*>(nullptr));
                }
            }
        }
        execlp("Xwayland", "Xwayland", m_display.characters(), "-rootless", "-displayfd", fdstr, static_cast<char*>(nullptr));
        _exit(127);
    }
    m_child_pid = pid;
    close(wfd);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    return fds[0];
}

bool XwaylandBridge::poll()
{
    if (m_connected) {
        // If the X server died while we were serving clients, reap it so it does
        // not linger as a zombie; a future revision can respawn from here.
        if (m_child_pid > 0) {
            int status = 0;
            if (waitpid(m_child_pid, &status, WNOHANG) > 0) {
                warnln("xwayland: X server exited (status {}); X11 clients will fail", status);
                m_child_pid = -1;
            }
        }
        return true;
    }

    if (m_external) {
        // External mode: probe non-blockingly whether the X server answers.
        if (!XWM::ready(m_display.characters()))
            return false;
    } else if (!m_ready) {
        if (m_ready_fd < 0)
            return false; // spawn failed; nothing to wait for
        char buffer[16];
        ssize_t n;
        bool got_signal = false;
        bool eof = false;
        while ((n = read(m_ready_fd, buffer, sizeof(buffer))) > 0)
            got_signal = true;
        if (n == 0)
            eof = true; // write end closed: Xwayland exited before signalling
        if (got_signal) {
            m_ready = true;
            outln("xwayland: ready ({})", m_display);
        } else if (eof) {
            // Xwayland died before it was ready (bad option, missing GPU, ...).
            // Reap it and stop retrying instead of spinning on a closed pipe.
            int status = 0;
            waitpid(m_child_pid, &status, WNOHANG);
            m_child_pid = -1;
            warnln("xwayland: exited before becoming ready (status {}); continuing without X clients", status);
            close(m_ready_fd);
            m_ready_fd = -1;
            return false;
        } else {
            return false; // EAGAIN: still booting
        }
    }

    // Readiness consumed; the pipe is no longer needed.
    if (m_ready_fd >= 0) {
        close(m_ready_fd);
        m_ready_fd = -1;
    }

    // Xwayland is ready (signalled or probed) - the real connect is safe.
    if (!XWM::connect(m_display.characters()))
        return false;
    m_connected = true;
    outln("xwm: attached to Xwayland on {}", m_display);
    return true;
}
