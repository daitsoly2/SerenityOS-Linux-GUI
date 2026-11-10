# 核心稳定性审计：WindowServer + WlrootsHost 桥（2026-11-09）

> 目标：找出「单个客户端的坏输入 → 整个 GUI 崩溃」的路径并修掉，而不是追求零 bug。
> 范围：`Userland/Services/WindowServer/**`、`Ports/LinuxGUI/WlrootsHost/**`，
> 顺带看了 `X11Compat/xwm.cpp`、`ImeService`、`AppletManager`。
> 结论：**18 处已修（3 处是确定性 abort/SIGBUS）**；原先记录的 6 项遗留也已在第二轮处理完。

---

## 0. 结论速览

| # | 位置 | 触发 | 后果 | 状态 |
|---|---|---|---|---|
| 1 | `ConnectionFromClient::set_window_backing_store` | pitch/size 与匿名 fd 不匹配 | SIGBUS / `VERIFICATION FAILED` | ✅ 已修 |
| 2 | 同上 | `create_with_anonymous_buffer` 失败后仍 `release_value()` | 必崩（`value()` 内 VERIFY） | ✅ 已修 |
| 3 | `ConnectionFromClient::set_accepts_drag` | 无拖放时发 SetAcceptsDrag | `VERIFY(wm.dnd_client())` abort | ✅ 已修 |
| 4 | `WindowManager::clear_theme_override` | 配置里的主题文件缺失 | `MUST(load_system_theme)` abort | ✅ 已修 |
| 5 | `ConnectionFromClient::create_window` / `set_window_minimum_size` | rect / minimum_size 超大 | 窗口框位图分配失败 → `release_value_but_fixme` abort | ✅ 已修 |
| 6 | `WindowManager::set_theme_override` | 主题覆盖缓冲短于 `sizeof(SystemTheme)` | 越界读 + 垃圾度量 | ✅ 已修 |
| 7 | `ConnectionFromClient::create_menu` / `destroy_menu` | 复用 menu_id、窗口未销毁就删菜单 | 菜单栏悬垂 `Menu&` → UAF | ✅ 已修 |
| 8 | `Menu::content_width` | `set_menu_minimum_width(INT_MAX)` / 超长菜单项 | 内部窗口位图分配 abort | ✅ 已修 |
| 9 | `ConnectionFromClient::set_window_parent_from_client` | 二次设父 / 互设父子成环 | `VERIFY(!m_parent_window)` abort；成环后祖先遍历死循环/爆栈 | ✅ 已修 |
| 10 | `WMConnectionFromClient::set_applet_area_position` | 没有任何 applet（托盘为空）时 WM 上报位置 | `window()` 为空 → 空指针解引用 | ✅ 已修 |
| 11 | `WMConnectionFromClient::start_window_resize` | `resize_direction` 越界 | 一路传到 `resize_window()` 的 switch → `VERIFY_NOT_REACHED` | ✅ 已修 |
| 12 | `get_wallpaper` | 没配壁纸 | `wallpaper_bitmap()` 空指针 | ✅ 已修 |
| 13 | `get_screen_bitmap` / `..._around_location` | 请求 30000×30000 截图 | 数 GB 分配 → OOM 掉 WS | ✅ 已修（收敛到屏幕包围盒） |
| 14 | 桥 `copy_buffer_to_bitmap` | stride < width*4 / 空指针 / 未知格式 | 逐行越界读 → SIGSEGV | ✅ 已修（丢帧） |
| 15 | 桥 `on_toplevel_title` | 客户端从未设置 title（NULL） | `ByteString(nullptr)` → strlen(NULL) | ✅ 已修 |
| 16 | 桥 `on_popup_created` / `on_subsurface_created` | surface 已被登记成 ContentLayer | `entry->widget` 为空却继续解引用 | ✅ 已修（忽略） |
| 17 | 桥 `ensure_hosted_window` | X11 窗口报 30000×30000 | LibGUI 后备位图 OOM，桥被杀 → 所有外来窗口消失 | ✅ 已修（初始尺寸收敛到桌面） |
| 18 | `ImeService::read_until_handled` | 引擎持续输出不回 `handled` | 缓冲无限增长 → OOM | ✅ 已修（1MB 上限） |
| 19 | `Compositor::recompute_occlusions` | 不透明/透明区相交（主题圆角 + 颜色键等组合） | 4 处 `VERIFY` → abort | ✅ 已修（相交时剔除透明区，不再断言） |
| 20 | `Compositor::update_wallpaper_bitmap` | 屏幕尺寸位图分配失败 | `release_value_but_fixme` abort | ✅ 已修（清壁纸、退回纯背景色） |
| 21 | `ConnectionFromClient::get_screen_bitmap` | 带偏移的截图区域 | 目标坐标用了屏幕绝对坐标 → 内容错位 | ✅ 已修（相对请求区域原点） |
| 22 | `ScreenLayout::is_valid` | scale=0/超大、resolution 超大 | 0 尺寸位图 / 内存炸弹 | ✅ 已修（scale 1..4、分辨率 ≤8192） |
| 23 | `XimServer::commit_utf8` | X11 客户端异常断开后 IC 被 imdkit 释放 | `s_ic` 悬垂 → UAF | ✅ 已修（只用当前回调栈内的 IC） |
| 24 | `ImeService::read_until_handled` | 引擎卡住 | 桥事件循环每次按键卡 200ms | ✅ 已修（总预算 50ms，`SERENITY_IME_TIMEOUT_MS` 可调） |

**已修但需要真机回归**：#1/#5/#8 属于「正常应用也可能踩到」的路径（改尺寸、开菜单、换主题），
建议在 VM 里跑一遍 gedit / Firefox / 设置 / 开始菜单。

---

## 1. 三个「一行就能崩」的点（修复前后）

### 1.1 后备存储：`ConnectionFromClient::set_window_backing_store`

原代码：

```cpp
auto buffer_or_error = Core::AnonymousBuffer::create_from_anon_fd(anon_file.take_fd(), pitch * size.height());
...
auto backing_store_or_error = Gfx::Bitmap::create_with_anonymous_buffer(..., size, 1);
if (backing_store_or_error.is_error()) {
    did_misbehave("");          // ← 少了 return
}
window.set_backing_store(backing_store_or_error.release_value(), serial);
```

两个独立问题：

1. `release_value()` 内部是 `value()`，`value()` 里有 `VERIFY(!is_error())`；
   任何让 `create_with_anonymous_buffer` 失败（宽/高 ≥ 4M、负数、pitch×height 乘爆）的
   报文都会把 WS 直接 abort。
2. `Gfx::Bitmap` **不校验匿名缓冲实际大小**（`Bitmap` 构造函数只 VERIFY 不溢出）。
   `pitch` 与 `size` 不匹配时，位图 `m_data` 指向一块比 `pitch*height` 小的映射，
   合成器第一次 blit 就 SIGBUS。`pitch * size.height()` 本身还是 `int` 乘法，
   大窗口会先溢出成负数再转 `size_t`。

修法（均为服务端校验，不改客户端契约）：

- `size` 必须在 `(0, INT16_MAX]`；`pitch >= width*4`；`required = (i64)pitch * height`；
- `fstat(anon_file.fd()).st_size >= required`（memfd 的 `ftruncate` 大小就是真实容量）；
- 任意一步失败 → `did_misbehave(...) + return`；
- `create_with_anonymous_buffer` 失败也 `return`；
- `visible_size` 收敛到**当前** backing store 的真实尺寸（swap 分支里生效的是另一块 bitmap）。

### 1.2 菜单栏的裸引用

`Menubar` 存的是 `Vector<Menu&>`（源码里本来就有 FIXME：不支持移除菜单）。
而 `destroy_menu` 只 `close()` 就把 `Menu` 从 `m_menus` 删掉 → 菜单栏继续指向已释放对象，
下一次画标题栏 / `for_each_menu()` 就是 UAF。`create_menu` 复用同一 id 时更直接：
`m_menus.set()` 覆盖旧的 `OwnPtr`，旧 `Menu` 当场析构。

修法：

- `Menubar::remove_menu(Menu&)`（顺带清 `m_flashed_menu`）；
- `ConnectionFromClient::detach_menu_from_menubars(Menu&)`：遍历本连接所有窗口，摘掉并
  `invalidate_menubar()`；
- `destroy_menu`：先摘 → `close()` → `remove_child()` → 最后 `m_menus.remove()`；
- `create_menu`：id 已存在时先走同一套摘除 + close，再覆盖。

### 1.3 主题覆盖

- `set_theme_override` 收的是客户端给的 `AnonymousBuffer`，`PaletteImpl` 直接当
  `SystemTheme` 读。短缓冲 = 每次取色/取度量都越界读。现在要求
  `size() >= sizeof(Gfx::SystemTheme)`。
- `clear_theme_override` 原来 `MUST(load_system_theme("/res/themes/<Name>.ini"))`。
  这个端口上 `Theme/Name` 完全可能指向不存在的文件（只读前缀、改过名），
  于是一个客户端发 ClearSystemThemeOverride 就能把 WS 打死。现在失败只记日志、
  保持当前主题（`m_theme_overridden` 也只在成功后才清）。

---

