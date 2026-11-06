# Wine 原生 Wayland 驱动 × WaylandHost 协议审计

> 日期：2026-11-03  
> 状态：**✅ P0 已完成并部署验证**（第一轮仅审计；第二轮实现 viewporter + 双标题栏修复，VM 实测通过）  
> 结论：真 `wp_viewporter` 落地后，Wine 11.0 原生 Wayland 驱动能正常启动并渲染窗口；`wl_output` 尺寸、input region、pointer constraints 等仍是缺口（见 §6 / §7）。  
> 相关文档：`STATUS.md`、`LINUX_GUI_PORT.md`、`HANDOFF_WINAPPS.md`

---

## 一页速览

| 项目 | 结论 |
|---|---|
| Wine 当前实际后端（不强制时） | ❌ 仍是 Xwayland/X11（默认顺序 `mac,x11,wayland`） |
| 强制 Wayland 的硬阻塞 `wp_viewporter` | ✅ 已实现真协议并部署验证 |
| 旧假 XML viewporter | ✅ 已删除（wire 不兼容，见 §5 历史记录） |
| viewporter 语义 | ✅ set_source/set_destination 真 crop/scale + 协议错误（§5.5） |
| 双标题栏 | ✅ 已修：Wine 窗口 frameless + 实现 move/resize（§5.6） |
| `pointer_constraints_v1` / `relative_pointer_v1` | ⚠️ Wine optional；缺了影响鼠标锁定/相对移动（Wine 启动时打 ERR） |
| `keyboard_shortcuts_inhibit_v1` | ℹ️ upstream Wine 11.0 不用；DWProton/Proton 才需要 |
| `wl_output` 1600x1200 硬编码 | ⚠️ **未修**，下一个该修（§6.1） |
| 未动 = input region / subsurface / xdg 配置 / dmabuf | ⚠️ 缺口仍在（§6.2-6.6） |
| 未提交状态 | 改动在工作区，未 commit |

---

## 1. 当前 Wine 是怎么跑的

### 1.1 环境变量

相关代码：

- `Ports/LinuxGUI/WinAppLauncher/main.cpp:440`
  ```cpp
  setenv_if_unset("DISPLAY", ":100");
  ```
- `Ports/LinuxGUI/SessionManager/serenity_manager.cpp:443-444`
  ```cpp
  setenv("WAYLAND_DISPLAY", "wayland-serenity-0", 1);
  setenv("SERENITY_APP_DISPLAY", ":100", 1);
  ```
- `Ports/LinuxGUI/SessionManager/serenity_manager.cpp:473`
  ```cpp
  setenv("DISPLAY", ":100", 0);
  ```
- `Ports/LinuxGUI/Shell/main.cpp:45-51` 同时导出 `DISPLAY` 和 `WAYLAND_DISPLAY`。

Wine 启动时通常同时看到：

```text
DISPLAY=:100
WAYLAND_DISPLAY=wayland-serenity-0
```

### 1.2 Wine 11.0 的驱动选择

Wine 11.0 源码：

- `programs/explorer/desktop.c:43`
  ```c
  static const WCHAR default_driver[] = L"mac,x11,wayland";
  ```
- `desktop.c:996-1063` 的 `load_graphics_driver()` 按顺序尝试，前一个加载成功就不再尝试后面的。
- 覆盖项：
  ```text
  HKCU\Software\Wine\Drivers\Graphics
  ```

所以当前实际路径是：

```text
DISPLAY=:100 可用
  -> winex11.drv 加载成功
  -> 不再尝试 winewayland.drv
```

**`WINE_WAYLAND_DRIVER=1` 在 Wine 11.0 源码里搜不到，实测也没有效果。**  
`wayland_host.cpp:1530` 的注释写这个环境变量是错误的，下次改代码时顺手修掉。

### 1.3 如何强制 Wine 用 Wayland

在测试 prefix 里：

```bash
WINEPREFIX=$HOME/wine-wl-test \
  wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ /d wayland /f
```

