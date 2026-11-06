# 项目状态总览（已完成 / 待测试 / 未完成）

> ## ⛔ 合成器只用 WindowServer（KMS），坚决不用 wlroots
>
> 2026-11-06 用户明确要求。禁止任何 `SERENITY_PRESENTER=wlroots` 的 systemd drop-in；
> 发现 `wlroots.conf` 立刻禁用。自检：日志必须是 `KMS presenter on <W>x<H>` +
> `input via libinput`。详见 `Ports/LinuxGUI/SCREENSHOT_RECORDER.md` 顶部与
> `HANDOFF_WSWLR_COMPOSITOR.md` 的否决说明。
>
> **用法**：本文件是唯一的「一页速览」索引。新会话先读这一份即可，细节再按文末「源文档索引」跳转到对应的 dated handoff。
> **生成来源**：由 `HANDOFF.md`(11-01)、`QUICK_LAUNCH_HANDOFF.md`(11-01)、`HANDOFF_LINUX_APPS.md`(11-02)、`HANDOFF_WINAPPS.md`(11-03)、`HANDOFF_WAYLAND_WINE.md`(11-03) 及 `.workbuddy/memory/2026-11-03.md` 汇总。
> **最后更新**：2026-11-05（20:2x，Vista 主题双态 VM 实测通过；文件管理器 Vista/Win7 化落地；
> 修复 KMS 双缓冲「左右扫过按钮消失」——单份帧内容 + 全量提交，代价实测 1.31 ms/帧；详见 `VISTA_THEME.md` §6.1）。

---

## 一页速览（按子系统）

| 子系统 | 结论 | 详情节 |
|---|---|---|
| 显示（DRM/KMS） | ✅ 已完成 | §1.1 |
| 输入（libinput） | ✅ 已完成 | §1.1 |
| 桌面外壳（serenity-shell 单进程） | ✅ 已完成（仅缺截图） | §1.2 |
| 62/62 应用构建 + 安装到 /serenity | ✅ 已完成 | §1.2 |
| **视频播放器（libmpv SW 渲染）** | ✅ 有声音没图像已修（根因：控件只登记进布局、没 `add_child()` 挂进控件树 → 从不绘制）；顺带修暂停/音量（改走 `mpv_set_property`） | `HANDOFF_LINUX_APPS.md` §12 |
| **「打开文件」对话框（全端口）** | ✅ 已修：`serenity-file-system-access-server` 之前**从未构建/部署**，portal socket 不存在 → 所有用 `FilePicker` 的应用 Open 要么没反应要么 VERIFY abort。已构建+部署到 `/serenity/bin`，`serenity_manager` 下次登录自动拉起；播放器实测弹出完整选择器 | `HANDOFF_LINUX_APPS.md` §12.4 |
| 汉化（UI 字符串 700+ 条） | ✅ 已完成 | §1.2 |
| 主题 / Win2000 外观 | ✅ 已完成（含 libadwaita，用户确认） | §1.4 |
| X11 兼容（X11Bridge） | ✅ 已退役，切到 WaylandHost | §1.3 |
| WaylandHost 合成器（旧桥） | ❌ 已弃用：缺 `wl_subcompositor`，Firefox 的 Wayland 后端直接崩；二进制与构建入口已移除 | §1.3 |
| **Wayland 协议层（wlroots 路线）** | ✅ 生产会话已切 `serenity-wlroots-host`；Firefox 启动 / 页面渲染 / ☰ 菜单（落点、无闪现、连点不崩、**菜单项可点，Settings 正常打开**）全链路实测通过 | §1.3 |
| **内嵌 wlroots 合成器（新路线）** | ✅ 真机桌面 + 三类窗口同屏 + 输入/光标（`SERENITY_PRESENTER=wlroots`） | `HANDOFF_WSWLR_COMPOSITOR.md` |
| Wayland 帧回调节流（修 Firefox 冻结） | ✅ 已完成（11-04 VM 验证） | §1.3 |
| Wayland damage 跟踪 + KMS 脏区拷贝 | ✅ 已完成（11-04 VM 验证，空闲降到几百 px） | §1.3 |
| xdg_popup.grab / 菜单点外部关闭 / 菜单定位 | ✅ 已完成（11-04 VM 验证，gedit 实机操作） | §1.3 |
| 窗口装饰强制复古框（回退 CSD 尊重） | ✅ 已完成（11-04 VM 验证） | §1.3 |
| 原生 Wayland 客户端输入 | ✅ 已完成（GTK4 打字实测；Firefox 原生 Wayland 11-04 已可正常重绘+输入） | §1.3 |
| 开始菜单 Linux Apps + 点击启动 | ✅ 已完成 | §1.3 |
| 任务栏快速启动栏 | ✅ 已完成 | §1.2 |
| 系统监视器 Lite | ✅ 已完成 | §1.2 |
| 输入法（fcitx5 addon + LibGUI 客户端 + GTK immodule + 设置界面） | ✅ 已完成（11-05 VM 实测：只 rime / 模糊音 / 9 候选 / 内联预编辑） | §1.4 |
| Wine 中文字体 | 🟡 可装字体，FontSubstitutes 未生效 | §2.3 |
| Terminal 增强 + 尺寸/SIGCHLD 修复 | ✅ VM 验证通过 | §1.2 / §3 |
| 退出链路（注销/关机/重启） | 🟡 代码就绪，待部署验证 | §1.6 |
| 真机（物理机）安装 | 🟡 脚本就绪（`06-install-host.sh`），未执行 | §1.6 |
| Windows 启动器（serenity-winapp 原生 C++） | ✅ 已部署；app→原版wine / game→umu-run | §1.5 |
| Windows 程序管理器（WinAppsSettings） | ✅ 已部署 VM；运行时策略已定（§1.5） | §2.5 |
| 剪贴板桥接（X↔Wayland） | 🟡 代码完成，未实测 | §2.3 |
| 系统级剪贴板（桌面内复制粘贴） | ✅ 已运行（server+socket+history） | §1.3 |
| 输入法 rime 激活 | ✅ 已完成（11-05 VM 实测） | §3 |
| **Vista 系统主题 /「关闭主题」回经典** | ✅ VM 双态实测通过（Vista 玻璃标题栏/任务栏 + Aero chrome；classic 全部回落经典灰）；切换脚本服务检测已修 | `VISTA_THEME.md` / §1.4c |
| **文件管理器 Vista/Win7 化**（命令栏/地址栏/导航窗格/预览窗格/中文菜单/搜索过滤/共享条目层） | ✅ 已完成并在 VM 实测；搜索改为「仅用户目录 + 用户显式添加目录」；标题栏不再显示路径 | §1.4d |
| **KMS 脏区提交致「左右扫过按钮消失」** | ✅ 已修：`m_back_buffer_offset=0` + 全量提交（1.31 ms/帧，7.9% 单核） | `VISTA_THEME.md` §6.1 |
| 输入法设置界面（serenity-ime-settings） | ✅ 已部署 VM；点「应用」即时生效 | §1.4 |
| 显示性能（atomic KMS/GBM、热插拔、HiDPI） | ❌ 多未做/未真机验证 | §3 |
| 托盘（音量 applet、CPU/MEM/NET 图、拖拽） | ❌ 多处缺口 | §3 |
| 清理 InputBridge / 其他优化 | ❌ 待评估 | §4 |

---

## 一、已完成（代码 + VM 实测验证通过）

### 1.1 显示 / 输入 / 合成器底座
- **显示**：DRM/KMS dumb buffer + page flip，稳定（1280×960 起，现已走 preferred 自适应）。`probe_display_size` 优先选 `DRM_MODE_TYPE_PREFERRED`（宿主窗口实时大小）。
- **输入**：libinput（udev/seat0），移动 / 点击 / 键盘全部实测通过；绝对坐标 16 位、相对指针亚像素累积、按钮补发缓存位置均已处理。
- **合成器 / 窗口管理**：Serenity WindowServer 核心未改，正常。
- **退出链路**：`restore_display()` + `_exit()`，交还 DRM master，避免 systemd 重启时 SetCrtc 失败。

