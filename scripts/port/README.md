# SerenityOS 桌面环境 → Debian 13（无桌面）移植

> 完整版说明见项目根目录 [`LINUX_GUI_PORT.md`](../../LINUX_GUI_PORT.md)。

把 SerenityOS 的 **WindowServer（合成器 / 窗口管理器）** 作为普通 Linux 进程跑起来，
并渲染到一个宿主窗口里。图形栈本身（`LibGfx`、`Compositor`、`WindowManager`、
`LibIPC`）**原样复用**，只替换了与 SerenityOS 内核打交道的那一层。

## 目录

```
Ports/LinuxGUI/
  LinuxScreenBackend.{h,cpp}   SDL2 呈现后端（内存帧缓冲 -> 纹理 -> 窗口）
  main_linux.cpp               Linux 入口（替代 pledge/unveil + /dev/gpu 初始化）
  CMakeLists.txt               以 Lagom 目标构建 WindowServer
  IPC/CMakeLists.txt           WindowServer/Clipboard/Notification 的共享 IPC 头
  Apps/CMakeLists.txt          自带 app（当前是 AnalogClock）的宿主目标
scripts/port/
  00-install-deps.sh           安装 clang/cmake/ninja/SDL2 等
  01-fetch-caches.sh           预置 Unicode/CLDR/TZDB 等离线数据（GitHub 走 gh-proxy）
  02-build.sh                  配置 + 构建 WindowServer 与 AnalogClock
  03-run.sh                    运行桌面（X11 / Wayland / KMSDRM / headless）
  04-run-app.sh                启动桌面 + 自带 AnalogClock
  05-install-prefix.sh         把运行期装进 /serenity（与 /usr/local 分离）
  winapp-launcher.sh           Windows 应用入口：DWProton → wine 自动选择
  wine-fonts.py                Wine 前缀中文字体：FontSubstitutes + FontLink
  build-ime.sh                 fcitx5 serenityime 插件的编译/安装
  ime-fake-engine.py           输入法协议假引擎（不装 fcitx 也能测）
  ui-strings.py                汉化查漏：源码候选串 vs [UI] 表
  04-qemu-debian13.sh          可选：QEMU 干净 Debian 13 测试床
```

## 构建与运行

```bash
cd scripts/port
sudo ./00-install-deps.sh
sudo ./01-fetch-caches.sh
./02-build.sh
sudo ./03-run.sh
```

跑自带 app：

```bash
sudo ./04-run-app.sh
```

它会先起 WindowServer，等 `/tmp/portal/window` 就绪后再启动 `AnalogClock`。

- 有 `DISPLAY` / `WAYLAND_DISPLAY` 时直接开窗。
- 纯 headless 服务器会自动用 `SDL_VIDEODRIVER=dummy`，并写一张
  `Build/screenshot.ppm`，用来确认合成器真的画出了桌面。
- 没有 X/Wayland、但有 DRM 设备时：
  ```bash
  sudo SERENITY_KMSDRM=1 ./03-run.sh
  ```

`03-run.sh` 会创建 `/res -> Build/lagom/Root/res` 软链接，并确保
`/etc/WindowServer.ini` 存在（Serenity 的主题/字体代码硬编码读取 `/res`）。
不需要时手动删除软链接即可。

## 改了哪些上游文件

| 文件 | 改动 |
|------|------|
| `Userland/Services/WindowServer/ScreenBackend.h` | 在非 SerenityOS 下本地定义 `FBRect` / `GraphicsHeadModeSetting` |
| `Userland/Services/WindowServer/Screen.cpp` | 屏蔽 `<sys/devices/gpu.h>`；`Mode::Virtual` 在宿主上改用 `LinuxScreenBackend`；`Mode::Device` 在宿主上直接返回失败 |
| `Userland/Services/WindowServer/EventLoop.cpp` | 宿主上不监听 `/dev/input` 与 `/tmp/system/devicemap`；改为自行 `listen` `/tmp/portal/{window,wm}`，输入由 SDL 事件泵注入 |
| `Userland/Services/WindowServer/KeymapSwitcher.cpp` | 宿主上没有 `/sys/kernel/keymap` 与 `/bin/keymap`，改为惰性桩，避免构造时 abort |
| `Userland/Libraries/LibKeyboard/CharacterMap.cpp` | 宿主上 `getkeymap`/`setkeymap` 返回 `ENOSYS`（调用方已有错误分支） |
| `Meta/CMake/lagom_options.cmake` | 新增 `ENABLE_LINUX_GUI_PORT` |
| `Meta/Lagom/CMakeLists.txt` | 构建 `LibKeyboard`；按开关加入 `Ports/LinuxGUI` |
| `Meta/download_file.py` | `mkdir(exist_ok=True)`，避免沙箱里 `rmtree` 失败导致构建中断 |