启动时最好去掉 `DISPLAY`，避免任何 X11 回退干扰：

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-serenity-0
unset DISPLAY
export WINEPREFIX=$HOME/wine-wl-test
export WINEDEBUG=+waylanddrv
wine notepad
```

当前 VM 的 `~/wine-wl-test` 已经是 `Graphics=wayland`，可以直接复现。

---

## 2. WaylandHost 当前实际暴露的 globals

### 2.1 部署中的二进制

部署的 `/serenity/bin/serenity-wayland-host` 是 **12:37** 构建的。实测 Wine 看到的 registry：

| Global | 版本 | 状态 |
|---|---:|---|
| `wl_compositor` | 4 | 有 |
| `wl_shm` | 1 | 有 |
| `wl_seat` | 7 | 有 |
| `wl_output` | 3 | 有 |
| `wl_subcompositor` | 1 | 有 |
| `xdg_wm_base` | 5 | 有 |
| `zxdg_decoration_manager_v1` | 1 | 有 |
| `zwp_text_input_manager_v3` | 1 | 有 |
| `org_kde_kwin_server_decoration_manager` | 1 | 有 |
| `wl_data_device_manager` | 3 | 有 |
| **`wp_viewporter`** | — | **没有** |

### 2.2 当前工作区源码

当前源码在 `wayland_host.cpp:3480-3498` 创建 globals，比部署二进制多了一个：

```cpp
wl_global_create(g_server.display, &wp_viewporter_interface, 1, nullptr, bind_viewporter);
```

但它使用的是：

```text
Ports/LinuxGUI/WaylandHost/protocols/ext-viewporter-v1.xml
```

这是**手写的假协议**，见 `CMakeLists.txt:24-27` 和 `74-80`。

另外，`Build/lagom/bin/serenity-wayland-host`（14:50 构建）已经带上了假 viewporter。**不要直接部署这个中间构建。**

---

## 3. Wine 11.0 的协议依赖矩阵

### 3.1 硬性 required

Wine 11.0 `wayland.c:288-313` 的检查顺序：

| Global | 要求 | WaylandHost | 结果 |
|---|---|---|---|
| `wl_compositor` | 必需 | v4 | OK，Wine 绑定 v4 |
| `xdg_wm_base` | 必需 | v5 | OK，Wine 绑定 v2 |
| `wl_shm` | 必需 | v1 | OK |
| `wl_subcompositor` | 必需 | v1 | global 有，但语义不完整 |
| **`wp_viewporter`** | **必需** | **部署缺失；源码是假实现** | **直接启动失败** |

缺任何一个，`wayland_process_init()` 返回 `FALSE`，`DllMain` 失败，最后就是：

```text
err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be loaded.
```

### 3.2 optional

Wine 11.0 `wayland.c:315-334`：

| Global | 要求 | WaylandHost | 缺失影响 |
|---|---|---|---|
| `zwp_pointer_constraints_v1` | optional | 无 | 鼠标锁定/confine 不工作 |
| `zwp_relative_pointer_manager_v1` | optional | 无 | 相对移动不工作 |
| `zwp_text_input_manager_v3` | optional | 有 v1 | OK |
| `wl_data_device_manager` | optional fallback | 有 v3 | 基础剪贴板可用 |
| `zwlr_data_control_manager_v1` | optional | 无 | 剪贴板功能受限，Wine 打 `ERR` |
| `xdg_toplevel_icon_manager_v1` | optional | 无 | 没有窗口图标，Wine 打 `ERR` |
| `wp_cursor_shape_manager_v1` | optional | 无 | 回退到 SHM cursor surface |
| `zxdg_output_manager_v1` | optional | 无 | 回退到 `wl_output` |

注意：`wl_seat` 和 `wl_output` 不在 Wine 的 required 检查里，但没有它们输入/显示信息不完整。

### 3.3 新版 Wine 额外检查

Wine 11.19/master 还多了：

- `wp_fractional_scale_manager_v1`（optional，缺失打 fractional scaling broken）
- `wp_color_manager_v1`（optional，缺失打 color management broken）

不影响 Wine 11.0 启动。

### 3.4 DWProton 的差异

DWProton 的 `/serenity/opt/dwproton/files/lib/wine/x86_64-unix/winewayland.so` 里有：

```text
WAYLANDDRV_SHORTCUT_INHIBIT
zwp_keyboard_shortcuts_inhibit_manager_v1
zwp_keyboard_shortcuts_inhibitor_v1
```

以及：

```text
ext_data_control_manager_v1
xdg_activation_v1
xdg_toplevel_tag_manager_v1
wp_content_type_manager_v1
wp_fractional_scale_manager_v1
```

它的 required 五项和 upstream Wine 一样，仍然包含 `wp_viewporter`。  
`zwp_keyboard_shortcuts_inhibit_manager_v1` 从字符串看没有“缺失即失败”的报错，倾向于是 optional；但游戏按键抑制会受影响，需要实测确认。

---

## 4. 实测失败链路

测试条件：

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-serenity-0
export DISPLAY=:100
export WINEPREFIX=$HOME/wine-wl-test
export WINEDEBUG=+waylanddrv
wine notepad
```

