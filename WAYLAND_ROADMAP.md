# Wayland 协议缺口与零拷贝路线图

> 建立于 2026-11-04。本轮只做**代码事实核对 + 影响分析 + 任务拆解**，不改功能代码。
> 后续每一项**单独开新对话**执行，按 §6 顺序推进；每项自带目标、改法、验收方法、回滚。
> 行号为 2026-11-04 工作树快照，定位以**函数名/符号名**为准。
>
> **进度更新（2026-11-04 15:10）**：阶段 0 **全部完成**（0.1 清理 / 0.2 帧节流 / 0.3 damage / 0.4 回归，§3.5 §3.7），
> 阶段 **1.1 `xdg_popup.grab` 完成**（§3.8），装饰策略为强制 SERVER_SIDE（§3.6），构建约定已放宽（§7）。
> **下一步候选**：① popup **定位**（菜单现在出现在屏幕左上角，不是锚点旁）② Firefox「菜单打开时点窗口内会崩」
> ③ 点桌面/任务栏不关菜单 ④ 1.2 output 事件现代化。§5 A″ 是"WS 直收客户端 shm"的中间态方案，可在 1.x 之后评估。

---

## 1. 前提修正（务必先读）

**目标平台是真机，VM 只是前期测试环境。** 这条前提推翻了 2026-11-04 早前几轮在 VM 语境下给出的三条结论：

| 早前结论（VM 语境） | 真机目标下的修正 |
| --- | --- |
| "wlroots 塞不进来、别动底层" | **部分撤回**：输出端确实是 WindowServer（不是 wlroots 能接管的 KMS），但**零拷贝该做**，落点在 `LinuxScreenBackend` 而非替换合成器 |
| "重量级应用走 X11 桥（`MOZ_ENABLE_WAYLAND=0`）" | **撤回**：真机上是性能倒退。X11 桥只是 VM 期的可用性拐杖，终局应让原生 Wayland 链路跑通 |
| "零拷贝收益小" | **撤回**：真机上每帧整屏 memcpy（1920×1080≈8MB，60fps≈500MB/s 内存带宽）是实打实的开销 |

项目定位保持不变（见 `~/.workbuddy/memory/MEMORY.md` 与 `STATUS.md`）：**界面复古、底层现代**；优先成熟组件，力气花在外观层；**只有当底层改动带来可测量收益时才动**。本文档所有 P0 判定都以此为准。

---

## 2. 架构与真实数据流

`Ports/LinuxGUI/WaylandHost/wayland_host.cpp` 不是完整合成器，而是**桥**：把每个 Wayland 客户端的 shm 缓冲拷成 `Gfx::Bitmap`，塞进 `GUI::Window`，由 **WindowServer**（LibGUI CPU painter）做真正的多窗口合成与复古边框绘制。

```
Wayland client (wl_shm buffer)
  │ commit
  ├─ bitmap_from_shm()            [拷贝 1] shm → Gfx::Bitmap（每次 commit 全缓冲）
  ├─ SurfaceWidget::set_frame() / draw_scaled_bitmap()
  ▼
GUI::Window → WindowServer 合成     [拷贝 2] 窗口位图 → m_framebuffer（双缓冲）
  │
  ├─ LinuxScreenBackend::present_buffer_kms()
  │    逐行 memcpy                 [拷贝 3] m_framebuffer → DRM dumb buffer mmap
  ▼
drmModePageFlip → CRTC scanout
```

输出端事实（`Ports/LinuxGUI/LinuxScreenBackend.cpp`）：

- `create_kms_presenter()` (≈399)：`DRM_IOCTL_MODE_CREATE_DUMB` + `MODE_MAP_DUMB` + `mmap` + `drmModeAddFB`，双缓冲
- `map_framebuffer()` (≈687)：`m_framebuffer = calloc(...)` 普通内存，双缓冲用 `m_back_buffer_offset`
- `present_buffer_kms()` (≈550)：**逐行 memcpy 整屏**到 dumb buffer，再 `drmModePageFlip`（阻塞等 `flip_done`）
- `flush_framebuffer_rects()` (≈742)：KMS 路径**直接忽略 `rects`**，整屏交给 `present_buffer_kms`；只有 SDL 路径用 `SDL_UpdateTexture` 按脏矩形更新
- SDL 与 X11Presenter 只是 `presenter_should_use_drm()` 失败时的开发/回退路径

**结论**：用户提出的"在 WindowServer 输出层做 DRM 零拷贝"是**比改 wlroots 更贴合架构**的切入点——所有客户端（原生 Wayland / X11 桥 / Wine / 内置应用）最终都汇入 `m_framebuffer`，在这一层消拷贝对全体生效，且不碰桥的协议语义。详见 §5。

---

## 3. 现状事实表（已核对，非推测）

### 3.1 已注册的全局（共 14 个，`serenity_main()` ≈5019–5045）

`wl_compositor`(4) · `wl_shm`(1) · `wl_seat`(7) · `zwp_pointer_constraints_v1`(1) · `zwp_relative_pointer_manager_v1`(1) · `wl_output`(3) · `wl_subcompositor`(1) · `xdg_wm_base`(5) · `zxdg_decoration_manager_v1`(1) · `wp_viewporter`(1) · `zwp_text_input_manager_v3`(1) · `org_kde_kwin_server_decoration_manager`(1) · `wl_data_device_manager`(3)

另有 Xwayland 集成（`setup_xwm_callbacks`）、fcitx5 + serenityime IME 前端、InputBridge。

### 3.2 清单里各项的真实状态

