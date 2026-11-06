# ws-wlr-bridge —— Serenity WindowServer (ws_*) ↔ labwc/wlroots (wlr_*) 桥接层
#
# 独立可配置：cmake -S . -B build && cmake --build build && ctest --test-dir build
# 刻意不挂进上层 Ports/LinuxGUI/CMakeLists.txt：现阶段只改桥接层（规则 5），
# 不碰 WindowServer 核心，也不需要 wlroots 或 Serenity 的构建环境。
#
# 目录隔离是规则 4 的硬性保证：
#   include/ = PUBLIC，只有 POD 契约 + 两侧函数表 + 唯一门面 Bridge.h
#   src/     = PRIVATE，等价别名、适配 wrapper、独立记账状态全在这里

cmake_minimum_required(VERSION 3.20)
project(ws-wlr-bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WSWLR_BRIDGE_BUILD_TESTS "构建桥接层自检" ON)
option(WSWLR_BIND_REAL_SYMBOLS "把真实 ws_*/wlr_* 符号填进函数表（生产接线）" OFF)
option(WSWLR_BRIDGE_BUILD_HOST "构建先期可跑的 labwc foreign-toplevel host" ON)
option(WSWLR_BRIDGE_BUILD_COMPOSITOR "构建集成 wlroots 的桥内合成器" ON)

# 部署到 VM 时先安装到一个独立子前缀，不覆盖桌面会话的任何现有文件：
#   cmake --install <build> --prefix /serenity/opt/ws-wlr-bridge
# 装出来的是「可独立验证的桥接层」，尚不挂进 serenity-desktop 启动链。
include(GNUInstallDirs)

add_library(ws-wlr-bridge STATIC
    src/Bridge.cpp
    src/AdaptationWrappers.cpp
    src/Classification.cpp
    src/RealAbi.cpp
    src/detail/IndependentState.cpp
)

target_include_directories(ws-wlr-bridge
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WSWLR_BIND_REAL_SYMBOLS)
    target_compile_definitions(ws-wlr-bridge PRIVATE WSWLR_BIND_REAL_SYMBOLS=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ws-wlr-bridge PRIVATE -Wall -Wextra)
endif()

if(WSWLR_BRIDGE_BUILD_TESTS)
    enable_testing()

    add_executable(ws-wlr-bridge-selftest tests/selftest.cpp)
    target_include_directories(ws-wlr-bridge-selftest
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(ws-wlr-bridge-selftest PRIVATE
        WSWLR_PUBLIC_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(ws-wlr-bridge-selftest PRIVATE ws-wlr-bridge)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ws-wlr-bridge-selftest PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME ws-wlr-bridge-selftest COMMAND ws-wlr-bridge-selftest)
endif()

install(TARGETS ws-wlr-bridge
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/WsWlrBridge
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
if(WSWLR_BRIDGE_BUILD_TESTS)
    install(TARGETS ws-wlr-bridge-selftest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# ── 先期可跑的载体：一个普通 Wayland 客户端，接 labwc 的 foreign-toplevel ──
# 只在找到 wayland-client + wayland-scanner 时构建，不影响纯库/自检的独立编译。
# 这不是合成器，也不链接 wlroots：labwc 才是协议栈，本进程只做桥的翻译。
if(WSWLR_BRIDGE_BUILD_HOST)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    endif()
    find_program(WAYLAND_SCANNER wayland-scanner)

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER)
        set(HOST_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
        set(HOST_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/host-generated)
        file(MAKE_DIRECTORY ${HOST_GEN_DIR})
        set(HOST_GEN_H ${HOST_GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)
        set(HOST_GEN_C ${HOST_GEN_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c)
        add_custom_command(
            OUTPUT ${HOST_GEN_H} ${HOST_GEN_C}
            COMMAND ${WAYLAND_SCANNER} client-header ${HOST_XML} ${HOST_GEN_H}
            COMMAND ${WAYLAND_SCANNER} private-code ${HOST_XML} ${HOST_GEN_C}
            DEPENDS ${HOST_XML}
            COMMENT "wayland-scanner: foreign-toplevel-management (host)"
        )
        add_executable(serenity-ws-wlr-bridge-host
            host/main.cpp ${HOST_GEN_C} ${HOST_GEN_H}
        )
        target_include_directories(serenity-ws-wlr-bridge-host PRIVATE ${HOST_GEN_DIR})
        target_link_libraries(serenity-ws-wlr-bridge-host
            PRIVATE ws-wlr-bridge PkgConfig::WAYLAND_CLIENT
        )
        # wayland-scanner 的 inline wrapper 里有 void(**)() 强转，会触发 -Wcast-qual。
        target_compile_options(serenity-ws-wlr-bridge-host PRIVATE -Wno-cast-qual)
        install(TARGETS serenity-ws-wlr-bridge-host
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    else()
        message(STATUS "ws-wlr-bridge: host skipped (need wayland-client + wayland-scanner)")
    endif()
endif()

# ── 嵌入式 wlroots 合成器：作为静态库链进 serenity-window-server ──
# WindowServer 进程内嵌入 wlroots，拥有真实屏幕（KMS/DRM），并把原生桌面与
# 外来 X11/Wayland 客户端都合成进同一个 scene 图（「WS = 合成器」路线）。
# 只链系统 wlroots 库，不 vendor 源码。找不到 dev 包时自动跳过。
# 版本口子：优先 0.20（sid），退回 0.19 / 0.18 只改这里的 pkg-config 名，
# 但 scene/output API 需按对应版本适配，不能仅靠开关切换。
if(WSWLR_BRIDGE_BUILD_COMPOSITOR)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WLROOTS QUIET IMPORTED_TARGET wlroots-0.20)
    endif()
    if(WLROOTS_FOUND)
        # wlroots 的 .pc 把 wayland-server 列在 Requires.private，CMake 默认不带，
        # 但 wlroots_backend.c 直接调 wl_event_loop_add_fd / wl_display_*，要显式链。
        pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
        # wlroots 交互层必须用 C 编译器（0.20 头含 C99 `[static N]` 数组参数），
        # C++ 侧只通过 wlroots_backend.h 的纯 C ABI 调它。
        add_library(ws-wlr-compositor STATIC
            host/wlroots_backend.c
        )
        target_include_directories(ws-wlr-compositor
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/host
        )
        target_link_libraries(ws-wlr-compositor
            PUBLIC PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER
        )
        # wlroots 头默认拒绝被 include，除非显式声明接受 unstable API；
        # wlroots 0.20 的 scene/output 仍是 unstable，必须定义。
        target_compile_definitions(ws-wlr-compositor PUBLIC WLR_USE_UNSTABLE)
        target_compile_options(ws-wlr-compositor PRIVATE -Wno-cast-qual)
        install(TARGETS ws-wlr-compositor
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    else()
        message(STATUS "ws-wlr-bridge: compositor skipped (need wlroots-0.20 dev)")
    endif()
endif()
