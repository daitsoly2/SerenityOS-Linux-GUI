# serenity-wlroots-host
#
# 裁减版 wlroots 只提供 Wayland 协议服务（renderer=NULL，无 backend/output/scene），
# 每个 toplevel 的 CPU 像素进入一个普通 GUI::Window，由 WindowServer 统一合成。
#
# 与 WaylandHost 的区别：那边自己实现协议，这边用 wlroots 的协议实现。

find_package(PkgConfig REQUIRED)

# ── 优先使用仓库内 vendored 的「仅协议支持」wlroots ───────────────────────────
# 位置：<repo-root>/wlroots/prefix （由 wlroots/ 目录下的 meson 构建并 install 得到）。
# 这是固定到上游 commit d783533（0.20.2）、关闭了所有 backend/renderer/session/
# xwayland 的裁剪版，只保留 WS 需要的协议对象（compositor/shm/subcompositor/
# seat/data_device/xdg_shell/xdg_decoration）。
# 这样做有两个目的：
#   1) 固定版本，避免宿主机 wlroots 升级导致 serenity-wlroots-host 行为漂移；
#   2) wlroots 源码独立放在仓库根的 wlroots/ 目录（自带 MIT LICENSE），不混入
#      WindowServer 等 BSD 许可的源码树，保持许可边界清晰。
# 若 wlroots/prefix 不存在，则回退到系统的 wlroots-0.20（需宿主机已安装）。
set(WLROOTS_VENDOR_PREFIX "${SERENITY_PROJECT_ROOT}/wlroots/prefix")
if(EXISTS "${WLROOTS_VENDOR_PREFIX}/lib")
    # lib 目录可能带多架构 triplet（lib/x86_64-linux-gnu、lib/aarch64-linux-gnu …），
    # 用 ** 深度通配兼容不同目标机，定位 wlroots-0.20.pc 所在目录。
    file(GLOB_RECURSE WLROOTS_VENDOR_PC "${WLROOTS_VENDOR_PREFIX}/lib/**/pkgconfig/wlroots-0.20.pc")
    if(WLROOTS_VENDOR_PC)
        set(WLROOTS_VENDOR_PCDIRS "")
        foreach(_pc ${WLROOTS_VENDOR_PC})
            get_filename_component(_pcdir "${_pc}" DIRECTORY)
            list(APPEND WLROOTS_VENDOR_PCDIRS "${_pcdir}")
        endforeach()
        list(REMOVE_DUPLICATES WLROOTS_VENDOR_PCDIRS)
        list(APPEND WLROOTS_VENDOR_PCDIRS "$ENV{PKG_CONFIG_PATH}")
        list(REMOVE_DUPLICATES WLROOTS_VENDOR_PCDIRS)
        string(JOIN ":" WLROOTS_VENDOR_PCPATH ${WLROOTS_VENDOR_PCDIRS})
        set(ENV{PKG_CONFIG_PATH} "${WLROOTS_VENDOR_PCPATH}")
        message(STATUS "WlrootsHost: using vendored wlroots from ${WLROOTS_VENDOR_PREFIX}")
        # 推导出 vendored 的 lib 目录（pkgconfig 的上一级），用于给本 target 加
        # BUILD_RPATH，使运行时加载器优先于系统的完整版 wlroots 找到我们的裁剪版，
        # 从而兑现「固定版本、不随宿主机 wlroots 升级漂移」的承诺。
        set(WLROOTS_VENDOR_LIBDIRS "")
        foreach(_pc ${WLROOTS_VENDOR_PC})
            get_filename_component(_pcdir "${_pc}" DIRECTORY)
            get_filename_component(_libdir "${_pcdir}" DIRECTORY)
            list(APPEND WLROOTS_VENDOR_LIBDIRS "${_libdir}")
        endforeach()
        list(REMOVE_DUPLICATES WLROOTS_VENDOR_LIBDIRS)
    endif()
endif()

pkg_check_modules(WLROOTS QUIET IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
# wlroots 的 .pc 把 xkbcommon 放在 Requires.private，shared 链接不传递；
# 我们直接调 xkb_context_new / xkb_keymap_new_from_names，必须显式链接。
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
# X11/Wine 兼容：不重写 XWM，直接复用旧桥的两个编译单元（只在其头文件里
# 加了一个 pid() getter，用于区分 per-client 的 Wayland object id）。
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCOMPOSITE REQUIRED IMPORTED_TARGET xcomposite)