## 2. 尺寸类输入：为什么必须设上限

`WindowFrame::paint()` 里有：

```cpp
m_top_bottom = Gfx::Bitmap::create(..., { frame_rect_including_shadow.width(), top_bottom_height }, scale)
                   .release_value_but_fixme_should_propagate_errors();
```

`Gfx::Bitmap::size_would_overflow()` 会拒绝 `width/height >= 1<<22` 和负数。
所以只要让窗口矩形变成 `INT_MAX`（或让 `set_rect` 算出负数），这里必然 abort。
而 `create_window` 的 `rect` / `minimum_size`、`set_window_minimum_size` 的 `size`
原来都没有上限（只有 `set_window_rect` 有 `INT16_MAX` 检查）。

现在三处统一为 `(0, INT16_MAX]`（`set_window_rect` 原有规则），
`minimum_size` 另外拒绝负数（`Window::set_minimum_size` 里有 `VERIFY(size >= 0)`）。

菜单同理：`Menu::content_width()` 现在把最终宽度收敛到 `Screen::bounding_rect().width()`，
一次性覆盖「最小宽度」和「超长菜单项文本」两个来源。

---

## 3. 父子窗口：VERIFY 与成环

`Window::set_parent_window()` 第一行是 `VERIFY(!m_parent_window)`，
而 `set_window_parent_from_client` 原来不检查子窗口是否已有父窗口 →
同一个 child 发两次就是 abort。

更糟的是可以成环：客户端先 `AddWindowStealingForClient(self, w1)`（自己的窗口可以给自己授权），
再 `SetWindowParentFromClient(self, w1, w2)`、`SetWindowParentFromClient(self, w2, w1)`。
之后 `Window::is_descendant_of()` 的父链遍历永不终止，
`for_each_window_in_modal_chain()` 无限递归 → 爆栈；`destroy_window()` 同样无限递归。

现在：

- child 已有父窗口 → `did_misbehave + return`；
- 从候选 parent 沿 `parent_window()` 上溯，遇到 child 就拒绝（此时链上还不可能有环）。

---

## 4. 桥程序（serenity-wlroots-host）

### 4.1 像素入口

`copy_buffer_to_bitmap()` 改成返回 `bool`，进入前检查：`pixels != null`、
`width/height > 0`、`stride >= width*4`、格式只能是 `ARGB8888/XRGB8888`、
目标 bitmap 尺寸与 `width/height` 一致。失败就丢帧（画面停一帧好过桥进程消失）。
wl_shm 理论上已保证 stride，但这是唯一像素入口，且 `int stride` 来自
`(int)size_t` 强转，挡一次很便宜。

### 4.2 生命周期

- `on_toplevel_title`：xdg title 在客户端设置前是 `NULL`，现在 `title ? title : ""`。
- `on_popup_created` / `on_subsurface_created`：`ensure_hosted_window()` 命中已存在的
  `ContentLayer` entry 时没有 `window/widget`，原来会直接解引用；现在记一行日志后忽略。
- `ensure_hosted_window`：X11 客户端可能报远大于屏幕的初始尺寸；LibGUI 会照这个尺寸
  分配后备位图，桥被 OOM 杀掉等于所有外来窗口一起消失。现在初始尺寸收敛到桌面大小，
  真实尺寸仍由后续 buffer commit 决定。

### 4.3 输入法

- `read_until_handled()` 的接收缓冲加 1MB 上限：引擎协议流错位时不再无限增长。
- `handle_panel()` 的候选数量收敛到 `[0, 1000]`。
- `XimServer::caret_screen_rect()`：`xcb_setup_roots_iterator().data` 为空时不再解引用。

---

## 5. 第二轮：原先“记录未修”的 6 项也已处理

| 位置 | 原风险 | 处理方式 |
|---|---|---|
| `Compositor::recompute_occlusions()` 的 4 处 `VERIFY` | 不透明/透明矩形一旦相交就 abort | 相交时用 `shatter()` 把透明区从遮挡区里剔掉；`affected_transparency_rects` 重复命中改为合并；末尾三个“缓存矩形 vs 壁纸矩形”断言降级为调试日志。几何错了最多多画/少画一块，不再杀进程 |
| `Compositor::update_wallpaper_bitmap()` | 屏幕尺寸位图分配失败 → `release_value_but_fixme` abort | 改成 `ErrorOr` 分支：打日志、清掉该屏壁纸位图；`paint_wallpaper()` 里对应的 `VERIFY` 改为“没有位图就只画背景色” |
| `get_screen_bitmap(rect)` 带偏移 | 目标坐标用了屏幕绝对坐标，截图内容整体错位 | 先把请求区域与包围盒求交，再用 `src_rect.location() - requested_rect.location()` 作为目标坐标；同时天然挡住了超大分配 |
| `ScreenLayout` 只校验 `scale_factor >= 1` | scale 极大 → `resolution/scale` 为 0 → 0 尺寸位图 | `is_valid()` 增加 `scale ∈ [1,4]`（与 `Gfx::Bitmap` 一致）和分辨率 `≤ 8192` |
| `XimServer::s_ic` | 客户端异常断开后 IC 可能已被 imdkit 释放 | 新增 `s_callback_ic`，只在 `xcb_im_filter_event()` 的回调栈内有效；`commit_utf8()` 只认它。回调外提交变成 no-op，而不是 UAF |
| `ImeService::handle_key` 同步等 200ms | 引擎卡住时桥的事件循环（所有窗口输入）一起卡 | 改成**总预算** 50ms（`SERENITY_IME_TIMEOUT_MS` 可调 1–1000ms），`EINTR` 不再当超时；超时按“未消费”把键还给客户端 |

仍然保留、但已不属于“崩溃”范畴的：

- `ImeService` 的同步等待本身还在（协议要求“这个键吞不吞”必须当场决定），
  只是最坏延迟从 200ms 降到 50ms；要彻底异步得改引擎协议。
- `Compositor` 的遮挡计算遇到坏几何会画出轻微残影，属于视觉问题，靠 `OCCLUSIONS_DEBUG` 日志排查。
- `Menubar` 移除菜单后不会重排剩余菜单（留空位），仅在运行期动态删菜单时可见。

---

## 6. 验证

- 环境问题：`ninja -C Build/lagom` 现在会在 **CMake 重配置**阶段失败
  （`pkg_check_modules(sdl2)` 找不到 sdl2，`Ports/LinuxGUI/CMakeLists.txt:33`），
  与本次改动无关；需要 `libsdl2-dev` 才能整包构建。
- 因此用 `Build/lagom/compile_commands.json` 里记录的原始命令逐个编译改动的 TU，全部通过：
  `ConnectionFromClient.cpp`、`WindowManager.cpp`、`Menu.cpp`、`Window.cpp`、
  `WMConnectionFromClient.cpp`、`AppletManager.cpp`、
  `WlrootsHost/{main,ImeService,XimServer}.cpp`。
- 建议真机回归：改窗口尺寸到极大（`xdotool windowsize`）、疯狂开/关右键菜单、
  设置里切主题后取消覆盖、空托盘启动、gedit 菜单连点、Firefox 菜单。

### 如何自己构造坏 IPC（回归用）

最小复现思路（LibGUI 客户端里直接调对应 `ConnectionToWindowServer` 消息）：

1. `SetWindowBackingStore`：`pitch = 4`、`size = {1000, 1000}`、fd 只 `ftruncate(4000)`。
   修复前：首帧 SIGBUS。修复后：客户端被断开，WS 存活。
2. `SetAcceptsDrag(true)`（不启动拖放）。修复前：`VERIFICATION FAILED`。
3. `CreateWindow` 带 `rect = {0,0,INT_MAX,INT_MAX}`。修复前：首帧 abort。
4. `CreateMenu` + `SetMenuMinimumWidth(INT_MAX)` + `PopupMenu`。修复前：菜单窗口分配 abort。

---

## 7. VM 实测（2026-11-09 16:19–16:36）

### 构建与部署（本环境特有，下次照做）

`ninja -C Build/lagom` 会在 CMake 重配置阶段失败，原因是 `CMakeCache.txt` 比
`build.ninja` 新（上次失败重配置留下的时间戳），加上宿主缺 `libxcb-imdkit`、
`crypt.h`、`libcrypt.so`。**不改动源码**的绕过步骤：

1. `touch -d '2026-11-08 12:00:00' Build/lagom/CMakeCache.txt`
   —— ninja 比的是 `.ninja_log` 里记录的 mtime，`touch build.ninja` 无效。
2. 从 VM 取 `/usr/lib/x86_64-linux-gnu/libxcb-imdkit.so.1.0.9` 到
   `Build/vm-libs/`，建 `libxcb-imdkit.so` / `.so.1` 软链；把 `build.ninja` 里
   那条绝对路径改指过去；链接时 `LIBRARY_PATH=Build/vm-libs`。
3. 从 VM 取 `/usr/include/crypt.h` 到 `Build/lagom/crypt.h`（已在 `-I` 路径里），
   并在 `Build/vm-libs/` 建 `libcrypt.so -> /usr/lib/x86_64-linux-gnu/libcrypt.so.1`。
