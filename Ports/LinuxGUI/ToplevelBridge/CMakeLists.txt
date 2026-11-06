# serenity-toplevel-bridge: publish the compositor's toplevel list for the shell.
#
# Bindings the shell cannot make itself: in the Xwayland session the desktop is
# one X client, so only a Wayland client can see labwc's foreign toplevels.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

add_custom_command(
    OUTPUT ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
           ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${PROTOCOL_XML}
            ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${PROTOCOL_XML}
            ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    DEPENDS ${PROTOCOL_XML}
    COMMENT "wayland-scanner: foreign-toplevel-management"
)

add_executable(serenity-toplevel-bridge
    toplevel_bridge.cpp
    ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    ${GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
)

target_include_directories(serenity-toplevel-bridge PRIVATE ${GEN_DIR})
target_link_libraries(serenity-toplevel-bridge PRIVATE PkgConfig::WAYLAND_CLIENT)

# wayland-scanner casts the listener structs to void(**)() inside its inline
# wrappers; that trips -Wcast-qual, which the project builds with.
target_compile_options(serenity-toplevel-bridge PRIVATE -Wno-cast-qual)