## GPU 加速

分两个层次：

1. **呈现层（已启用）**：`LinuxScreenBackend` 请求
   `SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC`，SDL 会自动选择
   OpenGL / Vulkan 后端，把 CPU 合成好的帧上传为纹理并 vsync 显示。
2. **合成层（未做，需改 `Compositor`）**：把每个窗口的 backing store
   （本身就是 `Gfx::Bitmap`）上传为 GL 纹理，用四边形 + 着色器在 GPU 上混合。
   仓库里已有 `LibAccelGfx`（OpenGL/EGL 的 `Painter` 与 `GlyphAtlas`）可复用；
   `Meta/CMake/accelerated_graphics.cmake` 在 Linux 上会自动探测 OpenGL/EGL。
   注意 Serenity 的 `ARGB32` 是预乘 alpha，GL 混合要用
   `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`。

## 当前状态 / 限制

- ✅ WindowServer（合成器、窗口管理、IPC、主题、字体）作为 Linux 进程运行。
- ✅ SDL2 呈现，支持 X11 / Wayland / KMSDRM / dummy。
- ✅ **完整 LibGUI**（117 个控件源文件）在宿主上编译，`ENABLE_LAGOM_GUI_APPS`
  打开后可用自带 app。
- ✅ 自带 **AnalogClock** 已接入并成功链接（依赖最少：`LibCore/LibGfx/LibGUI/LibMain`）。
  已验证：117 个 LibGUI 源文件 + WindowServer 24 个源文件在 Debian 13 / clang 21 上全部编译通过。
- ⏳ 其余 50 个自带 app 需要按依赖逐个加目标；多数只差
  `LibDesktop`/`LibConfig` 的运行时服务（LaunchServer/ConfigServer）。
- ⏳ 输入映射目前覆盖常用键；完整的 Serenity `KeyCode` 映射表可按需补全。

## 排错

| 现象 | 处理 |
|------|------|
| `Package 'sdl2' not found` / `Could not find SDL2` | `sudo apt-get install libsdl2-dev` |
| `Failed to open /res/themes/Default.ini` | 先跑 `03-run.sh`（它会建 `/res` 软链接并接入 `Base/res`） |
| `Unable to open cursor theme` | 同上；`Base/res/cursor-themes/Default` 必须存在 |
| `KeymapSwitcher` 构造时 abort | 确认 `KeymapSwitcher.cpp` 的宿主分支已生效（`#ifndef __serenity__`） |
| `WindowServer/WindowManagerClientEndpoint.h: file not found` | 干净构建的顺序竞态；`02-build.sh` 已改为先单独预生成 IPC 头。若直接 `ninja`，先跑那 8 个 `generate_*` 目标 |
| 链接时报 `libSDL2-2.0.so.0: not found` | 装了 `libsdl2-dev` 但缺运行时包，补 `libsdl2-2.0-0` |
| 其它编译错误 | 把 `02-build.sh` 的第一段报错发出来，按文件逐个修 |

第一次在干净环境构建会编译大量 Lagom 库（`LibGfx`/`LibIPC`/`LibUnicode` 等），
耗时取决于机器；之后增量构建很快。

## 回滚

```bash
git checkout -- \
  Userland/Services/WindowServer/ScreenBackend.h \
  Userland/Services/WindowServer/Screen.cpp \
  Userland/Services/WindowServer/EventLoop.cpp \
  Userland/Libraries/LibKeyboard/CharacterMap.cpp \
  Meta/CMake/lagom_options.cmake \
  Meta/Lagom/CMakeLists.txt \
  Meta/download_file.py
rm -rf Ports/LinuxGUI scripts/port
```