4. `LIBRARY_PATH=... ninja -C Build/lagom -j 8 serenity-window-server serenity-wlroots-host`。
5. 部署：`systemctl stop serenity-desktop` → `install` 到 `/serenity/bin` →
   `systemctl start`。原二进制已备份为
   `/serenity/bin/serenity-{window-server,wlroots-host}.bak-20261109`。

**注意 `/tmp` 在本会话的不同命令间不共享**，临时库/截图要放工作区内。

### 裸 IPC 注入（`scripts/ipc_stability_probe.py`）

按 `LibIPC/Message.cpp` 的帧格式（u32 长度 + endpoint magic 2938215075 + i32 MessageID
+ 小端原生参数）手写恶意消息，连 `/tmp/portal/window`。每个用例用新连接。

| 用例 | 服务端日志 | WS 结果 |
|---|---|---|
| 无拖放发 `SetAcceptsDrag(true)` | （静默忽略，符合预期） | 存活 |
| `CreateWindow` rect = INT_MAX | `misbehaved (CreateWindow with bad window sizing)` | 存活，连接断开 |
| `SetWindowMinimumSize` = INT_MAX | `misbehaved (SetWindowMinimumSize with bad size)` | 存活，连接断开 |
| 后备存储 pitch=1 | `misbehaved (SetWindowBackingStore: Pitch smaller than a row)` | 存活，连接断开 |
| 后备存储 fd 4KB 却声明 40000B | `misbehaved (...Backing store is smaller than the bitmap it describes)` | 存活，连接断开 |

### A/B 对照（决定性证据）

把 WS 换回备份的旧二进制，只发同一条 `SetAcceptsDrag(true)`：

```
旧版 WS pid 1948 → 注入后 1987（崩溃重启）
VERIFICATION FAILED: wm.dnd_client() at ConnectionFromClient.cpp:956
WindowServer: fatal signal 4, backtrace: ...
```

换回新版后同一条消息：**pid 不变，无任何崩溃日志**。
这同时排除了「注入消息其实没到服务端」的假阳性。

### 真实应用回归

| 场景 | 结果 |
|---|---|
| 计算器 / Notepad / 文件管理器三窗口叠放 | 渲染正常，标题栏与菜单栏无回归 |
| Theme Editor（含嵌套预览窗）+ 显示设置 + 截图 + 2 套重复窗口 | 6 窗口重叠，遮挡计算无 VERIFY |
| 15 次客户端 `SIGKILL`（5 轮 × 3 窗口） | WS pid 2066 全程不变，仅一条 `Disconnected from peer` |
| gedit（GTK，走 Wayland 桥 + text-input-v3） | 正常渲染、焦点/commit 链路工作，桥 pid 2068 不变 |
| 任务栏窗口反复切换 10 次 | 无花屏、无崩溃 |
| 全程日志 | 除 A/B 对照的旧版外，`VERIFICATION FAILED` / `fatal signal` = **0** |

**未覆盖**：切主题后取消覆盖（`clear_theme_override` 的 MUST 分支）、空托盘启动、
截图带偏移区域、`ScreenLayout` scale 上限。这四项需要 UI 精确点选或构造特定布局，
建议在下一轮手工验证。

---

## 8. 输入法引擎与设置界面 / 托盘脱节（本轮 VM 实测，已修复）

**结论：用户的判断属实，而且有两个独立原因。以下诊断是修复前的现场证据。**

1. **VM 里部署的 `serenity-engine` 是旧二进制。**
   VM：`/serenity/bin/serenity-engine` 11-07 21:38 / 51640 B，
   `strings` 里没有 `ok active`、`serenity-ime-status`、`中文拼音`；
   源码 11-08 11:00 才改成 librime 直连，宿主产物 11-08 11:10 / 123352 B。
   控制 socket 实测旧版回答 `status -> ok rime luna_pinyin`，
   当前源码 `handle_control()` 应答是 `ok active 1 rime` —— 协议都对不上。
2. **`serenity-ime-status` 状态文件根本不存在。**
   托盘 `IMEIndicator` 每 500ms 轮询 `$SERENITY_IME_STATUS_DIR/serenity-ime-status`
   （默认 `/tmp`），新引擎 `publish_status()` 才会写它；旧引擎不写，
   所以任务栏一直显示 `EN`。注意：新引擎与指示器的默认目录都是 `/tmp`，
   **部署新版引擎后不需要额外环境变量即可对齐**。
3. **设置界面仍指向 fcitx5 时代的路径。**
   `IMESettings/main.cpp` 开头注释就写着服务于 fcitx5 `serenityime` addon：
   模糊音/候选数写 `~/.local/share/fcitx5/rime/*.custom.yaml`，
   启用状态读 `~/.config/fcitx5/profile` 的 `DefaultIM`，
   应用时调 `rime_deployer` + `org.fcitx.Fcitx5` D-Bus `Restart`。
   新引擎用的是 `~/.local/share/rime/`（VM 日志 `user_data=...` 已确认），
   VM 里也没有 fcitx5 进程。控制 socket 部分（`set rime` / `toggle off`）
   新旧兼容，所以「启用中文输入法」开关有效，其余配置项全部写到没人读的地方。

### 环境变量该在哪里注入

`serenitid`（`Ports/LinuxGUI/SessionManager/serenity_manager.cpp`，`serenity_main()`）
是**唯一**生效的注入点：598–665 行 `setenv` 之后才 `make_child` 启动 WS、桥、引擎、
shell、launch-server，这些子进程及 shell 再派生的 Linux 应用都继承。
**WS / IME 自己 `setenv` 没有作用**——它们是子进程，环境无法回流到父进程或兄弟进程，
而且与 Linux 应用是并行关系。

要覆盖「不是桌面后代」的 Linux 应用（systemd --user 服务、D-Bus 激活的应用、
从外部终端/SSH 启动的进程），需要额外一层：
`/etc/environment.d/50-serenity.conf`（或 `~/.config/environment.d/`）
+ 会话开始时 `dbus-update-activation-environment --systemd <变量列表>`。
另外 manager 里输入法相关的 `setenv(..., 0)` 是「不覆盖」，外部若已有同名变量
（例如 `/etc/environment` 里的 `XMODIFIERS`）会赢，建议改成 `1`。

### 本轮改动（4 个文件）

| 文件 | 改动 |
|---|---|
| `SessionManager/serenity_manager.cpp` | 注入 `SERENITY_IME_STATUS_DIR=$XDG_RUNTIME_DIR/serenity-ime`（`mkdir 0700`，失败退回 `/tmp`）；`XMODIFIERS`/`DISPLAY` 改覆盖写；调 `dbus-update-activation-environment --systemd` 把会话变量推给 user unit / D-Bus 激活应用；ime child 设 `always_restart = true` |
| `IMEEngine/serenity-engine.cpp` | 控制 socket 从 `status_dir()` 派生（原来硬编码 `/tmp`，注入专用目录后会与托盘/设置失联）；新增 `reload` 控制命令，优雅退出让 serenitid 拉回并重新部署 rime |
| `IMESettings/main.cpp` | rime 目录改 `~/.local/share/rime`（与引擎 `RIME_USER_DATA_DIR` 对齐）；删掉 fcitx5 profile、`busctl ... Restart`、`rime_deployer`、double-fork；启用状态改查控制 socket `status`；应用后发 `set rime`/`toggle off` + `reload` |
| `scripts/vnc_abs_click.py` | 新增：VNC RFB 绝对坐标点击工具（`vclick` 的 Y 翻转在本 VM 上不准） |

**注意**：只改状态目录不够，必须同时改引擎的 ctl socket 派生规则 ——
托盘和设置界面都从 `status_directory()` 拼 ctl 路径，引擎原来写死 `/tmp`。

### VM 验证（16:58 部署后）

| 检查 | 结果 |
|---|---|
| `SERENITY_IME_STATUS_DIR` 注入 | engine 与 shell 环境均为 `/run/user/1000/serenity-ime` ✅ |
| 状态文件 | `serenity-ime-status = 1\trime\t中文拼音`、methods 两项，均生成 ✅ |
| 控制协议 | `status -> ok active 1 rime`（新版协议，旧版是 `ok rime luna_pinyin`）✅ |
| 托盘中英提示 | 开启时蓝色「中」，`toggle off` 后灰色 `EN`，再 `set rime` 变回「中」✅ |
| 设置界面启用状态 | engine off 时「启用中文输入法」未勾选 —— 确实走控制 socket 查询 ✅ |
| 配置目录 | 新版二进制只含 `/.local/share/rime`；旧版含 `fcitx5/rime`、`config/fcitx5/profile`、`busctl`、`org.fcitx.Fcitx5` ✅ |
| 配置读路径 | 在新目录写 `menu/page_size: 6` + 单组模糊音，重启设置界面后 SpinBox 显示 6、只勾选 z/zh ✅ |
| `reload` 自动重启 | engine pid 2885 → 2978，日志 `ime exited (status 0)` → `restarting ime in 200 ms` → `started ime`；开关状态保持开启 ✅ |
| 跨版本 librime | 宿主 1.17.0 编译 / VM 1.13.1 运行；两端 `rime_api.h` md5 完全一致，实测 `rime ready (user_data=/home/serenity/.local/share/rime)` ✅ |
| 全程 | WS pid 2878 未变、服务 active、无新增崩溃记录 ✅ |

