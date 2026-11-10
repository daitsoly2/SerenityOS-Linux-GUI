# 立项：GTK/Linux 菜单映射为 WindowServer 原生菜单

> 状态：**P0 完成；P1 桥侧闭环已验证**（2026-11-10）
> 决策：立项 WS 映射，**冻结 GTK 侧菜单像素修补**。
> 与方向无关的 `state = state_of(widget)` 缺陷已单独修复并部署。
> 剩余：P1b —— GTK4 shim 拦截 pure-GMenuModel 的 `GtkPopoverMenu`。

## 1. 为什么做

GTK4 菜单当前由 `libserenitypaint.so` 在 **客户端进程**复刻
`LibGfx::ClassicStylePainter` 绘制，再作为 `xdg_popup` 位图提交给 WS。
而原生 Notepad 菜单是 **WindowServer 自己**在
`Userland/Services/WindowServer/Menu.cpp` 画的。两套代码、两套调色板、
两套主题同步机制，只能靠人肉对齐像素。

2026-11-10 与原生 Notepad 的像素对比证明这套路没走通：

| 维度 | 原生 Notepad | GTK4 复刻 |
|---|---|---|
| 外框 | 1px 纯黑 + 右下 3D 投影 | 1px 中灰，右边框缺失 |
| stripe | 1 条 24px | 2 条（25+24px），中间白缝 |
| 分隔线 | 只画文字区 | 贯穿全宽，切过 stripe |
| 快捷键 | 右对齐独立列 | 紧贴文字 |
| 图标 | 原生 icon 列 | 空 |

继续修这些 = 用另一套实现去逼近另一套实现，永远有下一处不一致。
**映射走 WS 后，双方调用同一份 `Menu.cpp`，差异在架构上消失。**

## 2. 现成基础（已逐项查证）

| 环节 | 位置 | 状态 |
|---|---|---|
| 菜单绘制 | `WindowServer/Menu.cpp`：黑框、stripe、hover、分隔线、图标、快捷键列 | 完整 |
| 输入 IPC | `WindowServer.ipc`：`create_menu` / `add_menu_item`（含 enabled、checked、shortcut、icon、submenu_id）/ `add_menu_separator` / `update_menu_item` / `popup_menu` / `dismiss_menu` | 完整 |
| 回传 IPC | `WindowClient.ipc`：`menu_item_activated` / `menu_item_entered` / `menu_item_left` / `menu_visibility_did_change` | 完整 |
| 客户端 API | `GUI::Menu::construct()` + `add_action()` + `popup()`；`Action::on_activation` 即激活回调 | 完整 |
| 代理进程 | `serenity-wlroots-host` 是 `GUI::Application`，已持有 WS IPC 连接 | 无需新增进程 |
| 通道可行性 | GDK 公开 `gdk_wayland_display_get_wl_display()`；GTK4 已有 `gtk-shell.xml` 私有协议 + `wayland-scanner` 生成链路 | 可直接复用 |
| 数据源 | `GtkPopoverMenu` 由 `GMenuModel` 驱动，可内省 item / label / shortcut / icon / enabled / checked / submenu | 完整 |
| 先例 | FileChooser：GTK → D-Bus → 原生 `serenity-file-picker` → Response 单播回传 | 已上线 |

菜单是 **per-connection**（`ConnectionFromClient::m_menus`），桥代理所有
客户端时要自行隔离 menu_id 命名空间。

## 3. 硬边界：为什么不能"全部"映射

`gtk_popover_set_child()` 允许 GTK 应用往 popover 里塞**任意 widget**：
gnome-text-editor 截图里的 `− 100% +` 缩放行、搜索框、颜色板、emoji
选择器、GtkSourceView 的补全列表等。WS 的 `add_menu_item` 只表达菜单项，
承载不了。

因此本项目的范围是：

- **覆盖**：纯 `GMenuModel` 驱动的 `GtkPopoverMenu`（含 submenu / checked / icon / shortcut）
- **不覆盖**：任意自定义 `GtkPopover` → 保留现有 GTK 自绘路径（painter + 全局主题）

即**双轨**。GTK 侧那套 painter/主题不会废弃，它仍是自定义 popover 的
唯一实现；冻结的只是"为纯菜单抠像素对齐"这部分工作。

判定规则：shim 在 popover 创建时检查是否为 `GtkPopoverMenu` 且
`GMenuModel` 可完整表达（所有 item 都是标准 attribute/link，无 custom
widget）→ 走 WS；否则 → 走现有自绘。判定必须保守，宁可回退。