关键日志：

```text
trace:waylanddrv:registry_handle_global interface=wl_compositor version=4 id=1
trace:waylanddrv:registry_handle_global interface=wl_shm version=1 id=2
trace:waylanddrv:registry_handle_global interface=wl_seat version=7 id=3
trace:waylanddrv:registry_handle_global interface=wl_output version=3 id=4
trace:waylanddrv:registry_handle_global interface=wl_subcompositor version=1 id=5
trace:waylanddrv:registry_handle_global interface=xdg_wm_base version=5 id=6
...
trace:waylanddrv:wayland_output_done name=WaylandOutput0 logical=0,0+1600x1200
trace:waylanddrv:wayland_output_done mode 1600x1200 @ 60000 *
err:waylanddrv:wayland_process_init Wayland compositor doesn't support wp_viewporter
err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be loaded.
err:winediag:nodrv_CreateWindow L"The explorer process failed to start."
```

退出码：

```text
EXIT=1
```

因为 required 检查在 optional 检查之前返回，所以这次日志里看不到 pointer constraints / relative pointer / data control / icon 的 optional 报错。补上 viewporter 之后才会看到它们。

---

## 5. 当前假 `wp_viewporter` 为什么不能用

### 5.1 协议对比

真实 XML：

```text
/usr/share/wayland-protocols/stable/viewporter/viewporter.xml
```

真实协议：

```text
wp_viewporter:
  destroy         opcode 0
  get_viewport    opcode 1

wp_viewport:
  destroy          opcode 0
  set_source       opcode 1
  set_destination  opcode 2
  # 没有 event
```

当前假 XML：

```text
wp_viewporter:
  destroy         opcode 0
  get_viewport    opcode 1
  get_dl          opcode 2        <- 标准里没有

wp_viewport:
  destroy         opcode 0
  # 没有 set_source / set_destination
  destination     event opcode 0  <- 标准里没有

wp_viewport_dl:                   <- 标准里没有
```

### 5.2 Wine 实际会调用什么

Wine 11.0 `wayland_surface.c`：

- 创建每个 surface 时调用 `wp_viewporter_get_viewport()`：
  - 普通 window surface：`wayland_surface.c:168-169`
  - client subsurface：`wayland_surface.c:1085-1088`
- 更新内容时调用 `wp_viewport_set_source()`：
  - `wayland_surface.c:448-450`
- 配置尺寸时调用 `wp_viewport_set_destination()`：
  - `wayland_surface.c:578-580`
  - `wayland_surface.c:616-618`

所以 Wine 一创建 surface 就会发 `set_source`，一配置窗口就会发 `set_destination`。

### 5.3 后果

按 libwayland 的 wire 规则：

- 服务器生成的 `wp_viewport_interface` 只有 `destroy` 一个 request。
- Wine 发 `set_source`（opcode 1）时，服务器看到的是 **invalid method**，会发 protocol error 并断开客户端。
- 假实现还在 `get_viewport` 里立刻发一个 `destination` event；Wine 没有给 `wp_viewport` 注册 listener，libwayland 客户端会报 `listener function for opcode 0 of wp_viewport is NULL`，直接 abort。

所以：

> **假 viewporter 比“没有 viewporter”更糟。**  
> 没有时是干净的启动失败；有了假实现是协议错误/断连，调试起来更像“Wine 随机崩”。

### 5.4 正确做法

不要手写 XML。直接使用系统的：

```text
/usr/share/wayland-protocols/stable/viewporter/viewporter.xml
```

CMake 里生成：

```text
viewporter-server-protocol.h
viewporter-protocol.c
```

然后：

- 删除 `protocols/ext-viewporter-v1.xml`
- 删除 `wp_viewport_dl` / `get_dl` / `destination` event
- 实现真正的 `set_source` / `set_destination`
- `get_viewport` 必须要求非空 `wl_surface`，并对同一个 surface 重复创建 viewport 报 `viewport_exists`
- viewport 状态是 double-buffered，在 `wl_surface.commit` 时应用

最小实现建议：

- 在 `Surface` 里存：
  - `Gfx::IntRect viewport_source`（buffer 坐标，空表示未设置）
  - `Gfx::IntSize viewport_destination`（0 表示未设置）
