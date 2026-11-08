# Host (Linux) build of the SerenityOS services that make up a desktop session.
#
# These are ordinary daemons/apps, so they build unmodified except for the two
# main.cpp files, which cannot rely on SystemServer passing in a socket.

set(SERVICES_DIR "${SERENITY_PROJECT_ROOT}/Userland/Services")

set(LAUNCH_IPC_HEADERS
    ${CMAKE_BINARY_DIR}/Userland/Services/LaunchServer/LaunchServerEndpoint.h
    ${CMAKE_BINARY_DIR}/Userland/Services/LaunchServer/LaunchClientEndpoint.h
)

# --- ConfigServer ----------------------------------------------------------
add_executable(serenity-config-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_config_server_linux.cpp
    ${SERVICES_DIR}/ConfigServer/ConnectionFromClient.cpp
)
add_dependencies(serenity-config-server
    generate_ConfigServerEndpoint.h
    generate_ConfigClientEndpoint.h
)
target_include_directories(serenity-config-server PRIVATE ${SERVICES_DIR}/ConfigServer)
target_link_libraries(serenity-config-server PRIVATE LibCore LibIPC LibMain)

# --- LaunchServer ----------------------------------------------------------
add_executable(serenity-launch-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_launch_server_linux.cpp
    ${SERVICES_DIR}/LaunchServer/ConnectionFromClient.cpp
    ${SERVICES_DIR}/LaunchServer/Launcher.cpp
)
add_dependencies(serenity-launch-server
    generate_LaunchServerEndpoint.h
    generate_LaunchClientEndpoint.h
)
target_include_directories(serenity-launch-server PRIVATE ${SERVICES_DIR}/LaunchServer)
target_link_libraries(serenity-launch-server PRIVATE LibCore LibIPC LibDesktop LibFileSystem LibMain LibURL)

# --- Clipboard ------------------------------------------------------------
# Cross-application copy/paste. Without it every app logs
# "no clipboard server available" and Ctrl+C/Ctrl+V silently does nothing.
add_executable(serenity-clipboard-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_clipboard_server_linux.cpp
    ${SERVICES_DIR}/Clipboard/ConnectionFromClient.cpp
    ${SERVICES_DIR}/Clipboard/Storage.cpp
)
add_dependencies(serenity-clipboard-server
    generate_ClipboardServerEndpoint.h
    generate_ClipboardClientEndpoint.h
)
target_include_directories(serenity-clipboard-server PRIVATE ${SERVICES_DIR}/Clipboard)
target_link_libraries(serenity-clipboard-server PRIVATE LibCore LibIPC LibMain)

# --- FileSystemAccessServer --------------------------------------------------
# Backs File / Open / Save As in Notepad and the other ported GUI apps. It pops
# up a file picker, so it is a full LibGUI client of WindowServer; spawn it only
# after WindowServer is up.
add_executable(serenity-file-system-access-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_file_system_access_server_linux.cpp
    ${SERVICES_DIR}/FileSystemAccessServer/ConnectionFromClient.cpp
)
add_dependencies(serenity-file-system-access-server
    generate_FileSystemAccessServerEndpoint.h
    generate_FileSystemAccessClientEndpoint.h
)
target_include_directories(serenity-file-system-access-server PRIVATE ${SERVICES_DIR}/FileSystemAccessServer)
target_link_libraries(serenity-file-system-access-server PRIVATE
    LibCore LibFileSystem LibGfx LibGUI LibIPC LibMain LibURL)

# --- Taskbar ---------------------------------------------------------------
# The taskbar lives inside serenity-shell now (Ports/LinuxGUI/Shell): one
# process for desktop + taskbar, sharing the app catalog and icon cache.
# Userland/Services/Taskbar/main.cpp is still built for the native SerenityOS
# target, but the Linux port no longer ships a standalone taskbar.