## 4. 协议草案：`serenity_menu_v1`（私有 Wayland 协议）

新增 `Ports/LinuxGUI/WlrootsHost/protocol/serenity-menu-v1.xml`，
`wayland-scanner` 生成 server 侧（桥）与 client 侧（GTK4 shim）代码。

```
global  serenity_menu_manager_v1
  request create_menu(id: new_id<serenity_menu_v1>, surface: object<wl_surface>,
                      x: int, y: int, anchor_w: int, anchor_h: int)
  request destroy

interface serenity_menu_v1
  request add_item(identifier: uint, text: string, shortcut: string,
                   enabled: bool, checkable: bool, checked: bool,
                   submenu: object<serenity_menu_v1>?)
  request add_separator()
  request add_icon(identifier: uint, fd: fd, width: uint, height: uint, stride: uint)
  request popup(serial: uint)
  request dismiss()
  request destroy()

  event  activated(identifier: uint)
  event  dismissed()
  event  hovered(identifier: uint)
```

要点：

- **锚点坐标**：`x/y/anchor_w/anchor_h` 用 `wl_surface` 局部坐标，由桥换算成
  `popup_menu` 需要的 `screen_position` + `button_rect`。桥已有
  `HostedWindow::screen_relative_rect()`，可直接用。
- **图标**：GIcon → 渲染成 ARGB8888 → `wl_shm` 风格 fd 传递 → 桥转成
  `Gfx::ShareableBitmap` 交给 `add_menu_item`。
- **激活回传**：WS `menu_item_activated` → 桥按 `identifier` 发
  `serenity_menu_v1.activated` → shim 触发对应 `GAction::activate`。
- **生命周期**：客户端断开 / surface 销毁 → 桥 `dismiss()` + `destroy_menu`，
  防止 WS 侧菜单泄漏。

## 5. 分阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 协议 XML + scanner 接入两侧构建；桥注册 global，能收到 `create_menu` | `WAYLAND_DEBUG=client` 见到 global，探针能 bind |
| P1 | 纯文本最小闭环：GMenuModel → `add_item` → `popup` → WS 原生菜单弹出 → 激活回传触发 GAction | 探针菜单由 `Menu.cpp` 绘制，与 Notepad 像素一致；点击执行动作 |
| P2 | submenu / separator / checked / enabled / shortcut / 动态 `update_menu_item` | 与 GMenuModel 状态一致 |
| P3 | 图标传递；主题切换自动同步；生命周期与崩溃清理 | 换主题后菜单跟着变；杀客户端无残留菜单 |
| P4 | 判定与回退：非纯菜单 popover 自动走 GTK 自绘；回归 GTK3 / libadwaita / Firefox | 自定义 popover 不空白、不双弹 |

## 6. 风险

1. **grab / 焦点**：WS 原生菜单自带 grab，与桥当前 `xdg_popup.grab` 语义
   叠加可能冲突。P1 必须验证键盘导航与外部点击 dismiss。
   （顺带可能治好"popover 收不到 motion、hover 不亮"的老问题。）
2. **判定误判**：把含自定义 child 的 popover 送去 WS 会丢内容 → 必须保守回退。
3. **X11 / GTK3 路径**：Xwayland 与 GTK3 客户端没有 GMenuModel 序列化能力，
   初期只能走现有自绘，双轨长期存在。
4. **per-connection menu_id**：桥代理多客户端，必须隔离 namespace。
5. **协议演进**：私有协议可改，但 GTK4 shim 与桥必须同版本部署。

## 7. 本次冻结项（不废弃，仅暂停）

上一节诊断出的 GTK 侧菜单像素差异，在 P1 落地后会由 `Menu.cpp` 同源解决：

- 双 stripe（`MENU_ITEM` 分支重复画 stripe，且 GTK4 shim 缺 idle 抑制）
- 灰框（shim CSS `menu, popover, .menu` 的 `@serenity_shadow` 盖过 painter 黑框）
- 右边框缺失
- 行高 / 字号 / 分隔线 / 快捷键列对齐

若 P1 验证不可行，再解冻按原清单修。

## 8. 与方向无关、已完成

- `overlay/gtk4/gtk/gtkserenity.c`：`state = state_of(widget)`，修复未初始化
  栈值传给真 painter 导致 hover/checked/pressed 随机的缺陷。已编译部署，
  `unused function 'state_of'` 警告消失。

## 9. P0 完成记录（2026-11-10）

**已落地并验证**：