- `set_source` 处理 `-1,-1,-1,-1` 表示 unset，其余校验正数。
- `set_destination` 处理 `-1,-1` 表示 unset，其余校验正数。
- `surface_commit` 里：
  - 用 `viewport_source` 裁剪 bitmap
  - 用 `viewport_destination` 决定窗口/内容尺寸
  - 和现有 `window_geometry` 做交集，避免 CSD 阴影区域出错
- `ensure_window` 不再只用 buffer 的 `width/height`，而是优先用 destination size。

这样 Wine 的 `set_source` 是“裁剪 buffer 到窗口区域”，`set_destination` 是“把 surface 设成客户区大小”，才是真正可用的 pass-through。

### 5.5 ✅ 已实现：真正的 viewporter（11-03 第二轮）

实际落地的实现（与上面建议略有出入，以代码为准）：

- `Surface` 存的是 `ViewportState { ViewportSource source; Gfx::IntSize destination; }` 三份：`pending_viewport` / `current_viewport`，外加 `pending_viewport_dirty`。source 保留 **wl_fixed_t 原值**，这样才能在 “source 非整数且 destination 未设” 时报 `bad_size`。
- 协议错误全部实现：
  - `get_viewport` 已有 viewport → `WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS`
  - `set_source` 非全 -1 但 x/y < 0 或 w/h <= 0 → `bad_value`（全 -1 表示 unset）
  - `set_destination` 非全 -1 但任一 <= 0 → `bad_value`（全 -1 表示 unset）
  - surface 销毁后再发请求 → `no_surface`（surface 析构回调把 viewport 的 user_data 置空）
  - viewport destroy → 下次 commit 清掉 crop/scale 状态
  - commit 时：source 非整数且无 destination → `bad_size`；source 超出 buffer → `out_of_buffer`
- `surface_commit` 重构：viewport 只改状态但没新 buffer 时也会重渲染（Wine 常只 `set_destination` 不 `attach`）；内容几何由 `surface_content_from_buffer()` 统一算：viewport source ∩ buffer → 再把 xdg `window_geometry`（surface 坐标）按 crop/scale 反变换映射回 buffer 坐标裁剪，窗口尺寸用 destination（无 destination 用 source 尺寸，都没有用 buffer 尺寸）。
- 生命周期：viewport 资源有独立 destroy 回调（清 Surface 回指 + 置 dirty）；Surface 销毁时把 viewport user_data 置空而不是删掉对方资源。

### 5.6 ✅ 双标题栏：根因与修复（11-03 第二轮，用户实测反馈）

现象：notepad 窗口上叠着两个标题栏（Serenity 框 + Windows 风格标题栏）。

根因链（查 Wine 11.0 源码确认）：

1. Wine 的 Wayland 驱动**不实现 xdg-decoration**，也没有 `GetWindowStyleMasks`（win32u 对 Wayland 走 null driver，返回 FALSE）
2. 于是 win32u 自己画 Windows 非客户区（标题栏/边框/按钮），直接画进 toplevel 缓冲区
3. WaylandHost 又给所有 toplevel 套 Serenity 框 → 双标题栏
4. **`HKCU\Software\Wine\X11 Driver\Decorated=N` 实测无效**（那个设置只影响 win32u 的 visible rect，不控制是否画非客户区），不要浪费时间试

修复（WaylandHost 侧）：

- `is_wine_app_id()`：`xdg_toplevel.app_id` 以 `.exe` 结尾（Wine 惯例，如 `notepad.exe`）→ 判定为 Wine 原生 toplevel
- Wine 原生 toplevel（非 Xwayland、非 subsurface）`set_frameless(true)`，只留 Wine 画的 chrome
- **配套实现了 `xdg_toplevel.move` / `xdg_toplevel.resize`**：原来这俩是 no-op，frameless 之后 Wine 点自己标题栏要靠它挪窗
  - `begin_interactive_drag()` 记录按下时的指针屏幕坐标（`last_pointer_screen_position`，由 `hook_input_forward` 在(mouse down/move)时记录）和窗口原点
  - `surface_apply_drag()` 在 mouse move 时按 delta 移动/缩放；resize 用 `XDG_TOPLEVEL_RESIZE_EDGE_*` 位掩码，最小 24x24，且对原生客户端每帧发 `xdg_toplevel.configure` 让它重排 buffer
  - mouse up 时 `end_interactive_drag()`，release 照常转发给客户端