# --- FileOperation -----------------------------------------------------------
# Helper that FileManager forks+execs for copy/move/delete and reads progress
# lines ("PROGRESS ..."/"FINISH"/"ERROR ...") from a pipe. Without it every
# delete/copy in FileManager fails with "Read from pipe returned null".
add_executable(serenity-file-operation
    ${SERVICES_DIR}/FileOperation/main.cpp
)
target_link_libraries(serenity-file-operation PRIVATE LibCore LibMain)

foreach(target serenity-config-server serenity-launch-server serenity-clipboard-server serenity-file-operation)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endforeach()

# --- LoginServer -----------------------------------------------------------
# Reuses the upstream LoginWindow (GML), but hands off to the host session
# script instead of SystemServer.
set(LOGIN_DIR "${SERVICES_DIR}/LoginServer")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LoginWindowGML.cpp
    COMMAND sh -c "$<TARGET_FILE:Lagom::GMLCompiler> '${LOGIN_DIR}/LoginWindow.gml' > '${CMAKE_CURRENT_BINARY_DIR}/LoginWindowGML.cpp'"
    DEPENDS Lagom::GMLCompiler ${LOGIN_DIR}/LoginWindow.gml
    VERBATIM
)

add_executable(serenity-login-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_login_server_linux.cpp
    ${LOGIN_DIR}/LoginWindow.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LoginWindowGML.cpp
)
target_include_directories(serenity-login-server PRIVATE ${LOGIN_DIR} ${SERENITY_PROJECT_ROOT}/Userland/Services)
target_link_libraries(serenity-login-server PRIVATE LibCore LibFileSystem LibGfx LibGUI LibMain)
set_target_properties(serenity-login-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- AudioServer -----------------------------------------------------------
# Upstream mixer, host-side entry point. Talks to /dev/audio/0 which the
# serenity_compat kernel module provides.
set(AUDIO_SERVER_DIR "${SERVICES_DIR}/AudioServer")
set(AUDIO_ENDPOINT_DIR "${CMAKE_BINARY_DIR}/Userland/Services/AudioServer")

add_executable(serenity-audio-server
    ${CMAKE_CURRENT_SOURCE_DIR}/main_audio_server_linux.cpp
    ${AUDIO_SERVER_DIR}/Mixer.cpp
    ${AUDIO_SERVER_DIR}/ClientAudioStream.cpp
    ${AUDIO_SERVER_DIR}/ConnectionFromClient.cpp
    ${AUDIO_SERVER_DIR}/ConnectionFromManagerClient.cpp
)
target_include_directories(serenity-audio-server PRIVATE
    ${AUDIO_SERVER_DIR}
    ${SERENITY_PROJECT_ROOT}/Userland/Services
    ${AUDIO_ENDPOINT_DIR}
)
# Mixer.cpp includes <Kernel/API/Ioctl.h> (a SerenityOS kernel header) for the
# sound card ioctls; force-include the host equivalent instead of patching it.
target_compile_options(serenity-audio-server PRIVATE
    -include ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/Common/SerenityIoctl.h
)
target_link_libraries(serenity-audio-server PRIVATE LibCore LibThreading LibIPC LibAudio LibMain)
set_target_properties(serenity-audio-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- PrintService ----------------------------------------------------------
# Session-side printing front-end: apps ask it for printers / submit jobs
# instead of shelling out to lp(1) themselves. CUPS remains the real queue
# manager; this only wraps `lpstat -e` / `lp` behind a Unix socket.
add_executable(serenity-print-service
    ${CMAKE_CURRENT_SOURCE_DIR}/main_print_service.cpp
)
# Notifications go out over the session bus (org.freedesktop.Notifications is
# served by our compositor). The PkgConfig::DBUS imported target is scoped to
# the parent directory, so use the variables, which do inherit.
target_include_directories(serenity-print-service PRIVATE ${DBUS_INCLUDE_DIRS} ${SERENITY_PROJECT_ROOT})
target_link_libraries(serenity-print-service PRIVATE LibCore LibGfx LibCompress AK ${DBUS_LIBRARIES})
set_target_properties(serenity-print-service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