**未能自动验证**：设置界面「应用」按钮的点击（VNC 相对鼠标下坐标不准）。
但写入与读出共用 `luna_custom_path()`，读路径已证实正确，加上 `reload` 链路
已单独验证，配置落盘与生效路径是完整的。建议下次用真机手工点一次「应用」。

### 补充（17:30）：候选字体大小

用户指出输入法设置里缺字体大小调整。先澄清现状：

- **全局字体大小本来就有**：显示设置 →「字体」页 → FontPicker 的字号 SpinBox（1–255），
  `FontSettingsWidget::apply_settings()` 调 `set_system_fonts()`，WS 广播给所有客户端、
  写 `/etc/WindowServer.ini`、触发重绘。VM 当前是 `Sarasa Gothic SC 10 400 0`。
  只是路径藏得深（要点「字体」→「...」→ 选字号 → 确定 → 应用）。
- **缺的是输入法候选窗的独立字号**：`InputMethodCandidateWindow` 全程用 `font()`
  （全局默认字体），`IMESettings` 只有启用 / 模糊音 / 候选数 / 预览四项。
- 整体 UI 缩放（HiDPI）另属 `WAYLAND_ROADMAP §1.3` 的 P3，本次未做。

**实现（4 个文件，故意不改任何类的内存布局）**：

| 文件 | 改动 |
|---|---|
| `LibGUI/InputMethodCandidateWindow.h` | 加 4 个自由函数与常量（默认 14、范围 8–48），不改 `InputMethodPanelWidget` 的类定义 |
| `LibGUI/InputMethodCandidateWindow.cpp` | 配置文件 `~/.config/serenity/ime-candidate-font-size` 读写；`set_content()` 每次刷新重读并按 `presentation_size` 比较后 `set_font_size`；进程级 preview override 只给设置界面用 |
| `IMESettings/main.cpp` | 加「候选字体大小」SpinBox；`load_from_disk` / `apply_to_disk` 读写配置；预览即时跟随（不落盘）；修掉大字号把底部按钮挤出窗口的布局 bug |
| （桥 `ImeService.cpp`） | **无需改动**：它用的 `s_widget->set_content()` 就在 LibGUI 里，自动生效 |

设计要点：三个使用者（原生 LibGUI 候选窗、Wayland 桥候选窗、设置界面预览）都是
`InputMethodPanelWidget`，都走 `set_content()`，所以一处改动全覆盖；用自由函数 +
静态变量而不是加类成员，避免像 `Window` 加数据成员那样平移布局、逼迫全量重编客户端。
配置每次刷新重读，改完设置下一次打字即生效，不必重启应用；越界/损坏一律回退 14。

**VM 验证**：

| 检查 | 结果 |
|---|---|
| 无配置时 | SpinBox 显示 14、预览为默认字号 ✅ |
| 写 `22` 后重启设置界面 | SpinBox 显示 22、预览候选文字明显变大 ✅ |
| 写 `999`（越界） | SpinBox 回退 14、预览恢复正常 ✅ |
| 大字号布局 | 预览宽度封顶后「确定 / 取消 / 应用」三按钮仍可见 ✅ |
| 桥进程 | `/proc/<pid>/maps` 确认加载的是新部署的 `/serenity/lib/liblagom-gui.so.0.0.0` ✅ |
| 兼容性 | `ldd -r` 无 undefined / not found；桌面重启后四个核心进程齐全 ✅ |

部署时把 `liblagom-gui.so.0.0.0` 同时更新到 `/serenity/lib/` 和 `/usr/local/lib/`，
都留了 `.bak-20261109`。测试配置已删除，回到默认 14。

**未做实弹验证**：真实候选窗弹出时的字号（VNC 键盘/鼠标注入在这台 VM 上不可靠）。
但桥与原生候选窗共用同一个 `set_content()`，预览已验证该路径。建议真机打一次拼音确认。

---

## 9. librime 要不要改成源码集成（评估）

**结论：现阶段不建议；先做「私有化部署 + rpath」性价比最高。**

现状：`Apps/CMakeLists.txt:247` 用 `find_library(RIME_LIBRARY NAMES rime)` 动态链接，
运行时依赖 `librime.so.1` + `/usr/share/rime-data`，以及 glog / yaml-cpp /
leveldb / marisa / opencc / gflags / snappy / lzma 等一串传递依赖。

| 方案 | 成本 | 收益 | 何时选 |
|---|---|---|---|
| **A. 私有化部署 + rpath**（推荐） | 半天 | 把 `librime.so.1` 放 `/serenity/lib`、`rime-data` 放 `/serenity/share`，引擎加 `-rpath,$ORIGIN/../lib` 并把 `traits.shared_data_dir` 指过去；顺带消除宿主 1.17 / VM 1.13 的版本漂移 | 目标机器不该被要求装 `librime` 包时 |
| B. 静态链接 librime | 1–2 天 | 去掉 `librime.so` 依赖，但 yaml-cpp/leveldb/opencc 等仍是动态库 | 想锁死版本、又不想 vendored 全家桶 |
| C. 全源码 vendored | 1–2 周 | 完全自包含（项目已有 vendored wlroots 先例） | 要跑在无包管理环境（LiveCD / 嵌入式 / 路由器） |
| D. 自研极简拼音引擎 | 不建议 | 会丢掉模糊音、用户词频、简繁转换，与设置界面现有功能直接冲突 | — |

不建议现在做 C 的理由：① librime 核心加子依赖是十几万行 C++，CMake 子工程要处理
交叉编译与静态库顺序，维护成本高；② 词库/方案数据本来就是运行时资源，源码集成
并不能免去数据分发；③ 项目定位是「界面复古、底层优先成熟组件」，动态链接成熟库
符合这个原则；④ 本轮修的三个问题跟链接方式毫无关系，源码集成不会带来可感知改善。
真正值得警惕的是**编/跑版本不一致**（这次 1.17 vs 1.13），A 就能解决。

---

## 10. Linux 宿主访问代理 serenity-hostd（用户管理垂直切片）

用户报「管理用户功能无效」。VM 复现：从控制面板点「管理用户」或单独启动
`serenity-users-settings`，弹出 `UsersSettings must be run as root in order to
manage users.`。

### 四层根因（整个应用没做 Linux 账户体系移植）

1. `.af` 标了 `RequiresRoot=true` → `AppFile::spawn_with_escalation()` 调 Serenity 的
   `/bin/Escalator`，VM 上 `/bin/Escalator`、`/serenity/bin/Escalator`、`/bin/pls` 全不存在。
2. `main.cpp` 的 `getuid() != 0` 硬门槛，桌面会话是 uid=1000。
   （`pledge/unveil` 在 Lagom 是 `System.h:81` 的 inline no-op，所以没在更早阶段失败。）
3. `Core::command("useradd"/"userdel")` 用 `posix_spawnp`，VM 的 PATH 不含
   `/usr/sbin/`（命令实际都在那里）。
4. 语义不兼容：`DEFAULT_USER_GROUPS` 含 `window`/`lookup`/`phys`（VM 无这些组）、
   `WHEEL_GROUP_NAME="wheel"`（Debian 用 `sudo`）；**最危险**的是
   `Core::Account::sync()` 会按 Serenity 格式重刷 `/etc/passwd`、`/etc/group`、
   `/etc/shadow`，足以破坏系统登录。现在那个 root 检查反而挡住了更坏的事。

### 方案：能力化的 root 代理（用户提议）

和项目已有的 `config-server` / `launch-server` / `file-system-access-server` 同一套
服务化架构，只是代理的是 Linux 宿主而非 Serenity 语义。GUI 不需要 root，也不用
`sudo -E`（会丢 `DISPLAY`/`WAYLAND_DISPLAY`）；危险写操作收敛到一个入口。

| 组件 | 说明 |
|---|---|
| `Ports/LinuxGUI/Hostd/serenity-hostd.cpp` | root 守护（**11-09 晚间由 Python 改写为 C++**，见 §12），Unix socket `/run/serenity-hostd/socket`；能力 `ping` / `users.list` / `users.add` / `users.delete` / `users.modify` / `users.passwd` |
| `Ports/LinuxGUI/Hostd/serenity-hostd.service` | systemd unit，`Before=serenity-desktop.service`，desktop 侧加 `Wants/After` drop-in |
| `Userland/Applications/UsersSettings/HostdClient.h` | 纯头文件客户端（避免改 CMakeLists 触发重配置）；hex 编码字段、解析应答；`hostd_failure()` 用进程级静态缓冲承接 `Error::from_string_view` 的视图 |
| `UsersSettings` 4 个 .cpp + `Constants.h` | 写操作全改走代理，去掉 root 门槛与 pledge/unveil；`.af` 改 `RequiresRoot=false` |

**安全边界**：socket `root:serenity 0660`；`SO_PEERCRED` 只允许 uid 1000/root；
用户名白名单 `^[a-z_][a-z0-9_-]{0,31}$`；目标 UID 限定 1000–60000；禁止删
系统账户与当前桌面会话用户；shell 必须在 `/etc/shells`；组动态挑宿主机存在的；
命令一律 `argv` 数组不经过 shell；密码走 `chpasswd` 的 stdin 不进 argv；
每次请求审计到 journald。这是「单用户桌面上的本机管理员」模型，多用户场景
应在这里接 Polkit，而不是放宽 uid 校验。

