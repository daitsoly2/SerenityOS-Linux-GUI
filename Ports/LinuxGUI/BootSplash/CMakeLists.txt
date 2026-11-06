# Win9x-style boot splash for the desktop session.

add_executable(serenity-boot-splash boot_splash.cpp)
target_link_libraries(serenity-boot-splash PRIVATE LibCore LibGfx LibGUI LibMain)
set_target_properties(serenity-boot-splash PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Early progress screen: talks to /dev/fb0 directly, no window system involved,
# so it can be started by systemd long before WindowServer is up.
add_executable(serenity-fb-splash fb_splash.cpp)
set_target_properties(serenity-fb-splash PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