### 1.2 桌面外壳 / 应用 / 汉化 / 任务栏
- **桌面外壳**：`serenity-shell` 单进程（Desktop + Taskbar，explorer.exe 模式）合并完成，AppCatalog 唯一扫描、右键菜单统一、共享 app 列表。⚠️ 当时缺截屏通道，仅进程+日志层验证（待补「桌面图标+任务栏+开始菜单」截图）。
- **应用**：62/62 app 目标全部构建成功，安装到 `/serenity`（bin/lib/libexec/res/share/etc），`/usr/local` 已清空。
- **「所有 app 打不开」根因已修**：`Userland/Libraries/LibCore/Process.cpp` 入口统一 `/bin/Name` → `serenity-<kebab>` 转换；VM 实测双击图标/开始菜单/运行都通。
- **桌面目录驱动**：图标来自 `~/Desktop` + 系统目录的 `.af`/`.desktop`，`desktop.ini` 那套已移除。
- **右键菜单 Win9x 化**：桌面空白 = 排列/行列对齐/刷新/清理内存缓存/粘贴/新建/属性；图标右键（对齐 pre-Win10）= 打开/打开方式/[游戏模式启动]/剪切/复制/**删除/重命名**/压缩(ZIP,Tar.gz)/解压/属性。删除对「用户快捷方式」删 .af/.desktop 源文件、对真实文件/文件夹走 `FileSystem::remove`（Yes/No 确认）；重命名仅对真实文件（快捷方式显示名在 .desktop 内，改名需重写 Name=，暂不做）。旧的「移除图标」由「删除」取代。压缩解压走 `Core::command`。
- **桌面标签/菜单细节（11-03）**：图标标签里的 `&` 助记符已剥离（`.desktop` 的 `Name=&Demo App` → 桌面显示 `Demo App`，VM 截图验证）；Taskbar「帮助(H)」由 `app_is_installed("/bin/Help")` 守卫，Help 未构建时该菜单项不出现（旧 STATUS 描述已过时）。
- **桌面自动刷新（11-03）**：`DesktopIconsWidget` 首次加载后对 `~/Desktop` 建 `Core::FileWatcher`（inotify，监听 ChildCreated/Deleted/Content/Metadata），`on_change` 里 `load_entries()+update()`。此前压缩出 `.zip/.tar.gz` 或别处丢文件到桌面要手动「刷新」才出现；现自动更新。VM 实测：shell 运行时在 `~/Desktop` 新建 `WATCHTEST.txt`（另一进程写入），图标自动出现；删除后自动消失。
- **FileManager 打开 exe（11-03 修复）**：之前 `.exe/.msi/.bat/.cmd` 无 MIME handler（无 .af、无 LaunchServer.ini），双击/右键 Open 无反应。现 `DirectoryView::handle_activation` 与文件右键菜单（`add_launch_handler_actions_to_menu`）都识别 Windows 二进制并调 `serenity-winapp <path>`（auto 模式，自然分 app/game）。
- **桌面/文件管理器右键统一（11-03）**：抽出 `Ports/LinuxGUI/Common/ArchiveOps.h`（`is_archive` / `compress(source, use_zip, optional stem)` / `extract`，统一用 7z/zip/tar/unzip）。桌面 `compress_target/extract_target/is_archive` 改为调用它（行为不变）；FileManager 两处改为：`do_unzip_archive` 走 `PortArchiveOps::extract`（**现在能解压任意归档，不再只 `.zip`**），右键「Extract Here」的显示条件从 `ends_with(".zip")` 改为 `is_archive()`；`do_create_archive` 走 `PortArchiveOps::compress`，按输入名后缀选 zip / `.tar.gz`（原来固定 zip 且 fork /bin/zip），与桌面「压缩」一致。VM 确认 tar/unzip/7z 均在位。**仍存的差异（有意）**：桌面含「游戏模式启动」、FileManager 含「用 Wine 运行」；文案一个中文一个英文；桌面图标菜单暂无「粘贴」。
- **wine 11.0** 已装（清华源），双击 .exe 走 wine。
- **汉化**：LibGUI 运行时 `[UI]` 字符串表（700+ 条），改 ini 免重编。
- **快速启动栏**：图标、条目数、拖放（拖 .af 进栏 → 写 Taskbar.ini）、`.desktop` 条目全部验证通过；HiDPI blit 崩溃、`paint` 不跑、图标缓存、条目越写越少等根因已修。
- **系统监视器 Lite**（`serenity-system-monitor`，读 /proc，单实例 flock）替代上游依赖 LibSymbolication 的 SystemMonitor。
- **任务管理器**：`spawn_or_show_error` 仅在真正 ENOENT 时报「未安装」。
- **Terminal 增强（11-03 VM 验证）**：窗口尺寸记忆、Ctrl+PageUp/Down 切页、尺寸「无限长」bug 修复（配置键语义冲突 + chicken-and-egg 兜底）、`SIGCHLD=SIG_IGN` 经 forkpty 泄漏导致 apt `waitpid ECHILD` 修复（exec 前还原 `SIGCHLD`/`SIGPIPE` 为 `SIG_DFL`）；`Terminal.ini` 现稳定 `Width=706 Height=450`。