| 项 | 代码事实 | 状态 |
| --- | --- | --- |
| damage 跟踪 | `surface_damage` / `surface_damage_buffer` 均为空函数 (1283–1284) | ❌ 无 |
| 帧回调节流 | ~~`surface_frame()` 立刻 `wl_callback_send_done(cb, 0)` 并 destroy~~ → 改为 16ms 节流队列 + 单调递增时间戳（§3.5） | ✅ 11-04 已实现 |
| `xdg_popup.grab` | `.grab = [](...){ }` 空实现 (2080) | ❌ 无 |
| dmabuf / wl_drm | 全局表无；文件头注释自认 "Not implemented (yet): dmabuf"；`wl_shm` 仅 `ARGB8888`/`XRGB8888` (1171–1172, 1076) | ❌ 无 |
| output scale | `wl_output_send_scale(resource, 1)` 写死 (3433)；无 `fractional-scale-v1`、无 `xdg_output` | ❌ 固定 1 |
| 热插拔重发 | `main_linux.cpp:604` `take_display_change()` → `apply_screen_resolution()`；**WaylandHost 不重发 `wl_output` 事件** | ❌ 客户端不知分辨率变化 |
| `wl_touch` | `wl_seat_send_capabilities(... POINTER \| KEYBOARD)` (3613) | ❌ 只有两者 |
| `xdg_activation_v1` | 全局表无 | ❌ 无 |
| `zwp_primary_selection` | 全局表无（只有 `wl_data_device_manager` 的 clipboard） | ❌ 无 |
| `zwp_keyboard_shortcuts_inhibit` | 全局表无 | ❌ 无 |
| presentation-time / idle-inhibit | 全局表无 | ❌ 无 |
| 多 output | 单个 `output_global`；`Server::outputs` 是 `Vector<Output*>` 但仅记录 per-client bind | ❌ 单屏 |
| pointer-gestures / cursor-shape-v1 / tearing-control / xdg_toplevel_drag | 全局表全无 | ❌ 无 |
| shm 格式 | 仅 2 种（见上） | ⚠ 受限 |

### 3.3 本轮（2026-11-04）**已完成并部署**的改动，请勿在新对话重复做

1. `zwp_text_input_v3` 普通打字字符回显（修复 gedit 打字；仅可打印字符、非 Ctrl/Alt/Super）
2. `commit_string` 后 `done` 改用客户端最后一次 commit 的 serial（不再自造 serial）
3. commit 前无条件清 preedit
4. **shm buffer release 时机**：由"等下一次 attach 才 release 上一个"改为 **commit 消费完立即 `wl_buffer_send_release`**（修复 Firefox 缓冲池死锁：客户端池小、等 release 才复用，合成器等 attach 才 release → 互相等死）
5. **`wl_output` / `wl_surface.enter` 补齐**：
   - `Output` 增加 `client` 字段；`bind_output()` 注册进 `g_server.outputs`、destroy 时移除并清理各 surface 的 `entered_outputs`
   - 新增 `send_surface_output_enter(Surface&)`，按 client 匹配 output resource，用 `entered_outputs` 去重
   - `compositor_create_surface()` 创建即发 enter；`bind_output()` 对"先建 surface 后 bind output"的客户端补发
   - 文件头 `wl_output` 本就在 **Implemented globals** 列表里（第 19 行），早前"line 19 承认未实现"是误读，已更正

**验证结果**：编译通过、部署到 VM；`WAYLAND_DEBUG=client` 显示客户端收到 16 次 `wl_surface.enter`。但当轮 **Firefox 原生 Wayland 仍冻结**（见 §4 P0-1）—— 该冻结已由下一轮的帧回调节流（§3.5）修掉，此处保留为当时的现场记录。

### 3.4 临时诊断残留 —— ✅ 已清理（2026-11-04 13:28）

原清单（三处 `dbgln` + VM 环境/文件）已全部清掉，`grep 'COMMIT surf\|TXTECHO'` 无输出：
`wayland_host.cpp` 的 `COMMIT surf` 与两处 `TXTECHO`；VM 上 `/tmp/ime-debug`、
`systemctl unset-environment SERENITY_INPUT_DEBUG`，且 `serenity-desktop.service` 的
drop-in `debug.conf` 里 `Environment=SERENITY_INPUT_DEBUG=1` 已注释掉（`SERENITY_WL_CLIP_DEBUG`
暂留，供剪贴板调试用）。`input_debug_enabled()` / `ime_debug_enabled()` 两个门控函数保留。

### 3.5 本轮（2026-11-04 13:20–13:40）已完成并部署 VM —— 勿重复做

**0.2 帧回调节流（`wayland_host.cpp`）**：
- 新增 `struct FrameCallback { wl_resource* resource; Surface* surface; }` + 全局
  `pending_frame_callbacks()` 队列；`wl_resource_set_destructor(frame_callback_destroy)`
  在客户端提前销毁时把死资源从队列摘掉；`discard_frame_callbacks_for(surface)` 在
  `wl_surface` destructor 里丢弃该表面的待发 callback。
- `flush_frame_callbacks()`：由 8ms `pump_timer` 调用，按 `s_next_tick += 16` 的固定
  节拍（timer 迟到不累积漂移）批量 `wl_callback_send_done(cb, now)` 后 `wl_resource_destroy`；
  `now` 用 `clock_gettime(CLOCK_MONOTONIC)` 转 ms，且强制严格递增
  （`if (now <= last_timestamp) now = last_timestamp + 1`）。
- `surface_frame()` 不再同步 done，改为入队；`SERENITY_FRAME_IMMEDIATE=1` 恢复旧行为（回滚开关）。
- 顺手把旧的 `send_frame_done(Surface&)`（实际只做 `wl_buffer_send_release`）改名为
  `release_current_buffer()`，避免与 frame callback 概念混淆。
- **验收数据**（VM，`WAYLAND_DEBUG=client` 抓 firefox-esr 14s）：
  - frame 请求 **241 次 / 14s**（原 ~1036 次空转），`wl_callback.done` 时间戳相邻差
    **15/16/16/15/17/16 ms**（原恒 0）；
  - `attach` **45 次**（原 13 次）、`wl_buffer.release` **44 次** → 缓冲池循环恢复；
  - 截图确认 Firefox 原生 Wayland 不再冻结：新标签页内容、地址栏、实时下拉建议全部正常重绘。
  - **未决问题 1（Firefox 冻结 A/B）已闭环**：根因确认为帧回调无节流（请求即 done），
    不是 WebRender 停止出帧。

**装饰策略回退**：见 §3.6。