- 注意：`Decorated` 那个注册表项已在测试 prefix `~/wine-wl-test` 里设过 `N`，无害但无用，不用清理


---

## 6. 其它会影响“能用”的语义缺口

这些不是启动阻塞项，但补完 viewporter 后很可能马上遇到。

### 6.1 `wl_output` 尺寸错误

`wayland_host.cpp:79-80`：

```cpp
constexpr uint32_t MAX_OUTPUT_WIDTH = 1600;
constexpr uint32_t MAX_OUTPUT_HEIGHT = 1200;
```

`bind_output()` 在 `2448-2459` 固定发送：

```text
1600x1200 @ 60Hz
物理尺寸 300x200mm
```

实测：

- Wine 看到 `logical=0,0+1600x1200`
- Xwayland `xrandr` 也是 `1600x1200`
- 但 VM 的 Serenity 桌面实际是 `1280x800`  
  `/tmp/serenity-session.log` 里有：
  ```text
  WindowServer: probe_display_size -> 1280x800
  update_virtual_and_physical_rects for screen #0: [0,0 1280x800]
  ```

结果是 Xwayland/Wine 的屏幕比真实桌面大，窗口可能被裁切或跑到桌面外，DPI 也不对。

建议：

- 用 `GUI::Desktop::the().rects()[GUI::Desktop::the().main_screen_index()]` 取实际尺寸。
- 物理尺寸不要硬编码 300x200mm；未知时用 0,0 或按 DPI 计算。
- 多显示器时每个 screen 一个 `wl_output`，并配合 `zxdg_output_manager_v1` 发 logical position/size。

### 6.2 `wl_region` / input region / opaque region 都是 no-op

`wayland_host.cpp:866-867`：

```cpp
void surface_set_opaque_region(...) { }
void surface_set_input_region(...) { }
```

`wayland_host.cpp:977-978`：

```cpp
void region_add(...) { }
void region_subtract(...) { }
```

Wine 在 `wayland_surface.c:1080-1083` 给 client subsurface 设置了一个 **empty input region**，目的是让父 surface 处理所有指针事件。WaylandHost 不保存 region，也不执行 input region 语义，所以：

- Wine 的“空输入区域”不生效
- 指针 enter/focus 可能落到 client subsurface 而不是 toplevel
- 后续 pointer constraints、菜单、拖拽可能出问题

建议至少实现：

- `wl_region` 存矩形列表
- `set_input_region` 保存并用于命中测试
- `set_opaque_region` 保存用于合成（优先级较低）

### 6.3 `wl_subsurface` 不是真正的 sub-surface

当前实现把每个 subsurface 变成一个独立的无边框 Serenity 窗口，见 `wayland_host.cpp:1405-1408`：

```cpp
void subsurface_place_above(...) { }
void subsurface_place_below(...) { }
void subsurface_set_sync(...) { }
void subsurface_desync(...) { }
```

Wine 的 client surface 就是通过 `wl_subcompositor_get_subsurface()` 挂到 toplevel 上的，见 `wayland_surface.c:1119-1137`，而且会调用 `wl_subsurface_place_above()`。

当前实现的问题：

- 没有真正的裁剪，子窗口可以超出父窗口
- `place_above/below` 无效，z-order 只靠创建顺序
- `sync/desync` 无效，可能闪烁
- 父窗口移动时只是重新 `move_to`，没有 resize/clip

这不一定会让 Wine 启动失败，但会影响渲染正确性，尤其是菜单、工具提示、客户区。

### 6.4 `xdg_toplevel` / `xdg_popup` 语义不完整

`wayland_host.cpp:486-495` 的 `send_configure()` 发送的是空 states：

```cpp
wl_array states;
wl_array_init(&states);
xdg_toplevel_send_configure(..., &states);
```

其它 no-op：

- `xdg_toplevel_set_parent`：`1063`
- `set_max_size` / `set_min_size`：`1086-1087`
- `set_minimized`：`1121`
- `xdg_popup.grab` / `reposition`：`1244-1245`
- positioner 的 `set_parent_size` / `set_parent_configure`：`1347-1348`

影响：

- 最大化/全屏状态客户端可能不知道
- 最小/最大尺寸限制无效
- 子窗口/transient 关系无效
- 菜单弹出后 grab/reposition 行为不对
- 弹出菜单可能不消失或位置错误

Wine 的普通窗口可能还能用，但菜单和对话框会有问题。

### 6.5 剪贴板 / DnD

