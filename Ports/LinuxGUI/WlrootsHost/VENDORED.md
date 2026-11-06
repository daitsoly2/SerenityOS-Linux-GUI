# Vendored wlroots（协议版）部署说明

> `serenity-wlroots-host` 只把 wlroots 当「Wayland 协议服务器」用（renderer=NULL，无
> backend/output/scene），每个 foreign toplevel 的 CPU 像素进一个普通 `GUI::Window` 由
> WindowServer 统一合成。本文件记录 vendored wlroots 的版本、构建与部署，方便重部署照做。

## 1. 版本与来源

- 固定上游 **0.20.2**，上游 commit `d783533489e1f75d6886c2ab5c5960090ef268f8`，**MIT** 许可。
- 源码树位置：仓库根 `/serenity/wlroots/`（host 绝对路径 `/mnt/user/serenity/wlroots/`）。
- 裁剪为「仅协议支持」：meson 关闭全部 backend（drm/x11/libinput）、renderer（gles2/vulkan）、
  allocator（gbm/udmabuf）、session、xwayland、color-management。只保留 `types/` `util/`
  `xcursor/` `protocol/` 对应的对象。
- 验证：`wlroots/prefix/lib/<triplet>/libwlroots-0.20.so` 中
  `wlr_drm_backend` / `wlr_gles2_renderer` 符号计数 = 0；`.pc` 里 `have_*` 全为 `false`。

## 2. 为什么独立放在仓库根、不进 WindowServer 树

- **固定版本**：避免宿主机 wlroots 升级导致 `serenity-wlroots-host` 行为漂移。
- **许可边界**：wlroots 自带 MIT LICENSE，独立放在 `/serenity/wlroots/` 不混入
  WindowServer 等 BSD 许可源码树。
- **架构**：`serenity-wlroots-host` 是独立进程（不链进 WindowServer），WS 始终是唯一的
  KMS 合成器（2026-11-06 已否决 `SERENITY_PRESENTER=wlroots`）。

## 3. 构建 vendored 库（仅改 wlroots 源码时才需重做）

```bash
cd /serenity/wlroots
meson setup build --prefix="$PWD/prefix" \
  -Dexamples=false -Dwerror=false \
  '-Dbackends=[]' '-Drenderers=[]' '-Dallocators=[]' \
  -Dsession=disabled -Dxwayland=disabled -Dcolor-management=disabled \
  -Dlibliftoff=disabled -Dxcb-errors=disabled
ninja -C build
meson install -C build
```

产物：`wlroots/prefix/lib/<triplet>/libwlroots-0.20.so` + `wlroots-0.20.pc`
（`lib` 下可能带多架构 triplet，如 `lib/x86_64-linux-gnu`、`lib/aarch64-linux-gnu`）。

> 注意：`meson` 的 feature 类型只接受 `enabled/disabled/auto`，不要写 `=false`；
> `-Dbackends=[]` 等空数组在 zsh 里要加引号，否则 `no matches found`。

## 4. 与构建系统的集成（CMakeLists.txt）

`Ports/LinuxGUI/WlrootsHost/CMakeLists.txt`：

- 用 `file(GLOB_RECURSE ... lib/**/pkgconfig/wlroots-0.20.pc)` 定位 vendored `.pc`，
  注入 `PKG_CONFIG_PATH` 优先 vendored（兼容任意 triplet），找不到才回退系统 wlroots-0.20。
- `wlroots_proto.c` 额外加 `-Wno-c23-extensions`：clang 默认 gnu23 把未命名 `void*` 形参
  当 C23 扩展，叠加 `-Werror` 直接编译失败。
- 二进制 rpath（部署目录在前）：
  `/serenity/lib/<triplet>:<源码树 vendored 目录>:$ORIGIN:$ORIGIN/../lib`
  → 运行时优先命中 `/serenity/lib/<triplet>`，不依赖源码树、不回退系统完整版 wlroots。

## 5. 部署到 VM / 目标机（重部署照此做）

1. **同步两样进目标机源码树**（VM 源码树在 `/home/serenity/serenity`，与 host 独立）：
   - `wlroots/prefix/` 整目录
   - `Ports/LinuxGUI/WlrootsHost/CMakeLists.txt`
   - rsync 前先 `mkdir -p` 目标父目录，`wlroots/prefix` 的父目录不存在会直接失败。
2. **装库到运行时目录**：
   ```bash
   sudo mkdir -p /serenity/lib/<triplet>
   sudo cp wlroots/prefix/lib/<triplet>/libwlroots-0.20.so /serenity/lib/<triplet>/
   sudo ldconfig
   ```
3. **目标机内重建**（CMakeLists 改了会自动重配）：
   ```bash
   cd /home/serenity/serenity
   cmake --build Build/lagom --target serenity-wlroots-host -j"$(nproc)"
   ```
4. **部署二进制**：覆盖正在运行的文件会 `Text file busy`——serenitid 看门狗会立刻重启
   抢占该文件。用 `cp` 到 `/tmp/*.new` 再 `mv -f` 原子替换，然后 `sudo kill` 旧进程，
   看门狗会用新二进制重启。
   - `pgrep -x` 对 >15 字符进程名失效，用 `pgrep -f 'bin/serenity-wlroots-host$'`。
5. **验证**：
   - `readelf -d /serenity/bin/serenity-wlroots-host` 看 RUNPATH 含 `/serenity/lib/<triplet>`；
   - 新进程 `/proc/$PID/maps` 实际 mmap `/serenity/lib/<triplet>/libwlroots-0.20.so`
     （**不是** `/usr/lib/...` 系统完整版）。

## 6. 已知遗留

- **aarch64 目标机**：需单独交叉编译 vendored `.so` 到 `lib/aarch64-linux-gnu/`；
  CMake glob 已兼容任意 triplet。
- `WSWlrBridge/`（被否决的内嵌 presenter 代码）未删，待用户要求。
- host 开发构建的二进制 rpath 仍含源码树路径（仅该二进制自身用，无害）；
  目标机运行时以 `/serenity/lib/<triplet>` 为准。