**额外挖出的隐藏 bug**：GML 编译器生成的 `UsersTab::try_create()` 只搭控件树，
**不会调用手写的 `initialize()`** → 用户列表、增删改按钮回调从来没接上，界面能开
但完全空。修法是在 `main.cpp` 显式 `TRY(users_tab->initialize())`，并给
`initialize()` 加幂等 guard（否则会重复挂占位 label、重复绑定按钮回调）。

### VM 验证

| 检查 | 结果 |
|---|---|
| 服务与 socket | `active`，`/run/serenity-hostd/socket` 权限 `root:serenity 0660` ✅ |
| 协议 | `ping`、`users.list`（只列 1000–60000 的普通用户，不再含 nobody）✅ |
| 真实增删 | 创建 `testu1`（中文全名、自动匹配 VM 实际存在的组）→ 改密码 → 改详情/管理员 → 删除，全 `ok` ✅ |
| 输入/权限校验 | 重复用户名、非法用户名、删 `root`、删会话用户、未知能力全部正确拒绝 ✅ |
| 系统文件完整性 | `/etc/passwd|group|shadow` 字段数保持 7/4/9；passwd 与备份逐行一致；测试用户与家目录清理干净 ✅ |
| GUI | 不再弹 root 错误框；列表显示 `serenity`；`initialize` 只执行一次 ✅ |
| 兼容 | `serenitid` / WS / 桥 / 引擎不受影响，桌面服务 active ✅ |

部署前已把 `/etc/passwd`、`/etc/group`、`/etc/shadow` 备份为 `.bak-20261109`，
`UsersSettings.af` 也留了 `.bak-20261109`。冒烟脚本在
`scripts/port/hostd-smoke-test.py`。

**未做实弹验证**：GUI 里点「Add User...」按钮走完整流程（VNC 相对鼠标在这台 VM
上点不准）。协议层已用相同请求串验证过，GUI 侧调用的是同一组 `hostd_*` 函数。
建议真机手工点一次。

**后续扩展**：网络、显示、电源、包管理等设置页可以照 `能力.操作` 格式加新能力，
GUI 侧继续用 `HostdClient.h` 的 `hostd_call()`，不需要再改权限模型。

---

## 11. 一句话总结

这一轮修的都是「服务端相信了客户端的数字」：尺寸、pitch、缓冲区大小、父窗口关系、
菜单生命周期。修完之后，同样的坏输入最多让**那个客户端**被断开，
不再把 WindowServer 或桥进程一起带走。VM 实测与 A/B 对照已证实这一点。

---

## 12. 用户报错「Failed to add user: 参数个数不对」→ hostd 改写为 C++（19:17 截图）

### 12.1 根因：`strip()` 吃掉了尾随空字段

截图（`/home/ovo/图片/截图/截图 2026-11-09 19-17-06.png`）里填了
Full Name=tuzhi、Username=ovo、**密码留空**，点「添加」得到
`Failed to add user: 参数个数不对`。

hostd 日志同一时刻（19:16:57）只有一条 `users.add 失败: 参数个数不对`，
没有 `useradd` 记录 —— 说明请求在参数校验阶段就被拒了，压根没到建号。

请求行是 `users.add\t<username>\t<full_name_hex>\t<admin>\t<password_hex>`；
密码为空时 hex 是空串，行尾就是一个 `\t`：

```
users.add\tovo\t74757a6869\t0\t      # split("\t") -> 5 段，正确
```

而 Python 版 `handle()` 读行用 `...decode().strip()`：**`str.strip()` 无参时把
tab 也算空白**，行尾的 `\t` 被一起裁掉 →

```
users.add\tovo\t74757a6869\t0        # split("\t") -> 4 段
```

于是落到 `dispatch()` 末尾的 `raise HostdError("参数个数不对")`。
即：**空密码 = 报"参数个数不对"**，与真实原因（缺密码）完全对不上，
用户自然看不出该怎么改。

### 12.2 改写：`Ports/LinuxGUI/Hostd/serenity-hostd.cpp`

用户要求把这个 Python 守护改成 C/C++。改完是单文件、无第三方依赖的 C++17 程序，
协议与安全模型一字不动，只修 bug 与补强：

| 项 | Python 版 | C++ 版 |
|---|---|---|
| 读行裁剪 | `strip()`（吃掉尾随 `\t`） | 只裁 `\r\n`，`split('\t')` 保留空字段 |
| 空密码 | `参数个数不对` | `密码不能为空` |
| 建号后设密码失败 | 留下无密码的半成品账户 | `userdel --remove` 回滚，再回报原始错误 |
| 命令 stdin | 继承守护进程 | 有输入走管道；无输入接 `/dev/null` |
| 命令超时 | `subprocess timeout=30` | `poll` + 单调时钟 30s，超时 `SIGKILL` |
| UTF-8 | `bytes.decode('utf-8')` | 手写校验（拒绝过长编码/代理对/越界码点） |
| 依赖 | `/usr/bin/python3` | 无（98 KB ELF） |

保持不变的：能力集、hex 编码、`ok` / `ok\n<rows>\n.\n` / `err <原因>` 应答、
`SO_PEERCRED` 只认 uid 0/1000、用户名/UID/密码/全名白名单、组名按宿主机实际存在挑选、
命令一律 argv 数组（不过 shell）、密码只走 `chpasswd` 的 stdin、每次请求写 journal。

编译与部署：

```sh
Ports/LinuxGUI/Hostd/build.sh                 # -> Build/hostd/serenity-hostd
sudo install -m 0755 Build/hostd/serenity-hostd /serenity/libexec/serenity-hostd
# service: ExecStart=/serenity/libexec/serenity-hostd（不再是 python3 + .py）
```

`build.sh` 故意不进 Lagom 的 CMake 图：它跑在宿主/VM 上，独立编译可以避免
每次改一行就触发 CMake 重配置（宿主上那步容易因缺 `libsdl2` / `libxcb-imdkit` 失败）。

### 12.3 验证

宿主侧（root，socket 挪到 `Build/hostd-test/`）：

| 用例 | 结果 |
|---|---|
| `users.add\t…\t0\t`（空密码，尾随 tab） | `err 密码不能为空` ✅（旧版是 `参数个数不对`） |
| `users.add\t…\t0`（真缺密码，4 段） | `err 参数个数不对` ✅ |
| hex 非法 / 奇数长度 / 非 UTF-8 | `err 全名 编码不合法` ✅ |
| 用户名大写 / 超长 / 保留名 | `err 用户名必须…` / `不允许操作系统保留账户` ✅ |
| 密码含换行、全名含冒号 | `err 密码含非法字符` / `全名含非法字符` ✅ |
| 子进程：stderr+退出码 | 取 stderr 最后一行作为错误 ✅ |
| 子进程：stdin→stdout | 管道回灌正常 ✅（这里抓到并修掉了父进程关闭 fd 写反的 bug） |
| 子进程：超时 | `sh 执行超时`，子进程被 `SIGKILL` ✅ |
| `SIGTERM` | 退出时删除 socket ✅ |
| mock 命令验证 argv | `--create-home --shell /bin/bash --comment 张三 --groups … zzztest`，密码只进 stdin ✅ |
| chpasswd 失败 | 触发 `userdel --remove` 回滚 ✅ |

VM 实测（部署 19:27）：

| 检查 | 结果 |
|---|---|
| 服务与 socket | `active`，`/run/serenity-hostd/socket` 仍是 `root:serenity 0660` ✅ |
| `add(空密码)` | `err 密码不能为空`（截图里的报错消失）✅ |
| 真实建号 → 改密码 → 改详情 → 删除 | 全 `ok`，`/etc/passwd` 无残留 ✅ |
| `delete(root)` / `delete(会话用户)` / 未知能力 | 正确拒绝 ✅ |
| 旧实现 | `/serenity/libexec/serenity-hostd.py.bak-20261109`（不再被服务引用）✅ |

**未做实弹验证**：GUI 里点「Add User...」走完整流程（VNC 键盘注入在这台 VM 上不可靠）。
GUI 调用的就是 `hostd_add_user()`，协议层已用同一条请求串验证过。
按当前行为，空密码时弹窗会显示 `Failed to add user: 密码不能为空`。

### 12.4 GUI 级实证（19:30，顺带踩了一个部署顺序坑）

在 VM 里以 `serenity` 启动 `serenity-users-settings`（环境变量直接抄 `serenity-shell` 的
`/proc/<pid>/environ`），在 VNC 上点 Username 输入 `ovo`、密码留空点「添加」：

```
19:30:19 hostd: 请求 users.add uid=1000
19:30:19 hostd: users.add 失败: 密码不能为空      ← 截图里那条「参数个数不对」已消失
```

**但这次部署的二进制是在修 stdin 管道之前编译的**：`run()` 里父进程关错了 fd
（关掉了写端、然后往读端写 → `EBADF` → 直接 break），`chpasswd` 立刻读到 EOF、
以 0 退出，于是「建号成功、密码没写进去」。19:30:26 那次成功的 `users.add ovo`
（gecos=tuzhi，用户自己在控制台接着操作）就落成了 `ovo:!:…` —— 账户被锁。