`wl_data_device_manager` 有，基础 selection 有，但：

- `data_device_start_drag()` 是 no-op：`wayland_host.cpp:2772`
- `data_offer_accept()` 是 no-op：`2724`
- `data_offer_set_actions()` 是 no-op：`2757`

所以：

- 基础复制粘贴大概率可用
- 拖放不支持
- Wine 会因为没有 `zwlr_data_control_manager_v1` 打 “clipboard functionality will be limited”

DWProton 更希望 `ext_data_control_manager_v1`，当前也没有。

### 6.6 没有 dmabuf / wl_drm

当前没有：

- `wl_drm`
- `zwp_linux_dmabuf_v1`

Wine 的软件渲染走 `wl_shm`，可以工作；但：

- Wine 的 Wayland EGL 路径依赖 Mesa 的 Wayland platform，可能需要 `wl_drm`/dmabuf
- Vulkan WSI 的 `VK_KHR_wayland_surface` 通常也需要 dmabuf
- 游戏/DXVK 的 GPU 加速很可能不可用或退化到软件

这是“能启动”之外的另一个大工程，不要和协议 globals 混为一谈。

---

## 7. 下次会话待办

### P0：让 Wine 原生 Wayland 驱动真正能启动 —— ✅ 完成 + 双标题栏修复（11-03 第二轮）

- [x] **替换假 viewporter**：
  - 删除 `Ports/LinuxGUI/WaylandHost/protocols/ext-viewporter-v1.xml`
  - CMake 改用 `/usr/share/wayland-protocols/stable/viewporter/viewporter.xml`
  - 生成 `viewporter-server-protocol.h` / `viewporter-protocol.c`
  - 实现 `set_source` / `set_destination`，double-buffered、commit 时应用，含 `bad_value` / `bad_size` / `out_of_buffer` / `viewport_exists` / `no_surface` 协议错误处理
  - 去掉 `get_dl` / `wp_viewport_dl` / `destination` event
  - 更新 `wayland_host.cpp` include 和 `CMakeLists.txt`
- [x] **driver 选择说明修正**：`WINE_WAYLAND_DRIVER=1` 错误注释已随假 viewporter 块删除；准确说法是 `HKCU\Software\Wine\Drivers\Graphics=wayland`（默认仍 X11/Xwayland，见 §1）
- [ ] **修正 `wl_output`**：❌ 未做，仍是 1600x1200 硬编码（见 §6.1，下一个该修）
- [x] **重新构建并部署修正版**：VM 内构建 → `/serenity/bin/`；`unset DISPLAY` 强制 Wayland 后 driver 正常加载，不再出现 `doesn't support wp_viewporter`，optional 协议缺失只打 ERR（pointer constraints / relative pointer / data control / icon）
- [x] **双标题栏修复**（用户实测反馈后追加，见 §4.2）


### P1：让 Wine 基本可用

- [x] 实现 `wl_region` + input/opaque region
  - 已实现：矩形列表、boolean difference subtract、双缓冲、未命中冒泡到祖先 surface
- [x] 加 `zwp_pointer_constraints_v1` + `zwp_relative_pointer_manager_v1`
  - 已实现：lock/confine（activated 抑制 wl_pointer.motion）、relative_motion 走 seat 级 delta；限制：桌面光标不 warp（需 WindowServer 支持）
- [x] 改善 `wl_subsurface`
  - Z 序：`place_above` → 自身 move_to_front；`place_below` → 兄弟/父 move_to_front
  - sync/desync：sync 模式 hold buffer 与 surface 状态，父 commit 原子 flush（desync 立即 flush）
  - 仍缺：裁剪到父窗口（LibGUI 无 per-window clip）
- [x] 补 `xdg_toplevel` configure states / min-max / maximized / fullscreen
  - configure 携带 maximized/fullscreen/resizing 状态；min/max size clamp；原生客户端 resize 现在发 configure；popup grab/reposition 仍 no-op

### P2：可选增强

- [ ] `wp_cursor_shape_manager_v1`（没有也能回退 SHM cursor）
- [ ] `zxdg_output_manager_v1`（输出名称/逻辑尺寸）
- [ ] `xdg_toplevel_icon_manager_v1`（窗口图标）
- [ ] `zwlr_data_control_manager_v1` / `ext_data_control_manager_v1`（剪贴板增强）
- [ ] `zwp_keyboard_shortcuts_inhibit_manager_v1`（仅 DWProton/Proton 路径需要重点验证）
- [ ] `zwp_linux_dmabuf_v1` / `wl_drm`（OpenGL/Vulkan/游戏加速，另立项目）
- [ ] `wp_fractional_scale_manager_v1` / `wp_color_manager_v1`（新版 Wine/DWProton，非 11.0 必需）

