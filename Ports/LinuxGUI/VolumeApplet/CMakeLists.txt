# Tray volume applet that drives the host mixer (no LibAudio dependency).

add_executable(serenity-volume-applet volume_applet.cpp)
target_link_libraries(serenity-volume-applet PRIVATE LibCore LibGfx LibGUI LibMain)
set_target_properties(serenity-volume-applet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