已重建并重部署（md5 `e2551bd2…`）后复测：

| 检查 | 结果 |
|---|---|
| 真建号后 `getent shadow testpw1` | `$y$j9T$…`（真的是哈希，不再是 `!`）✅ |
| `users.passwd` 改密 → `users.delete` | 全 `ok`，无残留 ✅ |
| 冒烟脚本重跑 | 全部通过 ✅ |

教训两条：① **管道方向**——父进程只保留写端、必须关掉自己的读端，否则子进程等不到 EOF；
② **先编译到最终状态再部署**，`build.sh` 的输出和部署物要对一次 md5。

**遗留**：VM 里 `ovo`（uid 1001）密码位是 `!`（锁定），需要用户在
「用户设置 → 改密码」里重设一次（该路径已用 `users.passwd` 验证可用）。



---

## 13. 中文化补全（19:36–19:50）

### 13.1 为什么「中文化不全」

`LibGUI/UITranslation.cpp` 是这套端口的运行时翻译表：`set_text` / `set_title` /
`add_tab` / `GroupBox` 等 setter 会去 `$SERENITY_RESOURCE_ROOT/locale/<lang>.ini`
的 `[UI]` 里做**精确匹配**，命中就换，否则原样返回。所以漏翻只有两种原因：

1. 表里没有这条键（绝大多数）；
2. 键和源码里的字符串**不是逐字一致** —— 少个冒号、多个行尾空格、`...` 写成 `…`。

例如 `Username=用户名` 早就有了，但 GML 里写的是 `"Username:"`（带冒号），
精确匹配失败，界面上就一直是英文。反过来表里也有成对的 `Copying:=…` 和
`Copying: =…`，因为源码里两种写法都有。

### 13.2 审计工具漏了 GML

`scripts/port/ui-strings.py` 原来只扫 `*.cpp` 里带 `set_text` 等调用的行。
而这套应用大量文案写在 `.gml` 的 `text:` 里（编译成 setters），于是
「用户设置」这类界面**审计报 80% 覆盖、实际满屏英文**。

已修：新增 `GML_TEXT` 正则扫描 `text/title/placeholder/tooltip:`。改完后
真实覆盖率从「80%」掉到 62%（1781 处里 1107 处），这才是实际情况。

### 13.3 本轮补的条目

新增 **243 条**到 `Base/res/locale/zh-CN.ini`（`[UI]` 段尾部，
标记为 `# ===== 2026-11-09 补全 =====`）：

| 范围 | 说明 |
|---|---|
| 用户设置 | 账户类型 / 全名 / 用户名 / 密码 / 登录 Shell / 主目录 / UID-GID / 更改密码… / 添加用户… 及全部错误文案 |
| 显示设置 | 外观、动画、屏幕设置、配色方案、窗口主题、各类字体与阴影开关 |
| 鼠标 / 键盘 / 时钟 / 日历 / 终端 / 游戏 / 浏览器 | 各页全部标签与说明 |
| 文件属性 | 元数据标签（标题/艺术家/采样率/ICC…）与 Read/Write/Execute |
| 通用 | Not found、Input Method、Background、各种 `Failed to ...: {}` 模式 |

写入方式是脚本（`Build/add_zh.py`，一次性），带**反向校验**：字典里的键必须
能在源码扫描结果或现有表里找到，否则报错——这是唯一能挡住「少个空格就整条失效」
的办法。实际抓到两处手误：`used by the clock`（源码是 `used for`）、
`Couldn't save file: {}`（源码末尾还有句号）。

改完：**1650 条 / 覆盖率 77%**，「用户设置」**35/35 = 100%**。
剩下 362 个未翻译串大头是图像编辑、邮件、棋类、表格的内部错误提示，
以及一批 `{}` 占位超过一个的格式化串（翻译表只支持单占位）。

### 13.4 ComboBox 的值不走翻译表

`Account Type` 下拉框里的 `Standard` 一直是英文，因为值列表由 `ItemListModel`
直接交给 `ListView` 绘制，不经过任何 `set_text`。修法是在应用侧翻译一次：

`UsersSettings/Constants.h` 新增 `account_type_names()`，用**函数内 static**
（翻译表懒加载，静态初始化期读不到）缓存 `ByteString` 与对应的 `StringView`，
返回 `Array<StringView, 2> const&`；两个调用点
（`UserAddDialog.cpp:24`、`UserDetailsWidget.cpp:76`）改用这个函数。

**为什么要返回引用**：`ItemListModel` 存的是 `Container const&`，返回临时
`Array` 会让下拉框第一次展开就 UAF（这个坑 11-07 已经踩过一次）。

### 13.5 VM 验证

| 检查 | 结果 |
|---|---|
| 部署 | `/serenity/res/locale/zh-CN.ini`（`/res` → `/serenity/res`），旧文件留 `.bak-20261109` ✅ |
| 详情面板 | 用户名 / 全名 / 登录 Shell / 主目录 / UID / GID / 账户类型 + 「更改密码…」「添加用户…」「删除用户」全中文 ✅ |
| 添加用户对话框 | 标题「添加用户」、账户类型 / 全名 / 用户名 / 密码全中文 ✅ |
| 下拉框值 | 应用重编 + 重部署（`Build/lagom/bin/serenity-users-settings` → `/serenity/bin`，留 `.bak-20261109`）✅ |
| 控制面板分类视图 | 「经典视图」「双击图标打开对应的设置程序」等原有翻译未回归 ✅ |

**注意**：翻译表在进程内是 static 缓存，改 ini 后**必须重启应用**才生效。

---

## 14. 评估：Linux 应用的文件选择器能否接到 Serenity 原生（**已实现，见 §15**）

**结论：可以，走 xdg-desktop-portal 前端接口。不要试图去改 GTK 的对话框。**

### 现状（VM 实测）

- GTK 应用（gedit）现在弹的是**自己的** `GtkFileChooserDialog` —— 它是个普通的
  Wayland 客户端，由桥变成一个 Serenity 窗口，所以外观/行为都不是原生的。
- VM 里 `xdg-desktop-portal` + `xdg-desktop-portal-gtk` 都装了，但
  `/usr/share/xdg-desktop-portal/portals/gtk.portal` 写着 `UseIn=gnome`。
  会话是 `XDG_CURRENT_DESKTOP=Serenity`，**选不中任何后端**，于是
  `GtkFileChooserNative` 静默退回自带对话框。
- 原生侧有现成的入口：`LibGUI::FilePicker::get_open_filepath()` /
  `get_save_filepath()`（`LibGUI/FilePicker.cpp`），
  `file-system-access-server` 就是用 `get_filepath(Badge<...>)` 给原生应用弹选择器的。

### 方案

| 组件 | 作用 |
|---|---|
| `serenity.portal`（新，GDBus） | 实现 `org.freedesktop.impl.portal.FileChooser` 的 `OpenFile` / `OpenFiles` / `SaveFile`；`UseIn=Serenity`，D-Bus 名 `org.freedesktop.impl.portal.desktop.serenity` |
| `serenity-file-picker-helper`（新，小 app） | 用 `LibGUI::FilePicker` 弹原生对话框，把选中路径打到 stdout 后退出；取消则非零退出。portal 后端 fork 它并读 stdout |
| `serenitid` 改动 | 给 Linux 侧设 `GTK_USE_PORTAL=1`；确保 `XDG_CURRENT_DESKTOP=Serenity`（已有）；`dbus-update-activation-environment` 推这些变量 |
| 部署物 | `serenity.portal` 放进 `/usr/share/xdg-desktop-portal/portals/`，配 `org.freedesktop.impl.portal.desktop.serenity.service` |

链路：GTK 应用 → `OpenFile` → xdg-desktop-portal（前端，已存在）→ 我们的后端 →
helper 弹原生 FilePicker → 返回 `file://` URI。

### 已确认的前提 / 坑

- 宿主与 VM 都有 `glib-2.0` 开发包和 `gdbus-codegen`，后端可以用 GDBus 写 C++，
  不必引 dbus-1 或 sd-bus。
- portal 的 `parent_window` 是 `wayland:<token>`，需要 **xdg-foreign** 才能把
  原生对话框贴到 GTK 窗口上；桥里没有这个协议。第一版直接**忽略 parent 居中显示**
  （功能不受影响，只是位置不跟随），后续再补。
- 返回码语义：`response=0` 成功且 `results["uris"]` 是 `asa{sv}`；`1` 取消、`2` 其它错误。
- `filters` 要映射到 `FilePicker::FileTypeFilter`（portal 用 glob，LibGUI 用
  `String::matches`，注意它是**整串 glob**，`*.png` 不是 `png`）。
- `GtkFileChooserNative` 在 GTK3/GTK4 都走这套；Electron/Chromium 走
  `org.freedesktop.portal.FileChooser` 同样受益。但 X11 老应用（Motif/Qt4）不经过 portal。
- 工作量：后端 + helper + 部署脚本约 1–2 天；风险集中在 parent_window 与 filters 映射。

### 不建议的两条路