# wlroots 的 .pc 把一部分头（xcb/*）放在 Requires.private 里，shared 链接时
# 不会传播到编译命令中；显式补上 prefix 的总 include 目录。
pkg_get_variable(WLROOTS_PREFIX wlroots-0.20 prefix)

if (WLROOTS_FOUND)
add_executable(serenity-wlroots-host
    main.cpp
    wlroots_proto.c
    # 复用已有且验证过的 X11 侧实现（XWM + rootless Xwayland），不在新桥里
    # 复制一份窗口管理逻辑。这两个文件原在已删除的 WaylandHost 目录。
    ../X11Compat/xwm.cpp
    ../X11Compat/xwayland_bridge.cpp
)

target_include_directories(serenity-wlroots-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WLROOTS_PREFIX}/include
    # LibGUI 的 Desktop.h 会拉进 TaskbarWindow.h，后者引用任务栏 IME 指示器头。
    ${SERENITY_PROJECT_ROOT}/Ports/LinuxGUI/IMEFrontend
)

set_source_files_properties(wlroots_proto.c PROPERTIES
    COMPILE_DEFINITIONS WLR_USE_UNSTABLE
    # clang 在新版本默认 -std=gnu23，把「函数定义里省略参数名（void* 无命名）」
    # 当成 C23 extension 并发 -Wc23-extensions；配合 -Werror 会直接编译失败。
    # 这是旧式 C 回调签名（wl_listener::notify = fn(listener, data)）的惯用法，
    # 仅对该文件局部关掉这条告警，不动源码语义。
    COMPILE_OPTIONS "-Wno-cast-qual;-Wno-c23-extensions"
)

# 旧桥 xwm.cpp 里的 window_supports_protocol() 没有前置声明；那个 target 用
# 自己的告警配置编译通过，我们复用它的源码但不改它，只在本 target 对复用
# 单元关掉这一条告警。
set_source_files_properties(
    ../X11Compat/xwm.cpp
    ../X11Compat/xwayland_bridge.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-declarations"
)

target_link_libraries(serenity-wlroots-host PRIVATE
    LibCore
    LibGfx
    LibGUI
    LibMain
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::PIXMAN
    PkgConfig::LIBDRM
    PkgConfig::XKBCOMMON
    PkgConfig::X11
    PkgConfig::XCOMPOSITE
)

set_target_properties(serenity-wlroots-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 运行时优先加载 vendored 的裁剪版 wlroots。Lagom 的全局链接 flag 已经写入了
# -rpath=$ORIGIN:$ORIGIN/../lib（只覆盖构建树内的 lagom 动态库），不会包含我们的
# vendored 目录，导致运行时落到系统的完整版 wlroots。这里对单个 target 追加 -rpath。
# rpath 含两条，部署目录放前面优先命中：
#   1) /serenity/lib/<triplet> —— 与目标机（如 aarch64 设备）实际安装位置一致，
#      运行时不再依赖源码树存在，也不会回退到系统完整版 wlroots；
#   2) 源码树里的 vendored 目录（${WLROOTS_VENDOR_LIBDIRS}）—— 开发/验证时源码树
#      存在即可直接加载，无需先 install 到系统 lib。
# triplet 从 vendored lib 目录名（lib/ 之后那段）推导，x86_64 / aarch64 通吃。
if(WLROOTS_VENDOR_LIBDIRS)
    list(GET WLROOTS_VENDOR_LIBDIRS 0 _src_libdir)
    string(REGEX REPLACE ".*/lib/([^/]+)/?$" "\\1" _triplet "${_src_libdir}")
    set(_deploy_libdir "/serenity/lib/${_triplet}")
    set(_wlr_rpath "${_deploy_libdir}:${_src_libdir}")
    target_link_options(serenity-wlroots-host PRIVATE
        "-Wl,-rpath,${_wlr_rpath}"
    )
    message(STATUS "WlrootsHost: rpath -> ${_wlr_rpath}")
endif()
endif()