### 3.6 装饰策略改回「强制 SERVER_SIDE」（2026-11-04 13:3x，用户拍板）

用户结论：**窗口必须强制套复古边框**，不再「看客户端需不需要」；客户端自绘/Win3.1 标题栏/
混合窗口风格这些细节不用管。上午那版「尊重客户端」（CLIENT_SIDE → frameless）已回退。

- `apply_decoration_mode(Surface&, uint32_t)`：无条件 `decoration_mode = SERVER_SIDE`，
  `set_frameless(is_wine_toplevel(surface))` —— **唯一例外是 Wine**（其 Wayland 驱动自己把
  Windows 标题栏画进 buffer，套复古框会双标题栏）。
- `toplevel_decoration_set_mode()`（xdg-decoration）：忽略 CLIENT_SIDE 请求，回
  `send_configure(..., SERVER_SIDE)`（协议要求回复实际生效的模式，而非回显请求）。
- `kde_decoration_request_mode()`：同理，回 `send_mode(MODE_SERVER)`。
- `ensure_window()`：删掉 `decoration_mode == CLIENT_SIDE` 的 frameless 分支；新增
  `bool is_wine_toplevel(Surface const&)` 复用判定（`is_wine_app_id` + 无 xid + 非 subsurface）。
- `GTK_CSD=0`（`serenity_manager.cpp`）保留 —— 它正好是强制 GTK3 放弃 CSD 的手段。
- **验收**（VM 截图 13:39）：Firefox 原生 Wayland 窗口出现复古蓝框标题栏
  （`Mozilla Firefox` + 最小化/最大化/关闭），Firefox 自己的标签栏降级为普通窗口内容；
  无黑边、无层叠错乱（上午修好的 subsurface 合成 + 键盘焦点到 toplevel + ACTIVATED 让
  强制 SERVER 对 GTK CSD 布局安全）。

### 3.7 本轮（2026-11-04 14:00–14:20）0.3 damage 跟踪 —— 已完成并 VM 验收

分两段落地，各自独立可回滚。

**A. 桥内（`wayland_host.cpp`）**

- `surface_damage` / `surface_damage_buffer` 从空函数改为真正记录：矩形进 `Surface::pending_damage`，
  commit 时 `merge_pending_damage()` 合并成**一个包围盒**并 clamp 到 buffer，随后清空。
  （buffer_scale 恒为 1，surface 与 buffer 坐标同系，共用一张表。）
- **只拷脏区**：新增 `copy_shm_region(buffer, bitmap, region)`，`bitmap_from_shm()` 改为调用它拷贝全幅；
  commit 时若尺寸未变且非整幅，则**复用上一帧的 `surface->frame_bitmap`**、只拷脏区，
  不再每帧 `Bitmap::create` + 全量转换（省掉一次分配 + 一次整块转换）。
- **局部重绘**：`SurfaceWidget::set_frame_partial()` / `set_overlay_partial()` 把脏区映射到 widget 坐标后
  `update(rect)`，WindowServer 只重画这一块；缩放/裁切帧自动回退整幅 `update()`。
- **安全回退**：以下任一情况走旧的整幅路径 —— 无 damage（客户端没说）、viewport/crop 变化、
  `source_rect` 不等于整个 buffer（非 1:1，坐标对不上）、buffer 尺寸变化、`SERENITY_DAMAGE_DISABLE=1`。
- 统计：`SERENITY_DAMAGE_STATS=1` 时每 2s 打 `DMG commits/partial/full/copied/full_eq`。

**B. 输出侧 KMS（`LinuxScreenBackend.cpp`）**

- `flush_framebuffer_rects()` 的 KMS 分支不再整屏交：新增 `present_buffer_kms_rects()`，
  逐 rect 逐行 memcpy 到 scanout buffer，翻转动作抽成 `flip_kms()` 与整幅路径共用。
- **双缓冲同步**（关键，容易漏）：flip 到 B 后，A 缺的是"这一帧写进 B 的所有脏区"。
  故维护两张表：`pending_sync`（下一个要写的 buffer 缺什么）+ `accum_sync`（本帧写进去的），
  在 `flip_kms()` 里 `pending = accum`。一帧内多次 flush 也能正确结转；超过 16 个 rect
  置 `pending_full_sync`，下一次整幅拷贝兜底。
- 曾用"只记上一帧 rects"的简化版，一帧多次 flush 时会漏掉前一组的 rect（另一块 buffer 出现残影），
  已按上表改正。

**验收（VM，1600×840）**

| 场景 | 桥内拷贝量 / 整幅等价 | 说明 |
| --- | --- | --- |
| 空闲（光标闪烁） | 每次 commit 仅 **几百 px**（统计显示 0k，<1000） | 原先每次 760k px |
| gedit 连打 5 个字符 | **33%**（1260k / 3803k） | 含新窗口首帧 |
| 地址栏输入 3 字符 | **7%**（51k / 705k） | 小更新 |
| about:support 首屏 | 79–100% | 整页重绘，本就该整幅 |
| 连续 PageDown 滚动 | 72–100% | 视口整体变化 |

- 画面正确性：`about:support`（表格/高亮/长文本）、连续翻页滚动、gedit 打字、
  Firefox+gedit 叠加/遮挡重绘均无残影、无撕裂、无错位；桌面 wallpaper/任务栏正常。
- 过程中踩到并修掉的 bug：`Gfx::Rect::right()/bottom()` 是**排他**边界（`x+width`），
  循环里用 `<=` 会多算一行 → `Bitmap::scanline()` 断言 `y < physical_height()` 直接把桥打挂
  （表现为 Firefox 一起来就 `Broken pipe`）。**这是本端口改位图拷贝的通用坑。**
- 回滚：`SERENITY_DAMAGE_DISABLE=1` 让桥恢复整幅拷贝（输出侧改动可单独 git revert，
  行为只是多拷，无正确性风险）。

### 3.8 阶段 1.1 popup：grab / 定位 / 焦点（已完成，按 mutter 50.5 对齐）

