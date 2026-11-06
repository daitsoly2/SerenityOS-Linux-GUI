# fcitx5 frontend addon that speaks to SerenityOS over a unix socket.
#
# Built separately from the Lagom tree: it needs fcitx5 headers (fcitx5-modules-dev
# / libfcitx5core-dev) and must match the fcitx5 version shipped in the image.
# We pin the version on purpose - this system does not chase upstream fcitx5.

cmake_minimum_required(VERSION 3.16)
project(serenityime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fcitx5::Core already carries the fcitx5-utils dependency.
find_package(Fcitx5Core REQUIRED)

add_library(serenityime MODULE serenityime.cpp)
target_link_libraries(serenityime PRIVATE Fcitx5::Core)

set_target_properties(serenityime PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "serenityime"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS serenityime LIBRARY DESTINATION lib/fcitx5)
install(FILES serenityime.conf DESTINATION share/fcitx5/addon)
