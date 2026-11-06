# Linux GUI port / clean source:
# Only FileOperation is built on the host. The generic daemon services that
# used to live here (EchoServer, LookupServer, SSHServer, WebServer,
# ChessEngine, CrashDaemon, DeviceMapper, DHCPClient, KeyboardPreferenceLoader,
# NetworkServer, SpiceAgent, TelnetServer) all have mature Linux equivalents
# (sshd, nginx, systemd-resolved, NetworkManager, systemd-coredump,
# spice-vdagent, ...) and are not part of the desktop port, so they are removed
# from this clean tree.
add_subdirectory(FileOperation)

if (SERENITYOS)
    # Kept because the Linux port reuses their sources and/or IPC endpoints.
    add_subdirectory(AudioServer)
    add_subdirectory(Clipboard)
    add_subdirectory(ConfigServer)
    add_subdirectory(FileSystemAccessServer)
    add_subdirectory(ImageDecoder)
    add_subdirectory(LaunchServer)
    add_subdirectory(LoginServer)
    add_subdirectory(NotificationServer)
    add_subdirectory(RequestServer)
    add_subdirectory(SQLServer)
    add_subdirectory(SystemServer)
    add_subdirectory(Taskbar)
    add_subdirectory(WebContent)
    add_subdirectory(WebDriver)
    add_subdirectory(WebWorker)
    add_subdirectory(WindowServer)
endif()