**参照实现**：本机 `/mnt/user/rdp/mutter-50.5`：
`src/core/place.c: meta_window_process_placement()`、`src/core/constraints.c: try_flip_window_position()`、
`src/wayland/meta-wayland-popup.c`（`popup_grab_get_focus_surface` / `popup_grab_release`）、
`src/wayland/meta-wayland-xdg-shell.c`（`xdg_popup_grab` / `xdg_popup_reposition` /
`finish_popup_setup` / `meta_wayland_xdg_popup_configure` / `dismiss_invalid_popup`）。

**定位（原来算错的地方）**

- `anchor`/`gravity` 是**组合枚举 0–8，不是位标志**，旧代码用 `&` 判，算出 `configure(-99,-173)`
  （Firefox File 菜单，anchor=BOTTOM_LEFT=6、gravity=BOTTOM_RIGHT=8、size=283×394），菜单被压到屏幕左上角。
- 现在按 mutter `meta_window_process_placement()` 逐条抄：先在 anchor rect 上取点（left/right/center，
  top/bottom/center），再按 gravity 平移（**gravity LEFT → x -= w；gravity RIGHT → x 不变**，
  旧代码这里反了；垂直方向 gravity TOP → y -= h），最后加 offset。
- 约束调整按 mutter 的 FLIP 语义：放不下时**同时翻转 anchor 边和 gravity 边**（换到锚点另一侧），
  受 `constraint_adjustment` 的 FLIP_X/FLIP_Y 位控制（该字段以前被丢弃，现在存下来了），最后再贴屏幕滑动。
- `xdg_popup.reposition` 从空实现补上：保存新 positioner → `repositioned(token)` → 重算并补发
  `xdg_popup.configure` + `xdg_surface.configure` → 已有窗口跟着挪（协议要求每个 reposition 都要回 configure）。

**grab / 焦点（按 mutter 的隐式 grab 模型，16:10 二次修正）**

- mutter `xdg_popup_grab` **只把 seat/serial 记进 setup 状态**（`meta-wayland-xdg-shell.c:621`），
  真正的 grab 在 `finish_popup_setup()`（popup 映射时）创建；且只有 `setup.grab_seat != NULL`
  的 popup 会 `meta_window_focus()`，**非 grabbing popup 是 `window->input = FALSE`**
  （源码注释：「keyboard focus semantics for non-grabbing xdg_wm_base popups is pretty undefined …
  subsurfaces never receive keyboard focus, so it makes sense to do the same」，见 bugzilla 771694#c24）。
- 实测（`POPUP create` / `POPUP grab` 调试日志 + 全新 profile）：**Firefox 的 XUL 菜单根本不调
  `xdg_popup.grab`**；gedit/GTK 菜单会调。所以第一轮"给所有 popup 在映射时补 `enter`"是错的，
  已收敛为：映射时只有 `toplevel || grab_active` 才取焦点。开关 `SERENITY_POPUP_FOCUS_ALL=1`
  可恢复旧行为做 A/B。
- **键盘焦点是 seat 级状态，不能逐键事件重算**（本轮最关键的修复）：旧 `keyboard_key()`
  对每个键事件调 `keyboard_focus_surface(*surface)`，而 `surface` 取自"当前 LibGUI 焦点窗口"。
  F10 按下打开菜单 → popup 窗口获得 LibGUI 焦点 → **F10 抬起**就把
  `wl_keyboard.leave(toplevel)` / `enter(popup)` 发出去，在同一次按键的 press/release 之间换焦点。
  mutter 从不做这种事。现在 `keyboard_key()` 只按 `current_keyboard_focus_resource()` 选 client
  投递（`wl_keyboard.key` 不带 surface 参数，客户端按最近一次 `enter` 的 surface 处理），
  焦点变更只发生在 press / map / popup-dismiss 三处。
- **ACTIVATED 归属**：新增 `activation_toplevel_for()`，沿 subsurface 链 + xdg parent 链解析出
  拥有该焦点的 `xdg_toplevel`；解析不到（cursor、孤立 subsurface）就**保持现有激活**，
  绝不把 `g_activated_toplevel` 指向没有 xdg_toplevel 的 surface。旧代码因此给主窗口发过
  `ACTIVATE id=38 active=false`（菜单开着时窗口自认失焦）。
- `xdg_popup.grab` 里那次即时 `keyboard_focus_surface()` 也删了：那一刻 popup 还没有 buffer，
  给它发 `enter` 是把客户端没准备好的 surface 塞进键盘状态机。
- 外部按下关菜单：`dismiss_grabs_for_press()` 按 xdg 父链判断是否点在 popup/后代，否则
  `dismiss_popup_tree()`（先子后父）发 `popup_done`、焦点还父窗口，**并吞掉该次按键**（含 release）。
  （mutter 在 *release* 时判定并吞掉 release，且只对"别的 client / 无 surface"强制 dismiss；
  同 client 的父窗口由客户端自己关菜单。我们目前按 press 处理并吞掉整对，行为一致、实现更简单。）

**顺手修掉的悬垂指针**：`zwp_text_input_v3` 的 `focused_surface` 从不在 surface 销毁时清理，
下一次 `ime_sync_focus()` 会给已销毁的 `wl_surface` 发 `leave()`（我们侧 UAF、客户端侧收到野 id）。
现在 wl_surface 析构里调 `ime_forget_surface()`。

**验收（VM）**

| 场景 | 结果 |
| --- | --- |
| Firefox F10 菜单 | 落在 File 按钮下方、窗口左边缘对齐（mutter 算法） |
| Firefox 菜单 ↓↓ | 干净会话菜单区像素差异 0.92%（高亮确实移动；收回 popup 焦点**没有**造成导航回退） |
| 焦点序列 | `KEY ... focus=38` 从 F10 按下 → 抬起 → 两次 ↓ 全程不变；无 `KBFOCUS` 跳变、无 `ACTIVATE ... active=false` |
| Firefox「菜单开着点窗口内」 | **6/6 带断言压测全过**：每轮全新 profile、`alive=1`、`popup>=1`（`POPUP create` 日志断言菜单真弹出）、`deact=0` |
| gedit Shift+F10 | 菜单出现在文本光标处，↓↓ 导航正常（grab popup 仍取焦点） |
| gedit 点窗口内 | 菜单关闭、按键被吞（光标没跳到点击点） |

