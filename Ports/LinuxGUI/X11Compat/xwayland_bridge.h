/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// XwaylandBridge owns the whole lifecycle of the X server that backs X11
// clients, so the compositor never touches fork/exec or the X socket directly:
//
//   * launching it - today a child process; the fusion plan replaces
//     launch_child() with an in-process thread that shares this process's
//     wl_display (see HANDOFF notes / memory 2026-11-04),
//   * waiting until it can answer X connection setups (its -displayfd signal),
//   * attaching XWM to it, and
//   * reaping the child on shutdown.
//
// Everything above launch_child() is backend-agnostic: swapping "spawn a child"
// for "run Xwayland in this process" is a one-method change.

#pragma once

#include <AK/ByteString.h>
#include <sys/types.h>

class XwaylandBridge {
public:
    XwaylandBridge() = default;
    ~XwaylandBridge();

    XwaylandBridge(XwaylandBridge const&) = delete;
    XwaylandBridge& operator=(XwaylandBridge const&) = delete;

    // display: X display to use (":100"). wayland_socket: our own Wayland
    // socket name; a child Xwayland must inherit it via WAYLAND_DISPLAY.
    // external: don't launch anything, just wait for an externally managed
    // X server (SERENITY_XWAYLAND_EXTERNAL=1) to answer.
    void start(ByteString display, ByteString wayland_socket, bool external);

    // Drive readiness + XWM attachment. Call from a periodic timer; the caller
    // pumps the Wayland loop first so Xwayland's startup roundtrips can make
    // progress. Returns true once XWM is attached (idempotent afterwards).
    bool poll();

    bool connected() const { return m_connected; }
    ByteString const& display() const { return m_display; }
    // Wayland object ids are per-client, so the X11 front-end needs to know
    // which wl_client is Xwayland before it can pair WL_SURFACE_ID to a
    // wlr_surface. Exposing the child pid is the whole adaptation needed to
    // reuse this bridge from serenity-wlroots-host. -1 in external mode.
    pid_t pid() const { return m_child_pid; }

private:
    // Launch policy: fork/exec a child Xwayland. Returns a non-blocking
    // readiness fd that becomes readable when Xwayland is ready, or -1 on
    // failure. This is the ONLY place that knows Xwayland is a separate
    // process, so the in-process backend only has to replace this method.
    int launch_child();

    ByteString m_display;
    ByteString m_wayland_socket;
    bool m_external { false };
    bool m_renderfd_supported { false };
    int m_ready_fd { -1 };
    bool m_ready { false };
    bool m_connected { false };
    pid_t m_child_pid { -1 };
};