| 文件 | 内容 |
|---|---|
| `WlrootsHost/protocol/serenity-menu-v1.xml` | 私有协议定义（manager + menu 两个 interface） |
| `WlrootsHost/protocol/gen.sh` | 固化 `wayland-scanner` 四路生成，产物随源码提交 |
| `WlrootsHost/protocol/serenity-menu-v1-{server,client}.{c,h}` | 生成的桩代码 |
| `WlrootsHost/serenity_menu_proto.{c,h}` | 纯 C 协议模块：global 注册、请求解析、事件回传 API、C↔C++ callbacks |
| `WlrootsHost/wlroots_proto.{c,h}` | 新增 `wlroots_proto_display()` 访问器 |
| `WlrootsHost/main.cpp` | 实例化协议模块 + P0 日志回调 |
| `WlrootsHost/CMakeLists.txt` | 接入新源文件 |
| `WlrootsHost/tests/menu-probe.c` | P0 回归探针 |

**验收实测**（VM，2026-11-10）：

```
BOUND name=14 version=1
REQUESTS_SENT
--- server log ---
ws-menu: serenity_menu_manager_v1 global ready
ws-menu: client bound serenity_menu_manager_v1 v1
ws-menu: create_menu menu=0x... surface=(nil) anchor=100,200 40x24
ws-menu: add_item id=1 text='Save' shortcut='Ctrl+S' ...
ws-menu: add_item id=2 text='Save As...' shortcut='Shift+Ctrl+S' ...
ws-menu: add_separator menu=0x...
ws-menu: add_item id=3 text='Quit' shortcut='Ctrl+Q' ...
ws-menu: popup menu=0x... serial=42
ws-menu: destroy menu=0x...
```

**过程中修掉的坑**：
- XML 的 `surface` 起初没标 `allow-null="true"`，客户端传 NULL 被
  `wl_proxy_marshal` 拒绝 → P0 探针无 surface，已放开（P1 仍应传真实 surface 做锚点换算）。
- `wl_resource_destroy()` 在 wayland-server 1.26 只收一个参数。
- 宿主 `Build/lagom/CMakeCache.txt` 里 `XCB_IMDKIT_LIB` 指向不存在的
  `/usr/lib/x86_64-linux-gnu/libxcb-imdkit.so.1`，CMake 重配后链接失败；
  改为 `Build/vm-libs/libxcb-imdkit.so.1.0.9` 后通过（工作记忆已有此坑）。

**下一步 = P1**（纯文本最小闭环）：在 `main.cpp` 回调里建 `GUI::Menu` +
`GUI::Action`，`popup()` 时按 anchor 换算 `screen_position` 调 `Menu::popup()`，
`Action::on_activation` 回发 `serenity_menu_v1.activated`；随后在 GTK4 shim
侧拦截 pure-`GMenuModel` 的 `GtkPopoverMenu`，序列化并改用本协议，同时抑制其
`xdg_popup` 自绘路径。验收标准见 §5。

## 10. P1 桥侧闭环完成记录（2026-11-10）

**改动**（都在 `Ports/LinuxGUI/WlrootsHost/`）：

| 文件 | 内容 |
|---|---|
| `wlroots_proto.{c,h}` | 新增 `wlroots_proto_surface_from_resource()`：把协议收到的 `wl_resource*`（锚点 surface）反查成 `wlr_surface*`，供宿主窗口表定位；按 object id + client pid 匹配，避免 per-client id 撞车 |
| `main.cpp` | P0 日志回调全部替换为真实实现（见下）；回调实现移到 `lookup()` 之后，因为要用 `s_windows`；补 LibGUI/Menu/Action/Shortcut 等头文件 |
| `tests/menu-probe.c` | 改为弹出后保持连接并 dispatch 事件；菜单内容对齐原生 Notepad「编辑」菜单；支持点击/键盘激活、`dismissed` 回传验证；`SERENITY_MENU_HOLD_SECONDS` / `_X` / `_Y` 可调 |

**桥侧实现要点**：

- `s_native_menus`（`HashMap<void*, NonnullOwnPtr<NativeMenu>>`）持有 `GUI::Menu`
  强引用，key 是协议层 `serenity_menu_node*`。`node_destroy_notify` 先回调
  `menu_destroy` 移除引用、再 `free(node)`，所以 Action lambda 捕获 node 在自身
  生命周期内始终有效。
- `menu_create`：`menu_anchor_origin()` 把 surface 局部锚点 + 宿主窗口
  `screen_relative_rect()` 换算成屏幕坐标；surface 为空（探针）直接用原始坐标；
  查不到宿主窗口时保守回退，绝不崩。
