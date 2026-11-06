serenity_component(
    WinAppsSettings
    REQUIRED
    TARGETS WinAppsSettings
)

set(SOURCES
    main.cpp
)

# "Windows 程序管理": install / launch / uninstall Windows games & programs.
# Each program gets an isolated WINEPREFIX + a launcher wrapper (see
# Ports/LinuxGUI/Common/WinApps.h).
serenity_app(WinAppsSettings ICON app-brickgame)
target_link_libraries(WinAppsSettings PRIVATE LibCore LibGfx LibGUI LibDesktop LibConfig LibMain LibFileSystem LibURL)
