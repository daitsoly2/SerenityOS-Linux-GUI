# The desktop shell: desktop (wallpaper + icons) and taskbar (start menu, window
# list, clock, tray) in a single process, the way explorer.exe does it.
#
# Both halves share one /res/apps scan (AppCatalog), one icon cache and one copy
# of the "/bin/Name" -> "serenity-<kebab-case>" launch rule, and the session
# manager only has to supervise a single binary.

set(TASKBAR_DIR "${SERENITY_PROJECT_ROOT}/Userland/Services/Taskbar")

add_executable(serenity-shell
    main.cpp
    AppCatalog.cpp
    DesktopIconsWidget.cpp
    StartMenuShortcuts.cpp
    StartMenuOverrides.cpp
    SystemMenu.cpp
    ${TASKBAR_DIR}/ClockWidget.cpp
    ${TASKBAR_DIR}/QuickLaunchWidget.cpp
    ${TASKBAR_DIR}/ShutdownDialog.cpp
    ${TASKBAR_DIR}/TaskbarButton.cpp
    ${TASKBAR_DIR}/TaskbarFrame.cpp
    ${TASKBAR_DIR}/TaskbarWindow.cpp
    ${TASKBAR_DIR}/WindowList.cpp
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/IMEFrontend/IMEIndicator.cpp
)

add_dependencies(serenity-shell
    generate_WindowServerEndpoint.h
    generate_WindowClientEndpoint.h
    generate_WindowManagerServerEndpoint.h
    generate_WindowManagerClientEndpoint.h
    generate_ConfigServerEndpoint.h
    generate_ConfigClientEndpoint.h
    generate_LaunchServerEndpoint.h
    generate_LaunchClientEndpoint.h
)

# TaskbarWindow.h includes its siblings by bare name ("ClockWidget.h").
target_include_directories(serenity-shell PRIVATE ${TASKBAR_DIR} ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/IMEFrontend)

target_link_libraries(serenity-shell PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibDesktop
    LibConfig
    LibIPC
    LibMain
    LibURL
    LibFileSystem
)

set_target_properties(serenity-shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