---

## 8. 验收测试

### 8.1 启动验证 —— ✅ 实测通过（11-03）

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-serenity-0
unset DISPLAY
export WINEPREFIX=$HOME/wine-wl-test
export WINEDEBUG=+waylanddrv
wine notepad
```

实测结果：

- ✅ 不再出现 `Wayland compositor doesn't support wp_viewporter`
- ✅ registry 里看到 `wp_viewporter version=1 id=8`
- ✅ notepad 窗口正常映射渲染（截图确认单标题栏）
- ✅ optional 缺失只打 ERR 不退出：`zwp_pointer_constraints_v1`、`zwp_relative_pointer_manager_v1`、`zwlr_data_control_manager_v1`、`xdg_toplevel_icon_manager_v1`
- ⚠️ `wayland_output_done` 仍是 1600x1200（wl_output 未修）
- ⚠️ 25s timeout 时进程仍活着（EXIT=124 = 还在运行，被外部杀掉）

### 8.2 协议验证 —— ✅ 实测通过

`WAYLAND_DEBUG=1 wine notepad` /tmp/wine-wl3.log 确认：

```text
-> wp_viewporter#10.get_viewport(new id wp_viewport#16, wl_surface#3)
-> xdg_surface#22.set_window_geometry(0, 0, 1140, 855)
-> wp_viewport#17.set_destination(1140, 855)
-> wp_viewport#17.set_source(0.00000000, 0.00000000, 1140.00000000, 855.00000000)
```

无 `invalid method`、无 listener NULL abort、无 protocol error 断连。

### 8.3 功能验证

- [ ] 窗口移动/缩放
- [ ] 菜单弹出/关闭
- [ ] 剪贴板复制粘贴
- [ ] 鼠标锁定/相对移动（游戏）
- [ ] Xwayland `xrandr` 尺寸
- [ ] 如果跑 DWProton：Alt+Tab / 游戏快捷键是否被桌面吞掉

---

## 9. 关键文件与行号

| 文件 | 内容 |
|---|---|
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:3480-3498` | globals 创建 |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:1527-1599` | 假 viewporter |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:79-80` | 硬编码 1600x1200 |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:866-867` | input/opaque region no-op |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:977-978` | wl_region no-op |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:1405-1408` | subsurface no-op |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:486-495` | 空 configure states |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:1063-1121` | xdg_toplevel no-op |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:1244-1245` | popup grab/reposition no-op |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:2772` | start_drag no-op |
| `Ports/LinuxGUI/WaylandHost/CMakeLists.txt` | viewporter 生成（标准 XML，已改） |
| ~~`Ports/LinuxGUI/WaylandHost/protocols/ext-viewporter-v1.xml`~~ | 假 XML **已删除** |
| `Ports/LinuxGUI/WaylandHost/wayland_host.cpp:1527+` | 真 viewporter 实现（§5.5） |
| `Ports/LinuxGUI/WinAppLauncher/main.cpp:440` | `DISPLAY=:100` |
| `Ports/LinuxGUI/SessionManager/serenity_manager.cpp:443-444,473` | `WAYLAND_DISPLAY`/`DISPLAY` |
| `Ports/LinuxGUI/Shell/main.cpp:45-51` | shell 导出显示变量 |
| `/tmp/opencode/wine-11.0-src/programs/explorer/desktop.c:43,996-1063` | Wine 默认驱动顺序/注册表 |
| `/tmp/opencode/winewayland-11.0/wayland.c:288-334` | Wine required/optional 检查 |
| `/tmp/opencode/winewayland-11.0/wayland_surface.c:168-169,448-450,578-580,1085-1088,1119-1137` | Wine viewporter/subsurface 使用 |
| `/tmp/opencode/winewayland-11.0/wayland_pointer.c:876-987` | pointer constraints / relative pointer |
| `/usr/share/wayland-protocols/stable/viewporter/viewporter.xml` | 正确的 viewporter XML |

---

## 10. 已知坑