**⚠️ 方法论修正**：之前记的"改后 9 连过 / 12/12 全过"是**假阳性**——反复 `pkill -x firefox-esr`
让 Firefox 弹出 "Open Firefox in Troubleshoot Mode?" 恢复对话框，F10 根本没碰到菜单，
而 `pgrep` 仍然报活着。现在每轮用**全新 profile**，并在 `xdg_surface_get_popup` 加了
`POPUP create` 调试日志做存在性断言；所有"菜单没崩"的结论必须以这个为前提。

**仍存**：① 菜单关闭后偶见 popup 窗口残留成**未绘制黑块**（客户端 unmap / attach NULL buffer
时我们的窗口未隐藏，疑似 `hook_input_forward` 里窗口生命周期处理不全），待查；
② 点桌面/任务栏不关菜单（那是 shell 的窗口，按键不经过桥的 widget 钩子）；
③ 我们按 press 吞键、mutter 按 release 判定且只对"别的 client / 无 surface"强制 dismiss，
语义仍有细微差别（尚未观察到客户端异常）；
④ `dismiss_invalid_popup`（popup 与 parent 不相邻时发 `popup_done`）、
`meta_wayland_seat_can_popup` 的 serial 校验、`NOT_THE_TOPMOST_POPUP` 尚未实现。

---

## 4. 逐项影响评估（真机视角，回答"这些有没有影响"）

严重度：**P0** = 影响可用性/性能基线；**P1** = 影响具体功能或特定场景；**P2** = 锦上添花/特定硬件才有意义。

### 4.1 P0

| 项 | 真机影响 | 关键判断 |
| --- | --- | --- |
| **damage 跟踪 + 帧回调节流** | **最高**。~~当前无 vsync、无脏区~~ → **两项都已完成（§3.5 帧节流、§3.7 damage）**：空闲 commit 从整幅拷贝降到几百 px | **已实测为 Firefox 冻结的直接原因**：补完 `wl_surface.enter` 后，Firefox 仍以 ~9ms 间隔发 **1036 次 frame 请求 / 1057 次 commit，却只 attach 13 次内容帧**，画面停在首帧；IM 日志显示 `set_surrounding_text cursor=3`，即**输入已到达文本上下文，只是画面不重绘**。另注意 damage 有两层：Wayland 侧 `surface_damage`（省拷贝 1 的一部分）与输出侧 `flush_framebuffer_rects`（KMS 下忽略 rects，省拷贝 3），需分开做 |
| **`xdg_popup.grab`** | 高。下拉/右键菜单点外部不消失、popup 抢焦点行为错 | 实现量小（grab serial/seat 记账 + 外部按下时 `popup_done`），性价比极高，建议与 P0-1 同批 |
| **dmabuf / wl_drm** | 高但**依赖输出层改造**。所有客户端只能 shm + memcpy；mpv / Chromium(`--ozone-platform=wayland`) / GTK4 GL 全软件回退，大窗口重绘贵、视频掉帧 | 真机终局方向，但**单独补协议没用**：我们 commit 后立即 release，桥内必须拷贝；要真正零拷贝得同时改 §5 的 A/B。建议排在 §5 A 之后 |
| **缩放（scale 固定 1 / 无 fractional-scale-v1）** | 高。真机 HiDPI 屏很常见：字极小或整屏拉伸糊 | 改动横跨 `window_geometry`、输入坐标、位图尺寸、字体 DPI，且与"热插拔重发 mode / 多 output"强耦合 → 建议合并为一个"output 现代化"任务 |

### 4.2 P1

| 项 | 真机影响 | 备注 |
| --- | --- | --- |
| `xdg_activation_v1` | 中。Firefox 下载完成、点链接开新窗不弹前 | 依赖焦点模型；本桌面焦点由 WindowServer 管，实现时要把 activation token 转成 `GUI::Window` 激活 |
| `zwp_primary_selection` | 中。Linux 用户中键粘贴高频操作 | 已有 `wl_data_device_manager` 剪贴板桥，照同一套机制复制即可，实现量小 |
| `zwp_keyboard_shortcuts_inhibit` | 中。Wine 游戏、远程桌面/VNC 抓不到快捷键 | 真机游戏/远程场景才有意义；需与 InputBridge 的快捷键拦截配合 |
| `wl_touch` | 取决于目标硬件：触摸屏/二合一设备是 P1，纯台式机是 P2 | seat capabilities 加 `TOUCH` + touch 事件转发；InputBridge 需先能拿到触摸事件 |
| presentation-time / idle-inhibit | 中低。播放器帧时序不准、无法禁熄屏 | **必须先有帧回调节流**才有意义（否则时间戳无基准），排在 P0-1 之后 |

### 4.3 P2 —— 直接回答"这些有没有影响"

| 项 | 有没有影响 | 说明 |
| --- | --- | --- |
| **多 output / xdg_output / 热插拔重发** | **有，且真机比 VM 更需要**（P2 里唯一该上调的） | 真机外接显示器、改分辨率很常见。现状 `take_display_change()` 只改 WindowServer 分辨率，**不通知 Wayland 客户端** → 布局/缩放可能错乱。建议上调 P1，与 scale 合并做 |
| `pointer-gestures` | 基本无 | 触摸板捏合/滑动手势；笔记本才有意义，且可由 libinput 输入层单独接，不依赖 Wayland 协议先行 |
| `cursor-shape-v1` | 很小 | GTK4/libadwaita 新应用会用它声明语义光标；不实现时客户端回退到 `set_cursor` 传位图，**功能不坏**，只是形状可能不精确 |
| `tearing-control` | 现在无，将来才有 | 允许客户端请求异步/撕裂翻转以降延迟；但 `present_buffer_kms()` 目前**阻塞等 `flip_done`**，实现了也拿不到收益 → 必须先做异步 page flip（依赖 P0-1 / §5 A） |
| `xdg_toplevel_drag` | 很小 | 复古边框由 WindowServer 自己处理拖拽，Wayland 原生 toplevel drag 用不上 |
| **shm 仅 2 种格式** | 中低，但真机比 VM 明显 | 多数客户端用 `ARGB8888`/`XRGB8888` 没问题；但 10-bit/HDR、部分视频软件路径会请求 `XRGB2101010`、`ABGR` 等，当前直接 `post_error` 拒绝 → 客户端回退或失败。加格式不难（改 `bind_shm` 的 `send_format` + `bitmap_from_shm` 像素转换），但色彩转换要正确 |