- `menu_add_item`：`GUI::Action::create[_checkable]` + `set_enabled` /
  `set_checked`；shortcut 字符串用 `ENUMERATE_KEY_CODES` 宏生成的反查表解析成
  `GUI::Shortcut`（因为 WS 收的是 `Shortcut::to_byte_string()` 的显示文本，不能
  直接塞字符串）。每个 Action 的 lambda 捕获客户端 identifier —— WS 回传的是
  Menu 内部 item_id，`ConnectionToWindowServer` 会找到对应 Action 再 `activate()`，
  **identifier 映射天然对齐，无需额外查表**。
- `menu_popup` → `Menu::popup(screen_position, nullptr, button_rect)`；
  `menu_dismiss` → `Menu::dismiss()` + 回发 `dismissed`；
  `menu_destroy` → 从 side table 移除。
- P2/P3 占位：`submenu` / `update_item` / `add_icon` 目前只打日志并安全忽略
  （`add_icon` 会 `close(fd)`，不泄漏）。

**实测验收**（VM，2026-11-10，探针 `surface=NULL`、anchor=(300,200)）：

```
BOUND name=14 version=1
REQUESTS_SENT
ws-menu: create_menu at 300,200 button=40x24
ws-menu: add_item id=1 'Undo' 'Ctrl+Z' ... / id=2 'Redo' enabled=0 ... (×6)
ws-menu: popup -> Menu.cpp at 300,200
ACTIVATED id=1                     ← 探针 listener（键盘 Down+Return 触发）
ws-menu: activated id=1 -> client  ← 桥侧 Action 回调
DISMISSED / DONE                   ← 客户端 dismiss 的回传确认
```

截图 `p1-native-menu.png` / `p1-menu-zoom.png` 证明菜单由 `Menu.cpp` 绘制。
与原生 Notepad「编辑」菜单的 **y=250 行逐像素对比**：

| 指标 | 原生 Notepad | P1 | 旧 GTK4 painter 版 |
|---|---|---|---|
| 左边框 | `rgb(0,0,0)` 1px | `rgb(0,0,0)` 1px ✅ | `rgb(128,128,128)`，右边框缺失 |
| stripe | 1 条 24px `rgb(219,216,209)` | 1 条 24px `rgb(219,216,209)` ✅ | 2 条（25+24px），中间白缝 |
| 分隔线 | 不切 stripe | 不切 stripe ✅ | 贯穿全宽切过 stripe |
| 快捷键 | 右对齐独立列 | 右对齐独立列 ✅ | 紧贴文字 |
| 禁用项 / 3D 投影 | 有 | 有 ✅ | — |

**两个有价值的发现**：

1. **本地化自动同源**：探针明明发的是 `"Undo"/"Cut"/"Paste"`，菜单显示的却是
   「撤销 / 剪切 / 粘贴」。根因是 `LibGUI/Action.cpp:108` 的
   `m_text(translate_ui(text))` —— 映射走 WS 后菜单文本自动过系统级
   `zh-CN.ini [UI]` 翻译表。这是 GTK 自绘路线（各应用自带 gettext）做不到的。
2. **事件循环坑**：探针初版用 `wl_display_dispatch()` + `poll()`，鼠标点击看着
   "没反应"；改成标准 `prepare_read → poll → read_events → dispatch_pending`
   后立刻收到 `ACTIVATED id=1`。裸 `dispatch()` 会吞掉先前 `roundtrip` 已缓冲
   的事件，排查协议问题时容易误判成服务端没发。

**明确未做（P1b）**：GTK4 shim 尚未拦截 `GtkPopoverMenu`。当前 GTK4 应用的菜单
**仍走旧的 xdg_popup 自绘路径**，只有独立探针会走 WS 原生菜单。这是有意的
渐进部署：global 缺失或客户端未接入时自动回退，不会把现有桌面弄坏。
P1b 需要做：拦截 pure-`GMenuModel` 的 popover 生命周期、序列化 item/label/
shortcut/icon/enabled/checked、抑制其 `xdg_popup` 自绘、把 `activated` 映射回
`GAction::activate`、处理 grab/dismiss 生命周期。

**部署注意**：替换 `/serenity/bin/serenity-wlroots-host` 必须杀掉运行中的桥，
serenitid 会自动重启；这会**中断现有 Wayland 客户端连接**（Xwayland 会重连，
但已打开的 GTK 应用会退出）。生产环境应在会话空闲时部署。