- **改 GTK**：GTK 没有「文件选择器插件」这种扩展点，只能改 GTK 源码重编（不可维护）。
- **桥里拦截**：识别 `GtkFileChooserDialog` 再替换窗口 —— 依赖 GTK 内部结构，版本一变就废。

---

## 15. 文件选择器接原生：实现（20:00–20:15）

§14 的方案落地了。链路：**GTK 应用 → `org.freedesktop.portal.FileChooser`
（我们自己的 serenity-portal）→ fork helper → 原生 FilePicker → Response(file:// URI)**。

### 15.1 三个新件 / 四处改动

| 文件 | 说明 |
|---|---|
| `Ports/LinuxGUI/FilePickerHelper/main.cpp`（新） | `serenity-file-picker`：唯一职责是弹 `GUI::FilePicker`，把选中路径打到 stdout（0=选中，1=取消，2=参数错）。GUI 进程才能弹对话框，portal 守护不是 GUI 进程，所以必须分进程 |
| `Ports/LinuxGUI/Portal/portal.cpp` | 新增 `org.freedesktop.portal.FileChooser`（OpenFile/SaveFile/OpenFiles/SaveFiles + `version`=3）、`org.freedesktop.portal.Request`（Close + Response 信号）、`org.freedesktop.host.portal.Registry`（Register）；主循环从手写 `sd_bus_wait` 改成 `sd_event` |
| `Ports/LinuxGUI/Apps/CMakeLists.txt` | 加 `serenity-file-picker` 目标 |
| `Ports/LinuxGUI/WlrootsHost/wlroots_proto.c` | 启用 **xdg-foreign v1 + v2**（`wlr_xdg_foreign_registry/v1/v2_create`） |
| `Ports/LinuxGUI/SessionManager/serenity_manager.cpp` | 注入 `GTK_USE_PORTAL=1`，并**主动把 serenity-portal 作为常驻子进程拉起**（见 §15.5） |
| `Base/res/locale/zh-CN.ini` | 选择器自己的文案：`Look in:` / `Filename:` / `Files of Type:` |

### 15.2 踩到的五个坑（每个都表现为“配好了但没反应”）

1. **GTK 用 portal 之前必须能 export 父窗口**：没有 xdg-foreign 时 GTK3 打一行
   `Server is missing xdg_foreign support` 就**静默退回自带对话框**；GTK4 连警告都很少。
   桥里加上 v1/v2 之后 export 成功，GTK 才真正发 `OpenFile`。
   （我们并不 import 这个 token —— 原生对话框居中显示，不跟随 GTK 窗口。）
2. **`org.freedesktop.host.portal.Registry.Register`**：GTK4 4.18 起先调它上报
   app_id，失败就整个放弃 portal。必须实现（回空成功即可）。
3. **`version` 必须报 3**：GTK4 的 `GtkFileDialog` 读到 <3 直接回退，连 `OpenFile`
   都不发。我们语义够用，如实报 3；`OpenFiles/SaveFiles` 退化成单选。
4. **Response 要延迟约 300ms 再发**：helper 秒退时（脚本化 helper、或立刻失败），
   Response 会和 `OpenFile` 的应答抢在同一个主循环里出去，客户端还没挂上 match，
   信号就丢了 —— 表现为“应用永远在等”。真人操作时这 300ms 完全感知不到。
5. **`sd_event_add_child` 会漏掉“注册前就已退出”的子进程**：exec 失败立刻 exit 127
   时 SIGCHLD 早过去了，事件源永不触发。改成 100ms `waitpid(WNOHANG)` 轮询。

另外两个非坑但容易忘的：

- portal 是 D-Bus 激活的，**环境里没有 DISPLAY / LD_LIBRARY_PATH / SERENITY_***（
  `dbus-daemon` 只有 `XDG_RUNTIME_DIR`）。helper 的环境由 portal 自己补齐
  （`helper_environment()`，缺省 `/serenity/lib`、`:100`、`/res` …）。
- 调试开关 `SERENITY_PORTAL_PICKER=/path/to/script`：把 helper 换成打印一行路径的
  脚本，不点鼠标就能验证整条链路（本轮就是靠它把协议层验完的）。

### 15.3 验证

| 检查 | 结果 |
|---|---|
| 协议（假 helper） | `gdbus` 调 `OpenFile` 拿 handle；`dbus-monitor` 看到 `Response(0, {uris:[file:///home/serenity/build.log]})` ✅ |
| 取消 / 失败 | helper `exit 1` → `Response(1)`；`exit 3` → `Response(2)` + portal 日志 ✅ |
| 选项解析 | `handle_token`（返回路径后缀一致）、`current_name`、`current_folder`(ay)、`filters`(a(sa(us))) 全部生效 ✅ |
| 按 GTK 顺序裸调用 | Gio 客户端「先拿 handle 再订阅」收到 `RESPONSE 0 {'uris': [...]}` ✅ |
| 真实 GTK4 应用 | WordCraft 点「浏览…」→ 我们的原生对话框弹出，**带上它自己的过滤器**（`Documents\|*.docx,...`、`All files\|*`）✅ |
| 原生对话框 | 标题、位置、文件名、打开/取消全中文；选中文件后「打开」变可用 ✅ |
| D-Bus 激活 | 杀掉 portal 后 `gdbus` 一问就自动拉起，`version` 返回 3 ✅ |
| 桌面整体 | 重启会话后 WS/桥/shell/portal 全部 active，无崩溃记录 ✅ |

**未做实弹闭环**：在原生对话框里点「打开」后 GTK 端打印 URI ——
这台 VM 的 VNC 绝对坐标点击有明显漂移（点 `build.log` 会落到 `Neolmaging`），
输入不可靠。该链路的每一段都单独验证过（helper 输出、portal 响应、
Gio 客户端按 GTK 顺序收到 URI），剩下的只是把鼠标点准。

**部署物**：`/serenity/bin/{serenity-portal,serenity-file-picker,serenity-wlroots-host}`、
`/serenity/libexec/serenitid.real`（均留 `.bak-20261109`）、`/serenity/res/locale/zh-CN.ini`。

### 15.4 后续可选

- `parent_window` 走 xdg-foreign importer，让原生对话框真的贴在 GTK 窗口上
  （现在 export 通了但没 import，居中显示）。
- `OpenFiles` 多选：现在退化成单选。
- 镜像 `OpenURI`（在文件管理器里显示 / 用默认程序打开），GTK4 的「Show in Folder」需要。

### 15.5 二轮修复：为什么「对话框出来了，Linux 应用还是没反应」（20:15–20:40）

用户实测反馈点「浏览…」没反应。排查发现两个**真正**的阻断，都在 portal/会话层，
跟桥、跟 helper 都无关：

#### (1) Response 必须**单播**

实测 GTK 只给 portal 装了一条 `NameOwnerChanged` 的 AddMatch 规则，
**完全没有给 `Request.Response` 装 match**（`dbus-monitor --session` 逐条核对）。
我们原来用 `sd_bus_message_new_signal()` 发的是 `destination=(null)` 的广播信号 —— 没有
match rule 的客户端根本收不到，于是「原生对话框关掉了，GTK 还一直等」。
真实 xdg-desktop-portal 发的是带 destination 的单播信号。

修法：`PickerRequest` 记下 `sd_bus_message_get_sender(call)`，emit 前
`sd_bus_message_set_destination(signal, request->sender)`。

修完后**端到端闭环通过**（helper 用 `--auto-pick` 直接返回路径，无需鼠标）：

```
GTK3: URI: file:///home/serenity/build.log
GTK4: URI: file:///home/serenity/build.log
```

#### (2) portal 必须**随会话常驻**，不能只靠 D-Bus 激活

GTK 用 `G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START` 建
`org.freedesktop.portal.Desktop` 代理：应用启动那一瞬间若这个名字没人占，
GTK 就**永久**退回自带的 GtkFileChooser（在这个桌面上就是「点了没反应」）。

修法：serenitid 在写完用户级 `.service` 掩码后，
`make_child("portal", "serenity-portal", {}, true)` + `always_restart`。
之前反复调试时我杀掉 portal 又手工用不同环境变量重启，
serenitid 的自动重启会立刻把名字抢回去 —— 这造成了很多「假 helper 没生效」的假象，
一度误判成「GTK 收不到响应」。

#### 顺带修的两点

- `serenity-file-picker` 改成自己 `FilePicker::construct()`（不再用静态
  `get_open_filepath()`），把**文件名框回车**接到「打开」按钮 —— 上游
  `filename_textbox` 没有 `on_return_pressed`，手输路径按回车毫无反应，很像卡死。
  （本 VM 的 VNC 回车键注入本身不可靠，这一条未做实弹确认，真机上再看一次。）
- helper 增加 `--auto-pick PATH` 调试后门（与 `SERENITY_PORTAL_PICKER` 配合），
  不碰鼠标即可跑完「请求 → 响应 → 客户端拿到 URI」整条链路；
  同时在 `start / gui connected / showing picker / picker returned` 四处打 stderr 日志，
  排「对话框没弹出来」时能一眼看出卡在哪一步。

#### 本轮排查留下的判断依据

| 现象 | 判断 |
|---|---|
| helper 进程在、屏幕上没有窗口 | 先看 `/tmp/serenity-portal-picker.log`；卡在 `Application::create` 前通常是 WS 刚重启、portal socket 还没建好，重发一次请求即可 |
| 对话框在、也输入了路径，应用无反应 | Response 是广播 / 客户端没订阅 → 单播修 |
| 点「浏览…」连对话框都不弹 | portal 名字在应用启动时无人占 → serenitid 常驻修 |
| 页面标题英文但按钮中文 | 翻译表缺键（`translate_ui` 精确匹配），见 §13 |

**最终状态**（20:40）：portal 由 serenitid 常驻、单播 Response、桥带 xdg-foreign v1/v2、
`GTK_USE_PORTAL=1`、真实 helper ELF 已还原、桌面 `active`、无残留测试窗口。

---

## 16. gedit / WordCraft「另存为」没反应：老式 GtkFileChooserDialog 的覆盖问题

### 16.1 诊断（21:00–21:20）

`GTK3/GTK4 另存为` 走 portal 的端到端测试是**通的**（§15.5），但 gedit 点
「打开 / 另存为」完全没有 portal 调用。逐步排查：

| 检查 | 结果 |
|---|---|
| gedit 环境 | `GTK_USE_PORTAL=1`、`GDK_BACKEND=wayland,x11`、`DISPLAY=:100` 全有，不是环境变量问题 |
| gedit 版本 | 48.1，链接系统 `libgtk-3.so.0`；`libgedit-48.1.so` 里有 `gtk_file_chooser_native_new` |
| 实际弹窗 | 截图里是带 **Character Encoding / Line Ending / All Text Files** 的 gedit 定制框，任务栏另有 "Open Files" / "另存为" 两个窗口 —— 这是 **`GtkFileChooserDialog`（应用内 widget）** |
| WordCraft | `wordcraft-0.16.0-...AppImage`，107 MB，内含 Electron 特征，无 libgtk/GTK_USE_PORTAL 痕迹，属于另一套 dialog 实现 |

**结论**：`GtkFileChooserDialog` 根本不经过 Native 路径，`GTK_USE_PORTAL=1` 对它无效。
xdg-desktop-portal（无论标准前端还是我们的 serenity-portal）只能覆盖
`GtkFileChooserNative`：GTK4 默认走，GTK3 要 `GTK_USE_PORTAL=1`，而
gedit 这类应用两者都不占。要覆盖它们，**必须动 GTK 侧**。

### 16.2 用户问：「能不能源码定制一个 GTK portal 指向我们的」

**可以，而且这是覆盖 gedit 的唯一通用办法。** 三条路线：

| 路线 | 做法 | 代价 |
|---|---|---|
| A. 改 GTK3 源码（用户设想） | 在 `gtk/gtkdialog.c` 的 `gtk_dialog_run()` 或 `gtk/gtkfilechooserdialog.c` 里加「Serenity 模式」：不显示 widget， fork/调我们的 picker，`gtk_file_chooser_set_filename()`，直接返回 `GTK_RESPONSE_ACCEPT`；重编 `libgtk-3.so.0` 覆盖系统库 | 最彻底，所有链接系统 GTK3 的应用通吃；但要维护 GTK 分支、系统升级要重编、ABI 要保（只加代码不改结构则兼容）；AppImage 自带 GTK 覆盖不到 |
| B. GTK_MODULES 运行时模块（本轮做的原型） | `Ports/LinuxGUI/GtkChooserModule/serenity-filechooser.cpp`：官方扩展点 `GTK_MODULES`，用 `GtkWidget::show` 的 emission hook 抓住所有 `GtkFileChooserDialog` | 零 ABI 风险、可回滚、不用改系统 GTK；只能覆盖系统 libgtk-3 |
| C. LD_PRELOAD 拦 `gtk_dialog_run` | 符号级拦截，连窗口都不显示，直接返回 ACCEPT + 设置 filename | 最干净的回传路径；但要给目标进程注入 `LD_PRELOAD`（D-Bus 激活应用需 `dbus-update-activation-environment`） |

### 16.3 本轮原型验证到哪一步

- `GtkChooserModule/build.sh` 在 VM 上编译出 `libserenity-filechooser.so`，
  已装到 `/usr/lib/x86_64-linux-gnu/gtk-3.0/modules/`（**未设全局 GTK_MODULES，当前不生效**）。
- 关键坑：`gtk_module_init()` 跑得比 `GtkWidgetClass` 的 class_init 还早，
  `g_signal_lookup("show", GTK_TYPE_WIDGET)` 返回 0 → 必须先 `g_type_class_ref(GTK_TYPE_WIDGET)`。
- 带 `GTK_MODULES=serenity-filechooser` 启动 gedit，触发 `org.gtk.Actions.Activate open`：
  日志 `intercept open chooser` + `helper exit=0 path=/home/serenity/build.log`，
  **拦截与原生 picker 调用成功**，真实 helper 会弹出原生对话框。
- 中间试过走 portal + `Request.Response` 单播：GTK 的 `gtk_dialog_run()` 进嵌套主循环后，
  GDBus 的 `signal_subscribe` / `add_filter` 都收不到回调（AddMatch 装了、daemon 也投递了），
  所以改成模块直接 fork helper，避开 D-Bus 时序。
- **未解决**：把结果喂回 `gtk_dialog_run()`。`gtk_widget_hide()` 会让它直接返回
  `GTK_RESPONSE_NONE`；去掉 hide 后，`gtk_dialog_response(ACCEPT)` 在 idle / 50ms timer
  里发出，`run()` 仍不返回（最小 `Gtk.FileChooserDialog+run()` 复现，超时）。
  说明 GTK3 module 路线需要改用路线 C（LD_PRELOAD 拦 `gtk_dialog_run`）或在 A 里直接改
  `gtk_dialog_run` 的实现，才能在正确的时机返回。

### 16.4 当前环境（已恢复）

`/serenity/bin/serenity-file-picker` 已还原成正式 ELF（测试期的 `--auto-pick` 包装已撤）；
serenitid **没有**加 `GTK_MODULES`，不会把半成品注入所有 GTK3 应用；
测试用的 gedit / python 进程已清理；portal（单播、常驻）、桥（xdg-foreign v1/v2）、
`GTK_USE_PORTAL=1` 保持 §15.5 的最终状态不变。

### 16.5 GTK4 呢？（21:30 实测，回答「gtk4 的呢？」）

| 检查 | 结论 |
|---|---|
| GTK4 支持 `GTK_MODULES` 吗 | **不支持**。带 `GTK_MODULES=serenity-filechooser` + `LD_DEBUG=libs` 跑 GTK4 程序，`libserenity-filechooser.so` 加载次数 0，module 日志空 → 路线 B 对 GTK4 无效 |
| GTK4 需要定制吗 | **不需要**。`Gtk.FileDialog.save()` 实测直接发 `org.freedesktop.portal.FileChooser.SaveFile` 到我们的 portal，helper 弹原生选择器（§15.5 还验过 open/save 都拿到 `file://` URI） |
| 哪些 GTK4 覆盖不到 | 把 `GtkFileChooserWidget` 嵌进自己窗口的应用（嵌入式 widget，不是 dialog），或完全自绘 chooser 的 |
| 真要改 GTK4 源码改哪 | `gtk/gtkfilechoosernativeportal.c` 已走 portal，**不用动**；嵌入式在 `gtk/gtkfilechooserwidget.c`，异步 API 在 `gtk/gtkfiledialog.c`。GTK4 没有 `gtk_dialog_run()` 这种单一阻塞入口，全是 `GAsyncResult` 回调，重定向/拦截比 GTK3 复杂得多 |

**结论**：GTK4 侧的最佳策略就是「什么都不做」——保证 portal 常驻 +
xdg-foreign + 单播 Response（§15.5），应用只要用 `GtkFileDialog` /
`GtkFileChooserNative` 就自动落到原生选择器。要覆盖嵌入式 widget 才需要
LD_PRELOAD 拦 `gtk_file_dialog_open/save` 并伪造 GAsyncResult，或改 GTK4 源码，
成本高、收益低。

#### 顺带纠正：WordCraft 不是 GTK，也不是（典型）Electron

`strings` 对 AppImage 无效（squashfs 是压缩的），之前「内含 Electron 特征」的判断
不可靠。`--appimage-extract` 解开后真相：

- `usr/bin/wordcraft` **139 MB 自包含二进制**，`ldd` 只有 `libgcc_s / libm / libc`；
- 没有打包 `libgtk-*` / `libadwaita` / `libwebkit` / Electron 的 `.so` 或 `.asar`；
- 但 §15.3 抓到过它带 `Documents|*.docx,…` 过滤器调用我们的 portal，
  说明内部静态链接了某个 portal 客户端（Rust `rfd`/`ashpd` 一类）。

所以它的「另存为没反应」既不是 GTK3 也不是 GTK4 问题，得单独抓一次
`dbus-monitor` 看它到底发没发 `SaveFile`（本轮未做）。

#### 环境

GTK4 测试弹出的 picker 已清理，`/tmp/wcx` 解压目录已删；helper 仍是正式 ELF；
portal 常驻；桌面 `active`。