---

## 5. 零拷贝专题（回答"WindowServer 里实现 DRM 0 拷贝"）

分三档，**推荐顺序 A → A′ → B/C**：

### A. 输出端零拷贝（推荐第一步，改动集中、风险可控）

**做法**：让 WindowServer 的 `m_framebuffer` **直接指向 KMS dumb buffer 的 `mmap`**，删掉 `present_buffer_kms()` 里的逐行 memcpy。

- 涉及文件：`Ports/LinuxGUI/LinuxScreenBackend.cpp`（`map_framebuffer()` / `create_kms_presenter()` / `present_buffer_kms()` / `flush_framebuffer_rects()`）
- 要点：
  1. `map_framebuffer()` 不再 `calloc`，改为用 `state.buffers[i].map`；双缓冲切换时 `m_framebuffer` 指针/`m_back_buffer_offset` 跟着切
  2. `flush_framebuffer_rects()` 的 KMS 分支保留 rects 语义（要么按脏区提交，要么在直接画进 dumb buffer 后不再需要拷贝）
  3. 页翻转期间不能写正在扫描的那块 buffer —— 双缓冲已具备，但要确认 `flip_done` 等待逻辑不引入新死锁
- **唯一 trade-off**：dumb buffer 是 WC/uncached 内存，CPU painter 直接画（尤其读改写、alpha blend）可能**比在 cached 内存里画更慢**。这正是它**必须和 damage 跟踪（P0-1）配套**的原因：省了拷贝 3，却可能把合成拖慢。
- 可测量收益：每帧省 `width × height × 4` 字节 memcpy。**VM 里能验证正确性**（dumb buffer / pageflip 现成），**性能收益只能真机测**。
- 回滚：保留 `calloc` 路径，用环境变量（如 `SERENITY_KMS_ZEROCOPY=0`）做 A/B 开关。

### A′. 桥内零拷贝（不建议先做）

当前为了修 Firefox 缓冲池死锁，改成 **commit 后立即 release**，因此桥内**必须**把 shm 拷成 `Gfx::Bitmap`。想复用客户端内存就得把 release 推迟到"这一帧真正呈现完"，这会重新引入缓冲池压力，需要 3 缓冲或异步释放。风险高、收益只是省拷贝 1 —— 排在 A 之后，且要有 A/B 数据再决定。

### A″. WS 直接消费客户端 shm（「把 wayland-host 合进 WindowServer」的中间态）

背景：2026-11-04 讨论过是否把 `serenity-wayland-host` 整个合进 WindowServer。结论是**方向对、顺序不对**——
真要做，工作量不在搬文件，而在给 WS 加一种新的窗口内容源（现成的位图 + damage 矩形，而不是"按需 paint"）。

现状每个客户端帧的像素旅程（1600×840 ≈ 5.4 MB/次，60fps 约 320 MB/s/copy）：

1. 客户端 shm → 桥内 `Gfx::Bitmap`（格式转换，**已按 damage 局部化**）
2. `Bitmap` → 桥的 `GUI::Window` backing store（`paint_event` 里的 `draw_scaled_bitmap`，也已按 damage 局部化）
3. WS 合成 → `m_framebuffer`（WS 自己的脏区）
4. `m_framebuffer` → KMS dumb buffer（**已按 damage 局部化**）

合进程能省掉 1、2 两次搬运和一次 paint 往返，输入链也少两跳（WS→桥→客户端 变成 WS→客户端）。
但代价是：崩溃域合并（现在 wl/xwm 段错误只死桥，桌面还在）、调试日志混流、回滚粒度变粗（只能整包回退）、
且 `wayland_host.cpp` 自带 `wl_event_loop`，要与 WS 的 `Core::EventLoop` 合并成单线程。

**因此建议的做法**（保留双进程，先拿到八成收益）：

- 给 WS 加一条"窗口内容来自外部"的通道：`RegisterForeignSurface(window_id, shm fd, size, stride, format)` +
  `SetForeignDamage(window_id, rects[])` + `CommitForeignSurface(window_id)`。
- WS 侧把它当成一种 `Window` 内容源，合成时直接 blit 客户端映射，不再等 paint 请求。
- **必须同时把 buffer release 推迟到"这一帧合成完"**（现在是 commit 后立即 release）——这正是 §5 A′ 的
  正确版本，也是唯一实质难点：需要 3 缓冲或 fence/异步释放，否则 Firefox 的两缓冲池会重新死锁。
- 验证方式同 §3.7：`SERENITY_DAMAGE_STATS` 的 copied 像素数应再降一半左右（省掉第 2 次拷贝）。

**什么时候才真的合进程**：要做 §5 B（dmabuf/GBM/KMS plane 直贴）时。那时要共享 BO、fence、把客户端缓冲直接挂
plane，跨进程描述符与生命周期管理才是主要复杂度，合并才划算。

### B. dmabuf + GBM + GPU 合成（真机终局）

需要 `zwp_linux_dmabuf_v1` + GBM allocator，并让 WindowServer 具备 GPU 合成能力（或把客户端 BO 作为 KMS plane 直贴）。我们是 LibGUI CPU painter，GPU/CPU 内容混合是 wlroots 量级的工程，**必须等 §5 A 与 damage 稳定后再评估**。

### C. 单窗口 KMS plane 直贴（特例优化）

把全屏视频/游戏窗口放 overlay plane 绕过合成。需要 atomic KMS + plane 协商；与"每个窗口都套复古边框"的模型冲突，只适合全屏特例。属 P4（与既有 `atomic KMS/GBM` 待办一致）。

