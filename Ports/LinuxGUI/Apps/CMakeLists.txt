# SerenityOS GUI applications built for the host.
#
# AnalogClock is the smallest app that still exercises LibGUI widgets, GML-free
# window creation, menus and timers, so it is the bring-up target.

set(APP_DIR "${SERENITY_PROJECT_ROOT}/Userland/Applications/AnalogClock")

add_executable(serenity-analog-clock
    ${APP_DIR}/main.cpp
    ${APP_DIR}/AnalogClock.cpp
)

target_include_directories(serenity-analog-clock PRIVATE ${APP_DIR})
target_link_libraries(serenity-analog-clock PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
)

set_target_properties(serenity-analog-clock PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Screenshot & Recorder control panel.
#
# Replaces the upstream Screenshot app for the port: upstream shells out to
# /bin/shot (never built here) and knows nothing about the compositor's
# recorder, so the desktop's screenshot tool would be dead. This one drives
# WindowServer's Recorder over /tmp/serenity-recorder.sock.
add_executable(serenity-screenshot
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/ScreenshotApp/main.cpp
)
target_link_libraries(serenity-screenshot PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
)
set_target_properties(serenity-screenshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Every other bundled GUI app/game/demo, converted to a host target.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-apps.cmake)

# Video Player backed by libmpv (dlopen'ed, MPV_RENDER_API_TYPE_SW).
#
# Replaces the upstream VideoPlayer for the port: LibMedia only decodes VP8/VP9
# inside Matroska, so MP4/H.264, AVI/MPEG-4, MOV, MKV/AV1 etc. were unplayable.
# libmpv renders into a memory surface that we wrap as a Gfx::Bitmap; no GL or
# Vulkan context is needed (this VM only has software rendering).
# The upstream VideoFrameWidget is reused for painting and sizing modes.
add_executable(serenity-video-player
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/VideoPlayer/main.cpp
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/VideoPlayer/MpvBackend.cpp
    ${SERENITY_PROJECT_ROOT}/Userland/Applications/VideoPlayer/VideoFrameWidget.cpp
)
target_include_directories(serenity-video-player PRIVATE
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/VideoPlayer
    ${SERENITY_PROJECT_ROOT}/Userland/Applications/VideoPlayer
)
target_link_libraries(serenity-video-player PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
    ${CMAKE_DL_LIBS}
)
set_target_properties(serenity-video-player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Control Panel applet: File Associations. Lists every extension declared by an
# installed .af plus common formats, and lets the user pick the default program.
# Overrides go to ~/.config/LaunchServer.ini; LaunchServer is then told over IPC
# to reload so the change applies without a re-login.
add_executable(serenity-file-associations
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/FileAssociations/main.cpp
)
target_link_libraries(serenity-file-associations PRIVATE
    LibCore
    LibDesktop
    LibGfx
    LibGUI
    LibMain
)
set_target_properties(serenity-file-associations PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# `serenity-open <path>`: tiny CLI that resolves a file through LaunchServer's
# associations. Handy for verifying double-click behaviour without a GUI.
add_executable(serenity-open
    ${SERENITY_PROJECT_ROOT}/Userland/Utilities/open.cpp
)
target_link_libraries(serenity-open PRIVATE
    LibCore
    LibDesktop
    LibFileSystem
    LibMain
    LibURL
)
set_target_properties(serenity-open PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# CLI audio client (upstream utility), used to exercise the audio stack without
# a GUI: serenity-aplay <file> -> AudioServer -> /dev/audio/0 (kernel module).
add_executable(serenity-aplay ${SERENITY_PROJECT_ROOT}/Userland/Utilities/aplay.cpp)
target_link_libraries(serenity-aplay PRIVATE LibAudio LibFileSystem LibCore LibMain)
set_target_properties(serenity-aplay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Upstream utility behind ClockSettings' time zone tab: the app spawns
# "/bin/timezone <zone>" (hostify maps it here) to rewrite /etc/timezone.
# LibTimeZone is an OBJECT library that Lagom always builds (see Meta/Lagom).
add_executable(serenity-timezone ${SERENITY_PROJECT_ROOT}/Userland/Utilities/timezone.cpp)
target_link_libraries(serenity-timezone PRIVATE LibTimeZone LibCore LibMain)
set_target_properties(serenity-timezone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Upstream's SystemMonitor links LibSymbolication, which is not part of the
# Lagom build, so the app generator skips it. This small replacement reads
# /proc directly and keeps "/bin/SystemMonitor" (taskbar menu, Ctrl+Alt+Del)
# working.
add_executable(serenity-system-monitor
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/SystemMonitorLite/main.cpp
)
target_link_libraries(serenity-system-monitor PRIVATE
    LibCore
    LibFileSystem
    LibGfx
    LibGUI
    LibMain
)
set_target_properties(serenity-system-monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 每进程网络流量采集器（eBPF：kprobe tcp_sendmsg/tcp_recvmsg + hash map）。
# 纯 C，不链接 Serenity 库；需要 root（CAP_BPF）才能加载程序。
add_executable(serenity-netprobe
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/SystemMonitorLite/netprobe.c
)
set_target_properties(serenity-netprobe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Minimal native LibGUI app whose main widget is a focused TextEditor. Used to
# exercise the IME client (candidate window + taskbar indicator) in isolation,
# without depending on window-manager focus or z-order.
add_executable(serenity-imetest
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/IMEFrontend/imetest.cpp
)
target_link_libraries(serenity-imetest PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
)
set_target_properties(serenity-imetest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Native Notepad: a LibGUI text editor with the classic Notepad feature set
# (File/Edit/Format/Help, open/save with encoding + line-ending detection,
# syntax highlighting by extension, clipboard, undo), drawn with the port's
# own theme. Replaces the GTK mousepad shim behind /bin/TextEditor, which
# crashed easily.
add_executable(serenity-notepad
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/Notepad/main.cpp
)
target_link_libraries(serenity-notepad PRIVATE
    LibCMake
    LibConfig
    LibCore
    LibCpp
    LibFileSystemAccessClient
    LibGfx
    LibGUI
    LibJS
    LibMain
    LibMarkdown
    LibShell
    LibSyntax
    LibTextCodec
)
set_target_properties(serenity-notepad PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Native Windows-application launcher. Owns the launch policy (app vs game,
# runtime selection, environment, prefix prep) in C++, mirroring omikuji's
# assemble_launch/WineVariant, instead of a /bin/sh script. No Serenity libs:
# it is a plain POSIX exec shim that replaces itself with wine / umu-run.
add_executable(serenity-winapp
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/WinAppLauncher/main.cpp
)
set_target_properties(serenity-winapp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Input method settings (Control Panel -> Other). Reads/writes rime's custom
# YAML directly, runs rime_deployer, and notifies the fcitx5 addon over the
# control socket. The live candidate preview reuses GUI::InputMethodPanelWidget.
add_executable(serenity-ime-settings
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/IMESettings/main.cpp
)
target_link_libraries(serenity-ime-settings PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
)
set_target_properties(serenity-ime-settings PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Temporary repro harness for the FileSystemAccessServer crash (remove when fixed).
add_executable(serenity-fspicktest ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/FsPickTest/main.cpp)
target_include_directories(serenity-fspicktest PRIVATE ${SERENITY_PROJECT_ROOT}/Userland/Services/FileSystemAccessServer)
target_link_libraries(serenity-fspicktest PRIVATE LibCore LibFileSystemAccessClient LibGUI LibMain)