### 1.3 合成器 / X11 / 原生 Wayland 客户端（11-01 → 11-02）
- **`serenity-wlroots-host` popup 首帧 configure（11-05 VM 验证）**：wlroots 路线的两个时机错误已闭环 —— ①`schedule_configure` 断言 `initialized`（只在 popup 首次 commit 时置真），改挂 `surface->events.commit`；②`wlr_xdg_popup_unconstrain_from_box` 内部末尾就会 schedule_configure（反汇编确认），在 `new_popup` 里做约束避让同样必崩，改为「先存约束框、首次 commit 后生效」，且与 `schedule_configure` 二选一避免重复 configure。VM 实测 4 个 popup 各收到恰好一次 configure、坐标符合 positioner 语义、`Assertion` 计数 0。测试归档 `Ports/LinuxGUI/WlrootsHost/tests/`。
- **`serenity-wlroots-host` 真实点击链路（11-05 VM 验证，gedit 实测）**：真实部署后点菜单按钮才暴露的四个问题已修 —— ①`enter` 写死 (0,0)（窗口映射时指针已在窗内就再没有 motion 纠正）→ 所有点击打到左上角；②输入坐标没反算 `window_geometry`（GTK 阴影 26×23）→ 点击整体偏上、落在 GTK 标题栏触发 `xdg_toplevel.resize`（「点按钮变拖窗口」）；③缺 `wl_pointer.frame`（`wlr_seat` 的 motion/button/axis 不自动发）→ **按下不弹起、第一次点击只按下、第二次才生效**；④子窗口没开 alpha 通道 + 控件铺灰底 → GTK popover 的半透明阴影变成一圈灰边（不是「合成器不支持阴影」）。修后：鼠标首次点击即弹出 ☰ 菜单、菜单项可点（Preferences 正常出窗）、外部点击正常关闭、任务栏无 popup 按钮、阴影为柔和渐变。详见 `HANDOFF_WSWLR_COMPOSITOR.md` §0.11。
- **X11Bridge / WaylandHost 均已退役**：`Ports/LinuxGUI/X11Bridge/`（11-02）与旧桥 `Ports/LinuxGUI/WaylandHost/`（11-05）都已删除；X11 侧的 XWM + rootless Xwayland 保留为 `Ports/LinuxGUI/X11Compat/`，由 `serenity-wlroots-host` 复用。
- **WaylandHost 稳定项**：Xwayland 自 spawn（`-displayfd` 就绪信号）、X 窗口配对+渲染（xterm/Firefox GTK/ESR 复古边框正常）、鼠标左键映射、滚轮（axis/discrete/source/frame）、override-redirect 弹窗（无框 + TRANSIENT_FOR 锚定）、键盘映射表重写 + Shift/Ctrl/Alt/Super 修饰键转发（14 项根因史已闭环：aefd0318e6→46a586f751→8313b4af56→3f9ada7e05）。
- **原生 Wayland 客户端输入**：按客户端路由 pointer/keyboard（`for_each_pointer/keyboard_of_client`），`wl_subcompositor`/`wl_subsurface`、KDE `org_kde_kwin_server_decoration`（default=SERVER 复古框）、`wl_data_device_manager v3` 已落地；GTK4 gnome-text-editor 真实硬件注入打字成功（"abc" 上屏）。
- **开始菜单 Linux Apps 子菜单**：扫描 `~/.local/share/applications` 等三处，解析 Name/Exec/Icon；点击启动已修（`posix_spawnp` 查 PATH + `resolve_executable` 先解析绝对路径）。
- **菜单点不开（"鼠标没反应"）已修**：`keyboard_focus_surface()` 在 pointer press 也发 `wl_keyboard.enter`；xdg_popup 子表面一律 Popup + frameless。
- **liblagom 库随构建同步**：`build_*.sh` 一并 `cp -a lib/liblagom-*.so*` + `ldconfig`。
- **portal 自建 `serenity-portal`**：sd-bus 只实现 Settings 的 stub（双层 variant 坑已写进注释），gedit 出窗 18s → 1.0s。
- **X11 窗口标题（11-03 修）**：以前只读 `WM_NAME`（`XFetchName`），Wine/GTK/Qt 对中文标题只设 `_NET_WM_NAME`（UTF8_STRING）→ Serenity 标题栏和任务栏空白。现在 `xwm.cpp` 优先读 `_NET_WM_NAME`（UTF8_STRING）再回退 `WM_NAME`，并把 `_NET_WM_NAME` 加入 PropertyNotify 监听（标题晚到也能更新）。VM 实测 Wine 光影魔术手：标题栏和任务栏都显示「光影魔术手」。
- **截图正确通道**：`kill -USR1 $(pidof serenity-window-server)` → `/tmp/serenity-shot.ppm`（WindowServer 内存帧缓冲，经 `guest_shot.sh`+`gshot.py` 取回）。**QEMU screendump 不可信**（1337 vs 1344 行错位斜切）。
- **帧回调节流（11-04 13:35 VM 验证）**：`wl_surface.frame` 回调不再请求即 done，改由 8ms `pump_timer` 按 ~16ms 节拍批量 `send_done(cb, 单调递增 ms 时间戳)`，surface 销毁时丢弃其 callback；`SERENITY_FRAME_IMMEDIATE=1` 保留旧行为。**直接修掉 Firefox 原生 Wayland 冻结在首帧**（原 ~9ms 间隔 1036 次 frame 空转 / 13 次 attach → 现 15–17ms 节拍、45 次 attach、缓冲池 44 次 release 正常循环）。详见 `WAYLAND_ROADMAP.md` §3.5。
- **装饰策略改回强制 SERVER_SIDE（11-04 13:39 VM 验证）**：用户拍板「窗口必须强制套复古边框」，回退上午的「尊重客户端 CSD」。`apply_decoration_mode` 无条件 SERVER，`xdg-decoration`/KDE request_mode 均忽略 CLIENT_SIDE 请求并回 SERVER；唯一例外 Wine（buffer 自绘 Windows 标题栏）。`GTK_CSD=0` 保留。截图确认 Firefox 套上复古蓝框、无黑边、无层叠错乱。详见 `WAYLAND_ROADMAP.md` §3.6。
- **damage 跟踪 + KMS 脏区拷贝（11-04 14:20 VM 验证）**：桥侧 `surface_damage`/`damage_buffer` 真正记录，commit 时合并成包围盒、**复用上一帧 `frame_bitmap` 只拷脏区**（不再每帧新建位图 + 全量转换），`SurfaceWidget::set_frame_partial` 只 `update(脏区)`；输出侧 `flush_framebuffer_rects` 的 KMS 分支改为按 rect 逐行 memcpy（`present_buffer_kms_rects`，翻转抽成 `flip_kms`），并用 `pending_sync/accum_sync` 两张表解决**双缓冲跨帧结转**（一帧多次 flush 也正确）。**实测**：空闲 commit 从 760k px 降到几百 px；gedit 连打 5 字 33%、地址栏 3 字符 7%；about:support 首屏/翻页滚动 72–100%（本该整幅）。Firefox+gedit 叠加、打字、滚动均无残影/撕裂。回滚 `SERENITY_DAMAGE_DISABLE=1`，统计 `SERENITY_DAMAGE_STATS=1`。详见 `WAYLAND_ROADMAP.md` §3.7。⚠️ 踩坑：`Gfx::Rect::right()/bottom()` 是**排他**边界，循环用 `<=` 会多算一行 → `Bitmap::scanline()` 断言把桥打挂。
- **xdg_popup.grab + 菜单定位（11-04 15:25 VM 验证）**：`grab` 记录 seat/serial 并把键盘焦点给 popup（GTK 菜单方向键/回车、Firefox 菜单高亮都靠它）；每次 press 先判"是否点在 popup/后代里"，否则给整棵 popup 树发 `popup_done`、焦点还给父窗口，**并吞掉这次按键**（含 release）。同时修掉 popup 定位的根因：`anchor`/`gravity` 是**组合枚举 0–8 而非位标志**，之前按 `&` 解析算出 `configure(-99,-173)`，菜单被压到屏幕左上角；现按两轴解码并补了简化 FLIP+SLIDE，菜单落在锚点旁。实测：gedit Shift+F10 出菜单、↓↓ 导航、点窗口即关且不误触；Firefox File 菜单落点正确。**注**：当时记的「3 次/12 次全过」是假阳性（恢复对话框），且「待 weston 对照」已被用户否掉——问题在合成器，见下条。
- **键盘焦点模型重建 + 合成器协议保真度定性（11-04 16:20，按 mutter 50.5 对照 + VM 实测）**：用户判定「不是火狐的问题，继续修合成器」「不要自己猜测，多看看 mutter」，据此逐条对照本机 `/mnt/user/rdp/mutter-50.5` 源码，修掉三处**结构性**问题：① `keyboard_key()` 旧代码对每个键事件都 `keyboard_focus_surface(当前 LibGUI 焦点窗口)`，F10 按下开菜单 → popup 窗口夺焦 → **F10 抬起**把 `wl_keyboard.leave/enter` 重指向刚建的 popup，即在同一次按键 press/release 之间换焦点（竞态崩溃的土壤）；现改为按 `current_keyboard_focus_resource()` 选 client 投递，焦点只在 press/map/popup-dismiss 三处变更。② mutter `finish_popup_setup()` 对**非 grabbing popup** 是 `window->input = FALSE`；实测 Firefox XUL 菜单**不调** `xdg_popup.grab`（`POPUP grab` 日志为零），改为映射时只有 `toplevel || grab_active` 取焦点，`SERENITY_POPUP_FOCUS_ALL=1` 可回滚 A/B。③ 新增 `activation_toplevel_for()`，沿 subsurface + xdg parent 链解析 owning toplevel，解析不到就保持激活——旧代码把 `g_activated_toplevel` 指向 popup 的内容 subsurface，菜单开着时给主窗口发了 `ACTIVATE active=false`。另在 `xdg_surface_get_popup` 加 `POPUP create` 调试日志。**验收**：焦点序列 `focus=38` 全程不变、无 `KBFOCUS` 跳变、无 `ACTIVATE false`；干净会话菜单区 ↓↓ 像素差异 0.92%（高亮有效，功能未回退）；**6/6 带断言压测全过**（每轮全新 profile，`alive=1`/`popup>=1`/`deact=0`）。详见 `WAYLAND_ROADMAP.md` §3.8。**仍存**：菜单关闭后偶见 popup 窗口残留黑块（unmap 生命周期）、点桌面/任务栏不关菜单、serial 校验与 `NOT_THE_TOPMOST_POPUP` 未实现。