---

## 6. 分阶段执行清单（每项独立开新对话）

> 每一项都可单独作为新对话的 prompt；验收用 §7 的约定。
> 标记 **[VM可验]** = 虚拟机能测正确性；**[真机]** = 必须真机才有结论。

### 阶段 0 — 收尾与打通基线（先做，阻塞后续所有项）

| ID | 目标 | 状态 | 改法要点 | 验收 | 回滚 |
| --- | --- | --- | --- | --- | --- |
| **0.1** | 清理临时诊断残留 | ✅ 13:28 完成（§3.4） | 删 §3.4 列出的 3 处 `dbgln`；VM 上 `rm /tmp/ime-debug`、`systemctl unset-environment SERENITY_INPUT_DEBUG`；不改功能逻辑 | 编译通过；`grep -n 'COMMIT surf\|TXTECHO'` 无输出 | 纯删除，无风险 |
| **0.2** | **帧回调节流（P0-1a）** | ✅ 13:35 VM 验证通过（§3.5） | `surface_frame()` 不再立刻 done：维护 pending callback 队列，接到 `wl_display` 的 frame/refresh 事件或复用 8ms `pump_timer`（≈5080）按 ~16.7ms 节拍统一 `send_done(cb, 单调递增 ms 时间戳)`；surface 销毁时丢弃其 callback | **[VM可验]** `WAYLAND_DEBUG=client` 下 frame 请求降到 ~60/s（原 ~1000+/9ms 空转）；Firefox 原生 Wayland 输入 `test` 后地址栏可见文字、窗口正常重绘；CPU 占用明显下降 | 保留旧实现为 `SERENITY_FRAME_IMMEDIATE=1` 开关 |
| **0.3** | **damage 跟踪（P0-1b）** | ✅ 14:20 完成（§3.7） | 桥内记录 `surface_damage`/`damage_buffer` 并合并、复用上一帧 bitmap 只拷脏区、widget 局部 `update(rect)`；输出侧 KMS 按 rect 逐行 memcpy + 双缓冲跨帧结转 | **[VM可验]** 空闲 commit 从 760k px 降到几百 px；gedit 打字 33%、地址栏 3 字符 7%；滚动/叠加/打字均无残影 | `SERENITY_DAMAGE_DISABLE=1` 回整幅拷贝 |
| **0.4** | 回归基线 | ✅ 14:10 完成 | gedit 打字、Firefox 原生 Wayland 地址栏输入、开始菜单"浏览器"(X11 桥) 打字三者都正常 | gedit 打出 `abcdef`、Firefox 地址栏显示出键入内容；X11 桥由用户确认可用 | — |

### 阶段 1 — 交互正确性与 output 现代化

| ID | 目标 | 改法要点 | 验收 |
| --- | --- | --- | --- |
| **1.1** | `xdg_popup.grab` | ✅ 完成（§3.8）：grab 记账 + 键盘焦点给 popup + 外部按下 `popup_done`（吞噬该次按键）+ 简单位置修正（枚举解码 + FLIP/SLIDE） | gedit 菜单：Shift+F10 打开、↓↓ 导航、点窗口内即关且按键被吞；Firefox 菜单落点正确 |
| **1.2** | output 事件现代化 | 分辨率变化/热插拔时向所有已 bind 的 `wl_output` 重发 `geometry/mode/scale/done`；从 `take_display_change()` 打通到 WaylandHost | 改分辨率后客户端收到新 mode 并重配 **[VM可验]** |
| **1.3** | 多 output + `xdg_output` + scale | 拆掉单个 `output_global`；实现 `fractional-scale-v1` 与 `xdg_output`；同步修 `window_geometry`、输入坐标、字体 DPI | HiDPI 下文字清晰、坐标不偏 **[真机]**（VM 只验逻辑） |

### 阶段 2 — 零拷贝 A

| ID | 目标 | 改法要点 | 验收 |
| --- | --- | --- | --- |
| **2.1** | `LinuxScreenBackend` 直接画进 dumb buffer | 按 §5 A；加 `SERENITY_KMS_ZEROCOPY` A/B 开关 | **[VM可验]** 画面正确、无撕裂；**[真机]** 对比开关前后 CPU/帧率/带宽，确认 WC 内存不拖慢合成 |
| **2.2** | 异步 page flip | 去掉 `present_buffer_kms()` 里阻塞等 `flip_done` 的轮询，改事件驱动 | 为 tearing-control / presentation-time 铺路 **[VM可验]** |

### 阶段 3 — P1 协议批（彼此独立，可并行开对话）

- **3.1** `zwp_primary_selection`（照 `wl_data_device_manager` 桥复制，量最小，建议先做）
- **3.2** `xdg_activation_v1`（activation token → `GUI::Window` 激活）
- **3.3** `zwp_keyboard_shortcuts_inhibit`（与 InputBridge 快捷键拦截配合）
- **3.4** `wl_touch`（seat capabilities + 触摸事件转发；**取决于目标硬件是否带触屏**）
- **3.5** presentation-time + idle-inhibit（依赖 0.2 / 2.2）

### 阶段 4 — GPU 路径（真机终局，最重）

- **4.1** `zwp_linux_dmabuf_v1` + GBM allocator
- **4.2** WindowServer GPU 合成 / KMS plane 直贴（对应既有 `atomic KMS/GBM` 待办）
- 前置：阶段 0/1/2 全部稳定；**开工前必须先在真机拿到 §5 A 的 A/B 数据**，确认值得上 GPU

### 阶段 5 — P2 收尾

- **5.1** shm 扩展格式（10-bit/HDR）
- **5.2** `cursor-shape-v1`、`pointer-gestures`、`tearing-control`（依赖 2.2）、`xdg_toplevel_drag`
- 每项都小，但优先级最低；按真机实际硬件形态决定做哪些

---

## 7. 构建 / 部署 / 验证约定（新对话直接复用）

