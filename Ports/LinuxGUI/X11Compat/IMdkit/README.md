# Vendored IMdkit (XIM server protocol implementation)

Source: `ibus-src/util/IMdkit` (from the ibus source tree), which is the X11R6
Xi18n IMdkit originally written by Sun Microsystems / HP and maintained by
the ibus project. Used here to give the Serenity bridge a **real XIM server**
instead of re-implementing the wire protocol by hand.

Only the X transport is used (`IMServerTransport "X/"`); the local/socket
transport code is compiled but never selected.

Why vendored: same reasoning as `wlroots/` — pin the protocol implementation,
avoid depending on whatever libIMdkit the distro happens to ship, keep the
license boundary explicit (these files carry their own Sun/HP/ibus notices).

The bridge owns the Xlib Display and calls `XNextEvent()` from its LibCore
event loop; IMdkit installs an Xlib event filter (`_XRegisterFilterByType`),
so XIM ClientMessages are decoded automatically and dispatched to our
`IMProtocolHandler` callback.