### 1.4 主题 / Win2000 外观 + 输入法（11-03 用户确认视作完成）
- **主题 / Win2000 外观**：GTK3/GTK4/libadwaita 三套模板重写为 Win2000（面板 `#d4d0c8`、标题栏 `#0a246a→#a6caf0` 渐变、选中 `#000080` 白字、3D 滚动条、MS Sans Serif→Tahoma→DejaVu）。装法：`serenity-theme-gen.py --out /usr --theme Default` + libadwaita 覆盖 `~/.config/gtk-4.0/gtk.css`。
- **libadwaita 实装**：三个补丁 dry-run 通过，`build_libadw.sh` 产物覆盖 `/usr/lib/x86_64-linux-gnu/libadwaita-1.so.0`（原库备份 `/root/libadwaita-1.so.0.dist`）。
- **「显示设置」应用主题报错（11-03 修）**：`ThemesSettingsWidget` 点应用弹「Failed to apply theme settings」。根因：`/etc/WindowServer.ini` 是指向只读安装前缀的软链，`WindowManager::update_theme` 在 `sync_config_to_disk()` 失败时直接 `return false`——其实主题在内存里**已经应用**了，只是持久化失败。改为：持久化失败只 `dbgln` 警告、仍 `return true`（主题当场生效，不再弹错）。注：因 `/etc/WindowServer.ini` 不可写，主题**不会被持久化**，重启会回到配置里的 `Theme/Name`。
- **GTK 主题引擎**：`engine/serenity-theming-engine.c` 已编译 17KB；GTK 3.24 基本不经过引擎，结论「不继续投入」。
- **输入法客户端**：`LibGUI/InputMethod.{h,cpp}`（socket 客户端）+ `InputMethodCandidateWindow`（Autocomplete 弹窗）已写，`ime-fake-engine.py` 跑通 `nihao`→「你好」。fcitx5 addon + LibGUI 客户端链路闭合；**rime 激活**作为独立缺口仍留 §三。
- **候选窗对比度修正（11-05，VM 实测通过）**：旧配色 `#f4f4f4` 底几乎与白纸同色、选中项白底（比底色还亮）致选中不可见，被用户判"不对吧"。重做：冷灰面板 `#e8edf3` + 阴影 + 选中蓝底 `#2f6fd0` 白字 + 分隔线，对齐 gedit 里 fcitx5 观感。实测 notepad 里「你好」蓝底白字清晰可辨，截图 `Build/vm/ime-inline-*-2026-11-05.png`。
- **内联预编辑（11-05，VM 实测通过）**：新增 `TextEditor::set_preedit`，`InputMethod` 把拼音推给编辑器，在**输入区光标处**内联画拼音（下划线 + 内部光标），候选窗不再重复画拼音（只留 `aux`+候选+翻页）。实测 notepad 打字后光标处显示下划线 `ni hao`，空格上屏「你好」。对应 README 的"内联预编辑" TODO。
- **Linux 应用复用 Serenity IME（方案 A，11-05 VM 实测通过）**：新增 `Ports/LinuxGUI/IMEFrontend/gtk3/im-serenity.c`（GTK3 immodule），以 `GTK_IM_MODULE=serenity` 把 GTK 应用接到 `/tmp/serenity-ime.sock`；拼音走 `get_preedit_string` 内联下划线，候选窗用与 `InputMethodCandidateWindow.cpp` 完全一致的调色板自绘。**坑**：Wayland 下 `gtk_widget_set_parent_window()` 会把候选窗变成 `wl_subsurface`（被父 surface 裁剪、`move_to_rect` 失效、session log 无 xdg_popup）；改为 `gtk_window_set_transient_for()` 后才创建真正的 `xdg_popup`。截图 `Build/vm/ime-gtk-9candidates-2026-11-05.png`（gedit：内联 `ni hao` + 9 候选）。
- **输入法配置（11-05，按用户要求落定）**：`~/.config/fcitx5/profile` 只留 `keyboard-us` + `rime`（`DefaultIM=rime`）；`luna_pinyin.custom.yaml` 启用 5 组常见模糊音（z/zh·c/ch·s/sh、n/l、r/l、en/eng·in/ing、eng/ong）+ 简拼 + 拼写/按键纠错，**明确不含 f/h 与 g/k**；`menu/page_size: 9`。编译产物 `build/*.yaml` 已核对为 9，协议层 `panel` 的 `n=9`；模糊音生效的动态证据：`ni hao` 候选混入「利好/立好/理好」（n/l）。⚠️ `fcitx5 -r` 从 ssh 起会缺 DISPLAY 且退出时**覆写 profile**，必须 stop/start `serenity-desktop.service` 或走 DBus `Restart`。
- **输入法设置界面（11-05，新增并 VM 实测）**：`Ports/LinuxGUI/IMESettings/main.cpp` → `serenity-ime-settings`，`GUI::SettingsWindow` 单 Tab（中文输入法 / 常见模糊音 / 候选预览），`.af` 为 `Base/res/apps/ImeSettings.af`（`/bin/ImeSettings` → `serenity-ime-settings`）。三项需求直接体现在 UI：只显示 Rime、5 组模糊音且明写「已按要求排除 f/h、g/k」、候选数默认 9 且预览条复用 `GUI::InputMethodPanelWidget` 正好画 9 项。**生效链路**：点「应用」写 YAML + profile → `busctl --user call org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1 Restart` 让 fcitx5 自重启并重新部署 rime（`CanRestart=true`）。实测把 9 改成 7 后 build 与协议层同步变 7、改回 9 同步变 9。**坑**：`rime_deployer --build` 在 fcitx5 运行时是空操作（rc=0 但 build/*.yaml 不动），不能用来热更新。截图 `Build/vm/ime-settings-2026-11-05.png`、`ime-settings-7candidates-2026-11-05.png`。
- **候选窗定位修复（11-05，VM 实测通过）**：此前 GTK 候选窗被钉在父窗口**顶部装饰条**上（日志 `popup pos parent=80,80 lx=-26 ly=-23 -> 54+57`，`-26,-23` 正是 CSD 偏移的负值）。两层根因，各修一处：
  1. **客户端侧**（`gtk3/im-serenity.c`）：GDK 在 popup **首次映射时**就创建 `xdg_positioner`；原来在 `show_all()` 之后才调 `gdk_window_move_to_rect()`，首个 xdg_popup 用的是 `anchor_rect=(0,0)`，后续调用退化成桥未完成的 `reposition`。改为先 `gtk_widget_realize()`（去掉 `set_parent_window` 后不会再触发 anchored 断言）→ **映射前** `move_to_rect` 传光标 rect → 再 `show_all()`。
  2. **合成器侧**（`WlrootsHost/wlroots_proto.{c,h}` + `main.cpp`）：wlroots 把避让后的最终几何写进 `popup->scheduled.geometry`，`current.geometry` 要客户端 ack **且再次 commit** 才更新；GTK 对纯位置变化只 ack 不重贴 buffer。`wlroots_proto_popup_geometry()` 改为优先读 `scheduled`，并新增 `popup_configured` 回调，在发 configure 时立刻 `position_popup()`（不再等下一次 commit）。
  - 实测：configure 几何 `-26,-23` → `26,67`，窗口 `54+57` → `106+147`，候选窗落到 `ni hao` 正下方。截图 `Build/vm/ime-gtk-9candidates-2026-11-05.png`。原生 LibGUI 候选窗是编辑器内 overlay，不走 xdg_popup，不受此问题影响。
- **设置界面功能回归（11-05 16:00-16:30，VM 实测）**：
  - **控制面板入口**：`ImeSettings` 加入 `Settings/main.cpp::s_groups` 的 "System" 组；窗口从 500×420 加宽到 700×460，否则第 7 个图标被 IconView 滚动条裁掉、落到看不见的 "Other" 组。截图 `Build/vm/ime-settings-panel-2026-11-05.png`。⚠️ 双击图标的 QMP 注入误触发了一次会话软重启，未在 UI 内完成"点击启动"实测；入口可发现性 + `/bin/ImeSettings`→`serenity-ime-settings` 映射 + 命令行启动成功均已验证。
  - **模糊音开关**：取消 n/l 后状态栏变「7/8 组」，`luna_pinyin.custom.yaml` 少了 `n_l_bufen`、build schema 少了 `derive/^n/l/`、fcitx5 重启到新 PID（截图 `ime-settings-7of8-2026-11-05.png`）。候选里仍保留 `li-` 词条是 `spelling_correction` 的编辑距离纠错，属预期。
  - **恢复默认值**：8 条规则、`derive/^n/l/`、`page_size=9`、`DefaultIM=rime` 全部复原。
  - **修掉一个真 bug**：取消「启用中文输入法」时 `DefaultIM` 被写回 `rime`。根因是 fcitx5 在 `Controller1.Restart` 优雅退出时用**内存状态**覆写 profile，`SetCurrentIM` 又不改 group 的 DefaultIM。改为 `schedule_finalize()` 双 fork 延迟 4s，在保存落地后重写 profile 并经 ctl socket 切当前 IM；实测 `DefaultIM=keyboard-us` 持久化成功，再勾回来恢复 `rime`。
  - **候选框尾部空白**：设置界面预览条被布局拉满整行，内容到 `>` 后拖 ~180px 冷灰底色 → 用 `InputMethodPanelWidget::preferred_size()` 固定宽度 + 水平 spacer 左对齐；GTK 候选窗的翻页箭头预留 `2*(font_h+pad)` 改为按实际 `<`/`>` 字形测量，configure 宽度 492→459px。截图 `ime-settings-preview-fixed-2026-11-05.png`。
  - **最终状态**：只 `keyboard-us`+`rime`、8 条模糊音、无 f/h 与 g/k、custom/build 三处 `page_size=9`、协议层 `n=9`。



### 1.4b 任务管理器 Win7 对齐 + Ctrl+Alt+Del（11-05）

- **CAD 不再是重启**：根因 `/proc/sys/kernel/cad_pid` 是 `root:root 0600`，而会话以非特权用户运行，日志每次都是 `could not claim Ctrl+Alt+Del (cad_pid)`，CAD 落回 init → `ctrl-alt-del.target` → 重启。修法：`05-install-prefix.sh` 的 unit 加 `ExecStartPost=+sh -c "sleep 2; pgrep -x serenitid.real > /proc/sys/kernel/cad_pid"`（root 侧写入真实 PID）+ `systemctl mask ctrl-alt-del.target` 兜底；`serenitid` 去掉过期的「会话是 root」假设与误导日志。**实测**：注入 `sendkey ctrl-alt-delete` → `CAD pressed` / `CAD: launching task manager`，机器不重启，任务管理器窗口弹出（`Build/vm/taskmgr-cad-2026-11-05.png`）。
- **任务管理器按 Win7 严格对齐**（`Ports/LinuxGUI/SystemMonitorLite/main.cpp`）：6 个 Tab（应用程序 / 进程 / 服务 / 性能 / 联网 / 用户）、5 个菜单（文件 / 选项 / 查看 / 窗口 / 帮助）、全局 3 段状态栏（进程 / CPU / 物理内存）。应用程序页只留「任务 | 状态」+ 结束任务/切换至/新建任务按钮；进程页默认列「映像名称 / 用户名 / CPU / 内存 / 描述」，复选框「显示所有用户的进程」在底部，隐藏列由「查看 > 选择列」打开；服务页 5 列 + 服务按钮；性能页 **4 个图**（CPU 使用率 / CPU 使用记录 / 内存 / 物理内存使用记录）+ 系统·物理内存·适配器分组统计 + 资源监视器按钮；用户页「用户/ID/状态/客户端名/会话」。列可排序、右键菜单含结束进程树 / 优先级 / 属性 / 转到进程；「选项/查看/窗口/帮助」里本 port 没有 WM IPC 的项（层叠/平铺/前置/内核时间/帮助主题等）按 Win7 一样置灰而不是假装可用。窗口 660×460（用户反馈 920×620 太大）。
- **内核进程合并**：Linux 的几十个 kthread（`ppid==2`）聚合成单行「内核进程」（PID 2 / SYSTEM / Linux 内核），并禁止结束它；进程数 197 → 45。
- **进程树**：进程页「显示进程树」复选框按 PPID 构建层级，缩进 + `[-]`/`[+]` 标记，双击行展开/折叠（ASCII 标记，避免字体缺三角字形）。截图 `Build/vm/taskmgr-tree-2026-11-05.png`。
- **启动加载优化**：旧实现在 `window->show()` **之前**同步扫 `/proc`，每进程读 stat/status/io/cmdline/exe，`collect_gui_apps()` 还遍历每个 fd 做 readlink。改为窗口先绘制、首次采样放到事件循环下一轮；删掉 fd 全扫描；命令行改为属性对话框按需读取。首次采样实测 **9–15 ms / 49 进程**。
- **修掉真 bug**：`collect_gui_apps()` 的 `readlink()` 没补 `'\0'`，`strlen()` 读到栈垃圾，名字拼出 `rver`/`trver` 乱码并让 `-server` 过滤失效 —— 所以后台服务全跑进「应用程序」页。现补 NUL + 后台标记黑名单。
- **访问级别**：给 ELF 加 file capabilities `cap_dac_read_search,cap_kill,cap_sys_ptrace,cap_sys_nice+ep`（不用 setuid root），由 `05-install-prefix.sh` 的 `setcap` 持久化；运行进程 `CapEff=0x880024`。VM 需 `libcap2-bin`（`setcap` 在 `/usr/sbin`）。
- **崩溃恢复加固 + 实测**：`socket_ready()` 只 stat，无法区分 hung 与 stale socket → 新增 CAD 专用的 `socket_accepting()`（connect 后立刻关闭，不发数据）；`recover_graphics_stack()` 在重启失败时重试 3 次。实测 `kill -9 window-server` → supervisor 200ms 内重启 WS 和 shell、桌面自动恢复（壁纸/图标/任务栏）；再按 CAD → 任务管理器正常弹出且未误触发图形栈重启（`Build/vm/taskmgr-desktop-recovered-2026-11-05.png`、`taskmgr-cad-after-crash-2026-11-05.png`）。
- **已知限制**：如果 WindowServer **卡死但不退出**，内核 backlog 仍接受 connect，`socket_accepting()` 会判健康；要覆盖这类 hung 需要发一个合法 WindowServer IPC ping 等响应。完全自绘、不依赖 WindowServer 的「救援界面」也未实现 —— 它需要接管 DRM master（等 WS 释放）、evdev 输入以及一套独立控件框架，属独立大工程，当前恢复链路已覆盖进程崩溃这一常见场景。

### 1.4c Vista 系统主题与「关闭主题」开关（11-05，方向定稿）

- **用户拍板**：目标是**复刻 Vista 风格的系统**，不是只让某个 app 像 Vista；并且要能**关闭主题回到经典模式**（XP/Vista 关主题即回落经典外观）。已写入 `VISTA_THEME.md`（唯一权威说明）。
- **架构**：结构可应用内自绘，**颜色必须来自系统主题**。新增 `Base/res/themes/Redmond Vista.ini`（Aero 蓝 + `WindowTheme=RedmondGlass` 玻璃装饰）；现有 `Redmond 2000.ini` 即「关闭主题」态。复用既有 `Gfx::Palette` / `GlassWindowTheme` / `WindowManager::update_theme` 管线，不改 `Palette.cpp` / `WindowFrame.cpp`。
- **文件管理器 chrome 不再硬编码配色**：`ExplorerChrome.cpp` 的 `Aero::*` 全部经 `GUI::Application::the()->palette().color(ColorRole::…)` 取色（Window/Button/ThreedShadow1/Gutter/HoverHighlight/MenuSelection/WindowText/DisabledTextFront/Base）。切回经典主题时 chrome 自动变灰，无分支判断。
- **开关**：`scripts/port/serenity-theme.sh {vista|classic|status}` —— 改 `[Theme] Name` → 用同一份 ini 重生成 GTK3/GTK4/libadwaita/Qt 主题（`serenity-theme-gen.py`）→ 重启 `serenity-desktop.service`。
- **持久化修复**：`05-install-prefix.sh` 把 `${PREFIX}/etc` 交给会话用户、`WindowServer.ini` 设 664，修掉旧 §1.4 记的「主题生效但重启丢失」（`/etc/WindowServer.ini` 是只读前缀软链）。
- **待验收**：Vista / 经典双态 VNC 截图对比，GTK 应用是否同步，重启后主题是否保持。

### 1.4d 文件管理器 Vista/Win7 化 + 共享条目层（11-05）

- **目标**：把 `serenity-file-manager` 从 Win9x 布局改造成 Vista/Win7 Explorer —— 命令栏 / 地址栏（后退·前进·向上·刷新·面包屑·搜索框）/ 导航窗格（收藏夹·库·计算机，含桌面节点）/ 预览窗格 / 状态栏 + 中文菜单、实时搜索过滤、视图切换、文件夹选项。
- **决策**：不把 FM 进程并入 `serenity-shell`，只把条目数据模型与右键菜单抽成共享层。`Ports/LinuxGUI/Common/ExplorerEntries.h`（`Entry` / `launch_windows_binary(Auto|Game|App)`）与 `ExplorerMenus.h`（`append_item_actions` / `append_background_actions`）为两边唯一来源；桌面 `DesktopIconsWidget` 已改用共享层，`fill_new_menu` 让桌面模板/快捷方式并入同一个「新建」子菜单。
- **已落地**：`ExplorerChrome.cpp` 实现全部自绘组件；`FileManagerWindow.gml` 重排为四段骨架（自定义 widget 由 C++ 挂到命名 host，避免 GML 类注册）；`main.cpp::run_in_windowed_mode` 全量接线（中文菜单、组织/共享下拉、搜索、导航节点、预览、视图切换、文件夹选项对话框、共享层右键菜单）；`DirectoryView` 接入 `FilteringProxyModel`，`node(index)` 改为 `filter → sorting → FileSystemModel` 两级映射；`generated-apps.cmake` 重新生成以纳入 `ExplorerChrome.cpp`。
- **坑**：`generated-apps.cmake` 是 `scripts/port/gen-app-targets.py` 生成的，新增 .cpp 只改 `Userland/.../CMakeLists.txt` 不会进链接（一堆 undefined symbol），必须重跑生成器；`append_item_actions` 返回值带 `[[nodiscard]]`，桌面里不接收会 `-Werror` 失败。

- 🔒 **不可更改 / 冻结（DO NOT REVERT）— 右键菜单「能打开且不崩」修复（11-06）**
  `Userland/Applications/FileManager/main.cpp::run_in_windowed_mode` 的 `on_context_menu_request`
  里这套改法**是最终正确方案，禁止回退或改写**，否则会在「菜单打不开」与「点菜单项闪退」之间反复：
  1. 函数级 `RefPtr<GUI::Menu> live_context_menu;`，**每次 popup 前** `live_context_menu = xxx_menu;`
     把菜单留活。`GUI::Menu::popup()` 是异步的——handler 返回后 WS 才上屏菜单，栈上 `Menu` 会先被
     析构 → `unrealize_menu()` 删菜单 → 日志 `invalid menu ID`、菜单打不开。
  2. item 分支回调用 **按值捕获** `[&, primary]` / `[&, paths]` / `[&, paths, primary]`，
     **绝不可改回 `[&]`**。`paths`/`primary` 是 handler 局部，菜单被留活后这些局部已析构，
     `[&]` 会留悬垂引用 → 点击 `Action::activate` 时 `NonnullRefPtr`/`RefCounted` 校验失败崩溃。
     background 分支回调只引用函数级变量（窗口生命周期），安全不动；
     `organize_menu`/`share_menu`（~987/1009 行）本就函数级变量，安全。
  3. 顺带修同类隐患：搜索范围下拉菜单（`search_scope_menu` 同款留活，~1052/1086 行）。
  - ⚠️ **部署约束**：此修复目前是 **未提交（uncommitted）** 改动。若部署从干净/已提交源码（如
    `f4773b0c36` 那版）构建，会丢掉修复 → 右键菜单再次失效。**部署前必须确保源码含此修复**
    （先提交再部署，或部署构建工作树）。11-06 09:57 已发生一次：VM 的 `/serenity/bin/serenity-file-manager`
    被覆盖成旧二进制（2098792，无此修复）→ 右键菜单又没了，已重新部署 2182552 并重启
    `serenity-desktop.service` 恢复。
- **待办**：VM 部署 + 截图验证；Win+E 快捷键；`filesystemaccess` 自研组件接入评估。

### 1.5 Windows 运行时策略 + 原生启动器（11-03）
- **决策（用户拍板：移植为原生）**：`serenity-winapp` 由 sh 脚本改为**原生 C++ 二进制**，策略内置于代码，对齐 omikuji 的 `assemble_launch + WineVariant`（omikuji 核心启动是 Rust 原生 argv + `Command::new().spawn()`，`sh -c` 只用于 pre/post hook）。
  - 源码 `Ports/LinuxGUI/WinAppLauncher/main.cpp`；目标 `serenity-winapp`（无 Serenity 库，纯 POSIX `execvp`）。
  - `WinApps.h` 的 wrapper 仍是 `exec <prefix>/bin/serenity-winapp --prefix … --mode … exe`（`.af` 的 `Executable=` 带不了参数，故保留 1 行 wrapper）；调用点不变。
- **运行时两分（关键）**：
  - **game** → `<prefix>/bin/umu-run` + `PROTONPATH=<prefix>/opt/dwproton`（Steam Runtime + Proton + protonfixes + DXVK/VKD3D + fsync）。设 `GAMEID`(默认0)/`UMU_ALLOW_ROOT=1`/`WINEDLLPATH`(vkd3d)。
  - **app** → **原版 wine**（`/opt/wine-stable/bin/wine`，wine-11.0）。Proton 是游戏向分支（DLL override、依赖 Steam Runtime），对办公/安装器/老 Win32 应用是更多可破坏面；原版 wine 更稳更轻。DWProton 仅作回退（umu-run 缺失 / 无原版 wine 时）。
  - **旧行为已纠正**：此前 app 走 `opt/dwproton/files/bin/wine`（Proton 的 wine 但无 Runtime/修复）＝两头不占好。
- **WINEDLLPATH 作用域**：原 sh 全局 export 指向 Proton vkd3d DLL，会把原版 wine 带坏（ABI 不匹配）；现仅 game/proton 分支 `apply_proton_dll_path()` 设置。
- **auto 判定**：扫 PE 导入表（d3d8/9/10/11/12、dxgi、xinput、dinput8、vulkan-1、openal32；上界 32MiB）+ 同目录引擎标记（UnityPlayer.dll、steam_api*、*.pak/.uasset/.pck/.rpa/.assets）。`--mode` 覆盖，`winapps.ini` 持久化每程序选择。
- **安装**：`05-install-prefix.sh` 不再安装 sh 脚本（`serenity-*` 循环自动装二进制）；旧 `winapp-launcher.sh` 仅留作历史参考。
- **VM 实测**：`/serenity/bin/serenity-winapp` 为 ELF（115216 B）；`--dry-run` 判 app/game 正确；`--game` → 执行 `umu-run`（env `WINEDLLPATH/PROTONPATH/GAMEID=0/UMU_ALLOW_ROOT=1` 均对）；`--app` → `execve("/opt/wine-stable/bin/wine", …)`；服务 active。
- **修复：右键「打开」exe 无反应（11-03）**：未托管 exe 的默认 `WINEPREFIX` 原为 `/root/.wine-dw`，但 session 以 `serenity`（uid1000）运行、`/root` 是 `0700 root:root` → wine 报 `cannot use directory /root/.wine-dw : 权限不够` 后退出，错误无人捕获 = 「双击没反应」。已把默认前缀改为 `$HOME/.wine-dw`（原生 launcher 与旧 sh 同步）。VM 复现：改前 `wine: cannot use directory /root/.wine-dw : 权限不够`；改后 `wine: created the configuration directory '/home/serenity/.wine-dw'` 并继续执行。
- **开始菜单：Windows 程序独立分类（11-03）**：托管 Windows 程序不再混进「游戏」。`WinApps::write_menu_entry` 的 `Category` 改为 `"&Windows 应用"`（精确键，含助记符），`SystemMenu.ini` 加 `&Windows 应用=/res/icons/16x16/filetype-executable.png` 给子菜单图标。旧安装的 `.af`（`Category=&Games`）由 `WinApps::migrate_menu_category()` 在 shell 启动（`AppCatalog`）和管理器 `reload()` 时自动改写。VM 实测：既有 `neoimaging4-1.af` 已迁移为 `&Windows 应用`，开始菜单出现独立的「Windows 应用」子菜单（在「实用工具」与「Linux Apps」之间）。
- **启动器查注册表（11-03）**：`serenity-winapp` 在「未显式给 `--mode`/`--prefix`」时，会先查 `~/.config/serenity/winapps.ini`，命中就用其中登记的 Mode/Prefix（`lookup_managed_program`）。这修掉一个关键不一致：从**文件管理器/双击 exe** 打开时原来只走 auto 启发式（Hypergryph 启动器被猜成 `app` → **原版 wine**），现在会用注册的 `game` → **umu-run + DWProton**。VM 实测 `--dry-run`：Hypergryph→`game`、nEOiMAGING4→`app`；strace 确认 game 分支 `execve("/serenity/bin/umu-run", …)`。wrapper 显式 `--mode` 仍优先。

### 1.6 退出链路（注销 / 关机 / 重启）+ 真机（物理机）安装（11-03）
- **现象**：开始菜单 →「Exit…」→ 选「Log out」→ 弹 `错误: 未安装: /bin/logout`。根因：`ShutdownDialog`（`Userland/Services/Taskbar/ShutdownDialog.cpp`）spawn 的是 Serenity 内部路径 `/bin/{shutdown,reboot,logout}`，端口里 `PortHostify` 会把它们翻成 `serenity-<name>`，但这三个名字**从来没被安装过**。
- **修法**：新增 `scripts/port/session-control.sh`，`05-install-prefix.sh` 把它装成 `serenity-logout` / `serenity-shutdown` / `serenity-reboot`（同一脚本按 `$0` 分派）。
  - `logout` → 给 serenitid 发 **SIGUSR1**，不需要任何权限。serenitid 收到后按逆序停掉所有子进程并 **`execv(/proc/self/exe)`** 重开自己，等于「重新走一遍启动链（含 login 阶段）」；WindowServer 的 SIGTERM 处理器已把 DRM 交回，所以新会话能重新抢到 master。因为 `session.conf` 里 `Autologin=serenity`，观感是软重启桌面（想真停在登录窗就把 Autologin 去掉）。
  - `shutdown`/`reboot` → `systemctl poweroff|reboot`。会话是未特权用户、且没有 logind session，polkit 默认会拒（"Interactive authentication required"，GUI 里看不出原因）→ `05 --systemd` 额外写 `/etc/polkit-1/rules.d/50-serenity-session.rules`，只放行该会话用户的 poweroff/reboot 和本会话单元的 manage-units。
- **顺带修**：serenitid 传登录参数是 `--auto-login=USER`，登录服务器只认 `--auto-login USER`（两段）→ 改成两段（此前靠它自己读 `/etc/serenity/session.conf` 兜着，才没暴露）。
- **真机（物理机）多显卡**：`LinuxScreenBackend::kms_probe` 原来「打开成功的第一张卡」就用，真机上 `card0` 可能是无显示输出的独显（NVIDIA）→ 报 `no connected DRM connector` 整会话起不来。改为**逐张卡试到有 connected connector 为止**（card0/1/2），`probe_display_size` 同步。
- **真机安装脚本**：`scripts/port/06-install-host.sh`（root）＝建 `serenity` 会话用户（video/render/input/audio/shadow 组）→ 调 `05 --systemd`（unit 的 uid/home 现在**按实际用户取**，不再写死 1000）→ 写 `/etc/serenity/session.conf`（Autologin+zh-CN）并把 `/etc/{serenity,WindowServer.ini}` 软链进 prefix → 编 `serenity_compat.ko`（音频）→ `disable gdm3` + `enable serenity-desktop` + `set-default multi-user.target`。回滚脚本装到 `<prefix>/bin/serenity-restore-gnome`。**不自动启动桌面**，重启后生效。


---

## 二、待测试（代码已写 / 已编译，未部署 VM 实测，或仅代码走查）

> ✅ **主题 / Win2000 外观、libadwaita 实装、GTK 主题引擎、输入法客户端** 已于 11-03 经用户确认视作完成，已移至 §1.4。以下为仍待测试项。

- **Wine 中文字体**：`scripts/port/wine-fonts.py` 把 Sarasa Gothic SC 拷进前缀并注册中文字体名 + FontLink。**11-03 VM 实跑结论**：
  - VM 原本**没有可用 CJK 字体**（仅 `Droid Sans Fallback`；Sarasa 只在 `/serenity/res/fonts` 未被 fontconfig 扫到）→ 脚本 `fc-match "Sarasa Gothic SC"` 回退到 DejaVu Sans（纯拉丁，更糟）。已把 Sarasa 装到 `/usr/share/fonts/truetype/sarasa/` + `fc-cache`，`fc-match` 现返回 Sarasa。
  - 装后再跑 `serenity-wine-fonts --prefix <p>`：字体拷入 `drive_c/windows/Fonts/`、`Fonts` 注册项导入成功、marker 写入、rc=0。
  - **已修（11-03）**：`wine reg import` 会静默丢弃中文键名，导致 FontSubstitutes 不生效。改为 `wine reg add` **逐条写**（Fonts / FontSubstitutes / FontLink 每个值单独 reg add）。VM 实测：新前缀跑 `serenity-wine-fonts` 后 `wine reg query ...\FontSubstitutes /v 宋体` 命中 → `Sarasa Gothic SC`，`/v 微软雅黑` 同；`SimSun`/`Microsoft YaHei` 亦对（共 38 个 REG_SZ）。FontLink 的 `REG_MULTI_SZ` 为 best-effort。字体名解析链已闭合，中文 exe 应不再出方框（尚未用真实中文 exe 目测）。
- **剪贴板桥接（X↔Wayland）**：`wl_data_device_manager v3` 代码已实现（source/offer/device + set_selection 广播 + receive→send_data fd 转发，参照 weston）。**未实测**（X 应用复制 → GTK 原生粘贴）。
- **滚轮 / 悬停手测**：pointer_axis 已按客户端路由、悬停链路 GTK 已确认，但 Firefox/xterm 滚动、xterm 真实输入回归（VNC 注入点击 xterm→打字→回显）**只做过代码走查或做到一半**。
- **Settings 改造为 Win9x 控制面板**：`Userland/Applications/Settings/main.cpp` 改 GroupBox + IconView，本机构建通过，**未部署 VM 验证**；已知缺口 Display 项（DisplaySettings 依赖未移植的 LibEDID，点了失败）、Browser/Mail/Maps Settings 依赖 LibWeb 同样点开失败。
- **Windows 程序管理器（WinAppsSettings，11-03）**：`serenity-win-apps-settings` 已部署 VM（`/serenity/bin/` 在位）；启动器已原生化为 `serenity-winapp`（见 §1.5）。**「设置里点不开」已修（两层）**：①`GUI::Icon::default_icon("app-brickgame").bitmap_for_size(16)` 返回的指针归临时 `Icon` 所有，析构后 `Window::set_icon(悬垂指针)` 触发 `VERIFICATION FAILED` → 先存 `auto app_icon = default_icon(...)`；②真正的启动段错误：`GUI::ItemListModel` 存的是 `Container const&`（**引用**），而 combo 的模型是用临时 `Vector<ByteString>{...}` 建的 → 悬垂引用，`set_text("auto")` 读模型时 SIGSEGV → 把该 vector 提成具名局部变量。VM 截图确认「Windows 程序管理」窗口正常出现。**注意 ItemListModel 存引用**：其它地方传的必须是活得够久的 vector（成员/static/局部且组件同域）；已查 `g_types`/`s_*`/`m_*`/`static` 都安全，Spreadsheet 导入导出的局部 vector 未构建暂未处理。待办 = 端到端 UI 清单（面板可见、装 exe 生成 wrapper/桌面.desktop/开始菜单.af、开始菜单「游戏」子菜单合并、双击在独立 prefix 跑、卸载清三样产物）。P2 复核点：桌面目录一致性、`remove_artifacts` 删不掉时无提示、注册表并发。**UI 重做（11-03）**：改为 TableView 三列（名称/模式/程序文件）、底部状态栏、选中才启用 启动/运行环境/卸载、双击启动、模式下拉中文化、安装流程加「命名 + 是否立即运行」、加「刷新」。**「Label 不显示」实为根 widget 未填背景**（补 `set_fill_with_background_color(true)` 后 exe 路径 Label 正常显示，详见 §三）。
- **GTK 补丁系列**：目录与管线已就位（`Ports/gtk-serenity/`），patch 本体待写；GTK4 4.24.1 源码包 `dpkg-source` 解包失败（当前补丁对 4.18.6）。
- **serenitid 启动链**：已为子进程导出 `WAYLAND_DISPLAY`/`SERENITY_APP_DISPLAY`/`DISPLAY` 并拉起 fcitx5 + serenity-portal。**整体启动顺序未在最新改动后完整回归**。

---

## 三、未完成 / 已知缺口（未实现或待修）

- **剪贴板：Wayland 内部已可用（11-05）**：根因是没监听 `seat->events.request_set_selection`（wlroots 只当"请求"，不 accept 等于拒绝）。现已接上，`wl-copy` → `wl-paste` 实测可读出内容，Wayland 应用之间复制粘贴可用。**Xwayland 的 source 用 X 原子名（STRING/UTF8_STRING），直接塞进 seat 会让 host 堆破坏**，现用 `source_is_x11()` 隔离（X 内部仍可用）；剩余：X↔Wayland 互通 + Serenity `GUI::Clipboard` 双向桥（新桥缺这块）。
- **系统级剪贴板已就绪（11-03 复核）**：`serenity-clipboard-server` 已构建（`Ports/LinuxGUI/Services/main_clipboard_server_linux.cpp`）、安装、并由 serenitid 作为 `clipboard` 子进程拉起；socket `/tmp/session/0/portal/clipboard` 存在，`clipboard-history` 也在跑。LibGUI `Clipboard` 客户端连的就是这个 socket，桌面/应用间复制粘贴链路已通（旧 STATUS 说“没跑 ClipboardServer”已过时）。
- **Wine 原生 Wayland 驱动（11-03，✅ P0 完成 + P1 五项）**：真 `wp_viewporter` 落地；`wl_output` 用主屏实际尺寸；双标题栏已修（Wine frameless + move/resize）；`wl_region` + input region（未命中冒泡祖先）；pointer constraints + relative pointer；**subsurface Z 序 + sync/desync**；**xdg configure states（maximized/fullscreen/resizing/最小最大尺寸，native resize 发 configure）**。剩余：popup grab、dmabuf。详见 `HANDOFF_WAYLAND_WINE.md`。
- **输入法 rime 激活（11-05 实测已通）**：`serenityime` addon 已被 fcitx5 加载、socket 协议跑通；`fcitx5-remote -s rime` 切到 rime 后**组合态正常**，LibGUI 候选窗渲染出 rime 的候选（`nihao`→你好/姝好/逆號…），空格上屏。⚠️ 触发坑：rime 已是 `DefaultIM`，再发 Ctrl+Space 会切回 keyboard-us（反而关闭）；脚本用 `fcitx5-remote -s rime` 激活、只打字即可（`Build/vm/ime_show.py`）。
- **显示性能与自适应**：
  - atomic KMS + GBM（dumb buffer + 每帧 memcpy → 零拷贝）未做；**但每帧整屏 memcpy 已于 11-04 改为按脏区逐行 memcpy**（§1.3 damage 条），剩下的"零拷贝"是指把 `m_framebuffer` 直接映射到 dumb buffer（roadmap §5 A）；
  - 热插拔 `take_display_change()` 已实现但**没在真机验证过**；
  - HiDPI `ScreenLayout.scale_factor` 字段已存在，接上即用。
- **托盘 / 任务栏（11-05 复核，前两条已不成立）**：
  - 音量 applet 已在托盘区（`AppletManager` 生效，喇叭图标正常），不再占任务按钮；
  - `serenity-resource-graph` 的 CPU/MEM 图形正常（NET 无流量时是一条贴底黑带，非渲染 bug）；
  - 唯一剩余：桌面图标/开始菜单不支持拖拽，无拖到快速启动栏的 drop 目标。
- **窗口级已知缺陷**：
  - 新窗口与已有窗口**位置重叠**（Serenity WM 不级联），多同款 app 叠一起像「没打开」（判据看任务栏条目数）。
- **「`GUI::Label` 不显示文字」已查明——不是 Label 的 bug**：根因是**根 widget 没有填充背景**（缺 `set_fill_with_background_color(true)`），于是只被透明 `Label` 覆盖的区域显示的是**未绘制的黑色窗口缓冲**，黑字叠黑底 = 看不见；Table/Button/Statusbar 因自绘背景所以正常。gdb + 临时日志证实 `Label::paint_event` 正常、`text()` 非空、尺寸 240×24、字体 glyphW=9.6、前景=黑色（0xFF000000）全部正常；把 Label 矩形用红色填充后文字清晰可见。**修法：根 widget 调 `set_fill_with_background_color(true)`**。WinAppsSettings 已加，exe 路径 Label 正常显示。⚠️ 这是本端口的通用坑：任何根 widget 未填充背景的 app，其透明子控件区域都会露出黑底——排查「Label/文字不见了」时先看这里。
- **TextEditor 未移植**：依赖 LibWebView/LibWeb，`gen-app-targets.py` 直接跳过，快速启动栏里那个按钮点了失败（且输入法 `TextEditor` 客户端已加，但 app 本身不在）。
- ~~**弹出层外圈白边**~~：已随 popup/subsurface 窗口改为透明 + 不铺底色（11-05）消失。
- **桌面 1337 vs VNC 1344**：右侧 7px 黑边——实为 **VNC 帧缓冲（1344）比 guest DRM 模式（1337）宽**，`probe_display_size` 跟随驱动 preferred 模式本身是对的；属显示后端呈现差异，优先级低。
- **清理项**：`Ports/LinuxGUI/InputBridge/`（evdev bridge）现为 libinput 兜底，可评估删除（删前确认 LinuxInput 全目标机能起）；`Common/keymap.h` 保留。

---

## 四、关键命令速查（精简）

```bash
# VM SSH（密钥相对路径注意 cwd）
ssh -i Build/vm/id_ed25519 -p 2222 serenity@127.0.0.1
# 源码整目录同步（铁律：rsync -az --delete 带尾斜杠，禁止多文件平铺）
rsync -az --delete ./ serenity@127.0.0.1:/home/serenity/serenity/ -e 'ssh -i Build/vm/id_ed25519 -p 2222'
# 构建（宿主编译即可；VM 已升 libc6 2.43，产物可直接部署）
cmake --build Build/lagom --target serenity-shell serenity-win-apps-settings -j$(nproc)
# 部署（先停会话防 Text file busy；宿主产物 scp 进 VM 再替换）
sudo systemctl stop serenity-desktop.service
sudo PREFIX=/serenity scripts/port/05-install-prefix.sh
sudo systemctl start serenity-desktop.service
# 截图（正确通道，勿用 QEMU screendump）
kill -USR1 $(pidof serenity-window-server)   # → /tmp/serenity-shot.ppm
```

---

## 源文档索引

| 文档 | 内容 | 最后更新 |
|---|---|---|
| `HANDOFF.md` | 项目定位、总状态、环境命令、已知坑、P0–P5 | 11-01 |
| `QUICK_LAUNCH_HANDOFF.md` | 任务栏快速启动栏修复记录 | 11-01 |
| `HANDOFF_LINUX_APPS.md` | 原生 Wayland 输入、Linux Apps、portal、GTK/libadwaita、Win2000 主题 | 11-02 |
| `HANDOFF_WINAPPS.md` | Windows 程序管理器（WinAppsSettings）进度 | 11-03 |
| `HANDOFF_WAYLAND_WINE.md` | Wine 原生 Wayland：viewporter 已实现并验证、双标题栏修复、剩余 P1 待办 | 11-03 |
| `LINUX_GUI_PORT.md` | 技术细节分章节（环境/脚本/编译流程） | — |
| `HANDOFF_WSWLR_COMPOSITOR.md` | **wlroots 路线权威记录**：§0.11 = 生产 provider 切 wlroots-host、popup configure/菜单位置/内容层合成/连点崩溃的全部根因 | 11-05 |
| `WAYLAND_ROADMAP.md` | Wayland 协议缺口/零拷贝路线图，阶段 0–5 任务拆解（含验收/回滚）；§3.7 = damage 跟踪验收、§5 A″ = WS 直收 shm 方案 | 11-04 |
| `VISTA_THEME.md` | **Vista 复刻权威说明**：系统级主题层、Aero 配色表、开启/关闭主题开关、持久化与验收 | 11-05 |
| `.workbuddy/memory/2026-11-03.md` | Terminal 增强、尺寸 bug、apt SIGCHLD 修复 | 11-03 |