- **[2026-11-04 放宽] 宿主二进制可直接部署**：VM 已 `apt-get install -t sid libc6` 升到
  **2.43-7**（与宿主一致），`ldd` 无 missing。现在流程是 **宿主 `cmake --build Build/lagom`
  → `scp` 到 VM `/tmp` → `sudo systemctl stop serenity-desktop` → `rm` 旧文件 → `cp` → `start`**，
  省掉 VM 内构建。原约束「宿主产物报 `GLIBC_2.43 not found`」只对未升级 libc6 的机器有效。
  `Text file busy` 仍然是 stop → rm → cp → start 的理由。
- **源码同步铁律**：`rsync -az --delete` **整目录带尾斜杠**；禁止多文件平铺 rsync（曾覆盖 VM 的 `Ports/LinuxGUI/CMakeLists.txt`）。
- 单文件改动可用 `scp` 到 `/home/serenity/serenity/<同路径>`，再在 VM 内 `cmake --build Build/lagom --target serenity-wayland-host -j6`。
- **部署**：`sudo systemctl stop serenity-desktop.service` → `sudo rm -f /serenity/bin/serenity-wayland-host` → `sudo cp ...` → `start`（直接覆盖会 `Text file busy`）。
- **鼠标注入用 `poke:x,y`（相对增量）**：`Build/vm/vncshot.py` 的 `poke` 先 `rel(-5000,-5000)` 把指针撞到
  左上角（guest 会夹在 0,0，所以是确定性的），再 `rel(x,y)` 平移并按键。**前提是 VM 里
  `systemctl set-environment SERENITY_POINTER_FLAT_ACCEL=1`**（`LinuxInput.cpp` 对 DEVICE_ADDED 设 flat
  accel profile），否则 libinput 的加速会把第二段 delta 缩放，点偏几十像素——这正是"点了没反应"的元凶。
  RFB 绝对坐标（`vclick`）和 QMP `abs` 对相对设备都不可靠，只留作历史。
- **截图一律走 VNC**（用户明确要求，勿用 screendump）：
  `python3 Build/vm/vncshot.py "shot:/tmp/x.ppm"` → `convert /tmp/x.ppm x.png` → 读图。
  当前分辨率 1872×984；`move()/click()` 按 `SCREEN` 归一化，**点地址栏不可靠，优先用 `Ctrl+L` 聚焦**（`vmp.qmp` 发 qcode `ctrl`+`l`）。
- **协议追踪**：客户端启动时加 `WAYLAND_DEBUG=client`（日志量极大，定向 grep `enter|attach|release|frame|commit`）。
- **合成器日志**：`/tmp/serenity-session.log`；IME 诊断开关 `/tmp/ime-debug` 或 `SERENITY_IME_DEBUG=1`，输入诊断 `SERENITY_INPUT_DEBUG=1`。
- **damage 相关开关**：`SERENITY_DAMAGE_STATS=1` 每 2s 打一次
  `DMG commits=… partial=… full=… copied=…k px full_eq=…k px (…%)`（`copied/full_eq` 就是验收比例）；
  `SERENITY_DAMAGE_DISABLE=1` 让桥回退整幅拷贝，用于 A/B 与"怀疑是 damage 引起的"排查。
  两者都从 service 环境继承：`sudo systemctl set-environment …` 后 restart，排查完记得 `unset-environment`。
- **注意 VNC 点击不可靠**：QEMU 只有相对指针 + libinput 加速，`vclick` 的"先回 (0,H-1) 再绝对定位"在
  不同分辨率/加速下会漂。**验证输入优先用"新开窗口自带焦点"**（bridge 在 `mapped_once` 时给焦点）+
  键盘注入，不要依赖点击定位；`type_text` 的首字符偶尔会被吞（`Ctrl+L` 后第一个键）。
- **两条 Firefox 启动路径**（对照实验必须分清）：
  - 开始菜单「网络 → 浏览器」= `/serenity/bin/serenity-web-browser` 包装脚本，`MOZ_ENABLE_WAYLAND=0` + `DISPLAY=:100` → X11 桥 → **可用**
  - 「Linux Apps」= 继承 serenity-shell 环境（`MOZ_ENABLE_WAYLAND=1`）→ 原生 Wayland → **当前冻结（阶段 0.2 修复目标）**

---

## 8. 未决问题（新对话开工前需确认或顺手验证）

1. ~~**Firefox 冻结根因的 A/B 未闭环**~~ —— **✅ 已闭环（2026-11-04 13:35）**：改成帧节流后复测，
   frame 请求 241 次/14s、`done` 时间戳 15–17ms 递增、`attach` 从 13 → 45 次，Firefox 原生
   Wayland 正常重绘（用户确认）。根因确认为帧回调无节流，WebRender 本身没停。
2. **目标硬件形态未定**：是否带触摸屏、是否多屏、是否 HiDPI —— 直接决定 3.4 / 1.3 / 5.1 的优先级。
3. **X11 桥是否保留为 Firefox 默认路径**：VM 期它保证可用；真机终局应统一原生 Wayland，但切换时机未定。
4. **§5 A 的 WC 内存 trade-off 只有真机能测**：VM 里不要据"看起来没变慢"下结论。
5. ~~阶段 0.1 清理调试代码时，注意 `input_debug_enabled()` / `ime_debug_enabled()` 这两个门控函数本身可保留~~ —— 0.1 已完成，
   `input_debug_enabled()` / `ime_debug_enabled()` 保留，`SERENITY_INPUT_DEBUG` / `SERENITY_IME_DEBUG` 仍可用于新问题排查。
6. **damage 目前合并成一个包围盒**：客户端一帧发多个分散矩形时（GTK 常见：headerbar + 内容区），
   包围盒≈整窗，收益被吃掉（实测 gedit 单次 commit 仍是 82%）。下一步可把 `Surface::pending_damage`
   透传成最多 8–16 个矩形列表，超出再退化成包围盒；桥内拷贝与 `update(rect)` 都按列表做。
7. **不发 damage 的客户端全部整幅拷贝**：目前 Wine / 部分 Xwayland 路径没验证过是否发 damage，
   没有就自动回退整幅（安全但无收益）。要不要对这类客户端做"缓冲区 diff"来兜底，需要真机数据再定。