- **不要用 `WINE_WAYLAND_DRIVER=1`**：Wine 11.0 不读它。
- **不要直接部署 `Build/lagom/bin`**：宿主 glibc 可能比 VM 新；部署二进制应在 VM 内构建。
- **部署 `serenity-wayland-host` 时注意 `Text file busy`**：先停会话，`rm` 再 `cp`。
- **SSH 命令行里含 `serenity-wayland-host` 可能被 `pkill -f` 误杀**：走 `/tmp` 脚本文件。
- **测试必须 `unset DISPLAY`**：否则 Wine 可能仍走 X11，看起来“能用”但不是 Wayland。
- **Xwayland 当前被 `wl_output` 带成 1600x1200**：修 output 后要回归 X11 应用窗口尺寸。
- ~~假 viewporter 的 `destination` event 会让 Wine 客户端 abort~~：已随假 XML 删除。
- **部署流程已验证可行**：VM 内 `cmake --build --target serenity-wayland-host` → `systemctl stop serenity-desktop.service` → `cp` → `start`，不需要手动杀窗口进程。

---

## 11. 本次未完成 / 交给下一次

### ✅ 第二轮已完成（11-03）

- 真 `wp_viewporter`：标准 XML + `set_source` / `set_destination` + 全套协议错误
- ViewModel 部署链路：宿主 ✅ / VM 内构建 ✅ / `/serenity/bin` 部署 ✅
- `wine notepad` 强制 Wayland 启动验证：driver 加载 ✅、窗口渲染 ✅、无 protocol error
- 双标题栏修复：Wine 窗口 frameless + `xdg_toplevel.move` / `resize` 交互拖动
- 假 XML 已删、错误注释已删、`CMakeLists.txt` 已改用标准 `viewporter.xml`

### ❌ 未完成 / 下次从这里开始

| 优先级 | 事项 | 说明 |
|---|---|---|
| ~~P0~~ | ~~`wl_output` 尺寸~~ | ✅ 已修（11-03 第三轮）：绑定时取 `GUI::Desktop::the()` 主屏实际 rect，物理尺寸 0x0；xrandr / Wine 均验证 1585x831 命中 |
| ~~P1~~ | ~~`wl_region` + input/opaque region~~ | ✅ 已修：矩形列表实现 add/subtract（boolean difference），input region 双缓冲提交绑定；**命中未覆盖的点会冒泡到最近可接收的祖先 surface**（Wine client subsurface 空输入区域 → 事件落到 toplevel） |
| ~~P1~~ | ~~pointer constraints + relative pointer~~ | ✅ 已修：`zwp_pointer_constraints_v1`（lock/confine，activated 时 suppress wl_pointer.motion，relative_motion 仍流）+ `zwp_relative_pointer_manager_v1`（相对移动 delta 走 seat 级跟踪）；缺失 ERR 已消失 |
| P1 | ~~subsurface Z 序~~ | ✅ 已修（11-03 第四轮）：`place_above` → 自身 move_to_front；`place_below` → 兄弟/父窗口 move_to_front（LibGUI 无 move_to_back，近似方案） |
| P1 | ~~subsurface sync/desync~~ | ✅ 已修：sync 模式 hold 住 buffer 与 surface 状态，父 surface commit 时原子 flush（含孙子、viewporter、region）；desync 立即 flush |
| P1 | ~~xdg configure states / min-max / maximized~~ | ✅ 已修：configure 携带 `maximized` / `fullscreen` / `resizing` 状态；`min/max_size` 入库并 clamp configure；`set_maximized`/`set_fullscreen` 走 `window->set_maximized/set_fullscreen` + 立即回 configure；原生客户端 resize 现在也发 configure（让 Wine 能及时重排 buffer）；`set_minimized` 已接到 `window->set_minimized` |
| P1 | popup grab / reposition | 仍是 no-op |
| P1 | configure 无 TILED_* / SUSPENDED / ACTIVATED | 未实现 |
| P1 | confine 仅 clamp 报告坐标 | 桌面光标不 warping（需 WindowServer 支持） |
| 待测 | 最大化/全屏/拖动缩放手测 | 自动截图只能看静态渲染，交互（最大化按钮、全屏、resize）未手测 |
| P2 | cursor shape / xdg output / toplevel icon / data control | 见 §7 |
| P2 | dmabuf / wl_drm | GPU 加速另立项目 |
| 待测 | 游戏手测 | pointer lock 实测进游戏后才算，notepad 覆盖不到 |

**验证记录**：`/tmp/wine-wl.log`（26s 超时退出 = 进程活着）、`/tmp/serenity-shot.ppm` 三轮截图对比（双标题栏 → 单标题栏）。

