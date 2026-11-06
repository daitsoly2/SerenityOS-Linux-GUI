# serenity-portal: a Settings-only xdg-desktop-portal for this desktop.
#
# GTK blocks on org.freedesktop.portal.Settings for 25 s when no service owns
# that name, so the port ships the method GTK actually calls.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_executable(serenity-portal portal.cpp)
target_link_libraries(serenity-portal PRIVATE PkgConfig::SYSTEMD)
