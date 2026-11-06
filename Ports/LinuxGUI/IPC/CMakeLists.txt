# Shared IPC endpoints for the host GUI build.
#
# LibGUI, LibConfig and WindowServer all consume these generated headers via
# <WindowServer/...>, <Clipboard/...> and <NotificationServer/...>, which resolve
# through the global include dir ${CMAKE_BINARY_DIR}/Userland/Services.

set(SERVICES_BIN "${CMAKE_BINARY_DIR}/Userland/Services")
set(SERVICES_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services")

compile_ipc(${SERVICES_SRC}/WindowServer/WindowServer.ipc
    ${SERVICES_BIN}/WindowServer/WindowServerEndpoint.h)
compile_ipc(${SERVICES_SRC}/WindowServer/WindowClient.ipc
    ${SERVICES_BIN}/WindowServer/WindowClientEndpoint.h)
compile_ipc(${SERVICES_SRC}/WindowServer/WindowManagerServer.ipc
    ${SERVICES_BIN}/WindowServer/WindowManagerServerEndpoint.h)
compile_ipc(${SERVICES_SRC}/WindowServer/WindowManagerClient.ipc
    ${SERVICES_BIN}/WindowServer/WindowManagerClientEndpoint.h)

compile_ipc(${SERVICES_SRC}/Clipboard/ClipboardClient.ipc
    ${SERVICES_BIN}/Clipboard/ClipboardClientEndpoint.h)
compile_ipc(${SERVICES_SRC}/Clipboard/ClipboardServer.ipc
    ${SERVICES_BIN}/Clipboard/ClipboardServerEndpoint.h)

compile_ipc(${SERVICES_SRC}/NotificationServer/NotificationClient.ipc
    ${SERVICES_BIN}/NotificationServer/NotificationClientEndpoint.h)
compile_ipc(${SERVICES_SRC}/NotificationServer/NotificationServer.ipc
    ${SERVICES_BIN}/NotificationServer/NotificationServerEndpoint.h)

# LaunchServer endpoints: LibDesktop needs them on the host (on SerenityOS the
# service build generates them).
compile_ipc(${SERVICES_SRC}/LaunchServer/LaunchServer.ipc
    ${SERVICES_BIN}/LaunchServer/LaunchServerEndpoint.h)
compile_ipc(${SERVICES_SRC}/LaunchServer/LaunchClient.ipc
    ${SERVICES_BIN}/LaunchServer/LaunchClientEndpoint.h)

# FileSystemAccess endpoints: LibFileSystemAccessClient needs them on the host.
compile_ipc(${SERVICES_SRC}/FileSystemAccessServer/FileSystemAccessServer.ipc
    ${SERVICES_BIN}/FileSystemAccessServer/FileSystemAccessServerEndpoint.h)
compile_ipc(${SERVICES_SRC}/FileSystemAccessServer/FileSystemAccessClient.ipc
    ${SERVICES_BIN}/FileSystemAccessServer/FileSystemAccessClientEndpoint.h)

