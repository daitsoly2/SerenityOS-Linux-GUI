# SerenityOS Linux GUI 端口 · 项目总纲（Master Document）

> **生成时间**：2026-11-08
> **来源**：通读本工作区 38 份 Markdown（23 份项目文档 + 9 份 `.workbuddy/memory/` + 6 份 `.workbuddy-ai/memory/`）。**索引只保留仍然作数的 15 份**，废弃/被超越的已剔除，其仍成立的结论并入本文。
> **不含**：`Base/usr/share/man/` 下 2700+ 页上游 SerenityOS man（属发行物，非项目文档）。
> **用法**：唯一索引 + 结论层。查状态看 §5，接手开发看 §3/§6/§10，遇怪现象看 §8，想知道某份旧文档还算不算数看 §2 与 §11。

---

## 0. 一句话说清这是什么

把 SerenityOS 的桌面环境（WindowServer + LibGUI + 62 个应用）**整体搬到 Debian 13 无桌面主机上运行**，外观保持 90 年代 / Windows 2000–Vista 复古风，底层直接用 Linux 现成栈（DRM/KMS、libinput、Wayland、librime、Wine/Proton）。

**核心定位（项目最早定下，全程有效）**：

> **"复古" 指界面复古，不是架构复古。**

由此推出三条始终成立的取舍原则：

1. 优先用成熟组件（libinput / DRM-KMS / Wayland / rime / Wine），不自造底层；
2. 不为"更底层/更原生"写内核模块或替换可用栈，除非有**可测量收益**（唯一例外：`serenity_compat.ko` 提供 `/dev/audio/0` 与 `/sys/kernel/{stats,memstat,net/adapters}`，因为上游 Serenity 应用硬依赖这些路径）；
3. 力气花在**外观层**：主题配色、窗口装饰、候选窗、字体/DPI、图标。

曾因误判成"架构也要复古"写过 evdev→`/dev/mouse/0` 的 bridge，后来被 libinput 取代（`InputBridge/` 现仅为兜底）。

---

## 1. 硬约束（违反会直接砸掉已完成的工作）

### 1.1 ⛔ 合成器只能是 WindowServer + KMS presenter

**绝对禁止**任何 `SERENITY_PRESENTER=wlroots` 的 systemd drop-in（`wlroots.conf`），发现即禁用。
自检：会话日志必须是

```
LinuxScreenBackend: KMS presenter on <W>x<H>
WindowServer: input via libinput
```

出现 `input via embedded wlroots compositor` 就是被切了，必须回滚。
（依据：`HANDOFF_WSWLR_COMPOSITOR.md` §0.10 —— KMS 完全正常，wlroots 路径一移动鼠标就冒黑块，且 EGL 只有 `kms_swrast`。）

> ⚠️ **最容易误读的一条**：`serenity-wlroots-host` 这个**进程**是保留的、且是生产路径。它被禁的只是**"当合成器/presenter"**这个角色。它现在是 **Wayland 协议宿主**（把客户端 surface 桥成 `GUI::Window` 交给 WindowServer 合成），见 §3.2。

### 1.2 🔒 冻结项（DO NOT REVERT）

| 位置 | 内容 | 回退后果 |
|---|---|---|
| `FileManager/main.cpp::run_in_windowed_mode` 的 `on_context_menu_request` | ①函数级 `RefPtr<GUI::Menu> live_context_menu` 每次 popup 前赋值留活；②item 分支回调**必须按值捕获** `[&, primary]` / `[&, paths]` / `[&, paths, primary]`，**绝不可改回 `[&]`** | 回退到 `[&]` 或去掉留活 → 在「菜单打不开」和「点菜单项闪退」之间反复横跳 |
| `GUI::Menu` 的**数据成员** | **不许加**。`C_OBJECT` 的 `construct()` 内联在每个客户端二进制里，改 `sizeof(Menu)` 会让未重编的 app 内存越界 | 全量重编才能救，否则随机崩溃 |
| `GUI::WindowServer.ipc` 的 MessageID | 新消息只能**追加到 endpoint 末尾** | 序号平移 → 旧客户端发错消息 |
| `GUI::Window` 加数据成员 | 禁止；状态放 `Window.cpp` 的 static side table | 成员偏移平移，未重编 app 换主题重建控件即崩 |
| `Compositor` 色键分支 | **不要**在 blit 前 `clear_rect(Transparent)` | 整屏 framebuffer 路径下镂空会变**黑**，圆角凹口全黑 |

### 1.3 两棵树、两条 /serenity（部署头号坑）

| 项 | 路径 | 说明 |
|---|---|---|
| 新树（当前工作区） | `/mnt/user/serenity-linuxgui` | 干净 Lagom 编译树，`Build/lagom` 在这里。**TTPlayer、开始菜单重构只存在于这棵树** |
| 旧树 | `/mnt/user/serenity` | 日常编辑、`Build/vm/vm.sh`、ISO 脚本、`.workbuddy/memory` 都在这 |
| VM | `Build/vm/serenity-mini.qcow2` | QEMU，gtk + VNC:1 + SSH:2222 |
| VM 内源码 | `/home/serenity/serenity` | 与宿主**独立**，必须 rsync 同步 |
| 运行期前缀 | VM 内 `/serenity` | 宿主 `/serenity` 与 VM `/serenity` 是**两个文件系统** |

**只改宿主源码 / 只在宿主重编译，VM 里不会有任何变化** —— 这是"改了没生效"的头号原因。

---

## 2. 文档地图（只列仍然作数的）

> **只收活文档。** 被超越、被否决、已被 `STATUS.md` 完全覆盖的旧交接文档不再列入；
> 其中**仍然成立**的结论已全部并入本文（§1 硬约束、§4 ADR、§5 状态、§8 坑）。
> 需要追溯原始出处时才去翻它们，剔除清单与处置方式见 §11.2。
>
> **已下线的技术同样不再作为方案出现**：外部 IM 框架（含自写 addon、透明代理、守护脚本）、
> 抓帧式 X11 桥、第一代协议宿主、labwc 路线、evdev 输入桥、wlroots presenter。
> 它们只出现在两个地方：§4 ADR 的「被否决的替代方案」列（记录为什么不用），
> 以及 §11.2 的账本（记录哪份旧文档还在说它）。**看到正文提到它们，一律视为历史。**

### 2.1 权威（必读三份）

| 文档 | 行数 | 角色 | 可信度 |
|---|---|---|---|
| **`PROJECT_MASTER.md`**（本文） | — | 归并后的结论层 | ✅ 最新 |
| **`STATUS.md`** | 360 | 「一页速览」状态矩阵 + 已完成/待测/未完成 | ✅ **最权威的状态表**（11-08 更新）。顶部 ⛔ 与 §1.3 wlroots 行并存，按本文 §1.1 理解 |
| **`WAYLAND_ROADMAP.md`** | 469 | 协议缺口、真实数据流、零拷贝三期方案、阶段 0–5 任务拆解（含验收/回滚） | ✅ 技术判断权威；进度停在 11-04，后续看本文 §5 |

### 2.2 活的专题文档

| 文档 | 行数 | 覆盖内容 | 为什么还活着 |
|---|---|---|---|
| `HANDOFF_2026-11-06_STARTMENU_TTPLAYER.md` | 309 | 开始菜单重构（右键删除 ✅ / 拖拽 ❌）、TTPlayer 复刻千千静听的目标与拆解 | **当前主线**：两个 P0 卡点都在这里（§4 拖拽、§5 TTPlayer） |
| `HANDOFF_2026-11-07_WLROOTS_IME.md` | 393 | WlrootsHost × winit 崩溃修复全链、text-input 缺失根因分析 | §2 的 decoration 断言修复仍是有效代码；§4 的"两条输入法路径互不相通"分析无可替代 |
| `HANDOFF_ICONS_2026-11-07.md` | 187 | 统一图标解析 + 文件类型图标全表 | 唯一入口 `PortIcons`/`PortShortcuts` 的设计依据 |
| `HANDOFF_PRINT_2026-11-07.md` | 126 | 打印服务、页面合成下沉到服务端、通知、打印对话框 | 打印子系统唯一文档；属性页 UI 尚未验收 |
| **`VISTA_THEME.md`** | 162 | Vista 复刻的系统主题管线、Aero 配色表、开启/关闭开关、持久化、**§6.1 KMS 单份帧内容修复** | 外观层唯一权威说明（`STATUS.md` 多处引用它） |

### 2.3 技术长文

| 文档 | 行数 | 内容 |
|---|---|---|
| `README.md` | — | **仓库首页（中文）**：移植说明、截图、能力概览、架构一张图、快速开始、文档导航、许可与派生声明 |
| `LINUX_GUI_PORT.md` | 989 | 移植技术细节分章：环境、脚本、编译流程、serenitid、登录、翻译/字体、`/serenity` 前缀、汉化、Win 应用、浏览器、输入法、`serenity_compat` 内核模块、Ctrl+Alt+Del、Wayland 客户端、主题桥、输入路径、**§7 上游改动表** |

（`README.upstream.md` 是上游 SerenityOS 原文留档（作者/特性/构建），`CONTRIBUTING.md` /
`SECURITY.md` 亦为上游原样，均不在本项目维护范围。）

### 2.4 组件说明（按需查）

| 文档 | 行数 | 内容 |
|---|---|---|
| `Ports/LinuxGUI/SCREENSHOT_RECORDER.md` | 312 | 截图/录屏服务；**顶部是 ⛔ wlroots presenter 禁令原文** |
| `Ports/LinuxGUI/WlrootsHost/VENDORED.md` | 88 | vendored wlroots 0.20.2 协议版是怎么裁的（关掉全部 backend/renderer） |
| `scripts/port/README.md` | 129 | 端口脚本清单 |

### 2.5 记忆（两套隐藏目录，内容互补）

| 文件 | 性质 |
|---|---|
| `.workbuddy/memory/MEMORY.md`、`.workbuddy-ai/memory/MEMORY.md` | **跨会话必带**。前者是架构/坑/未完成浓缩；后者独有 ⛔ 合成器铁律、部署通道、TTPlayer 皮肤 6 条、托盘子控件化、图标铁律 |
| `.workbuddy/memory/2026-11-0X.md`（8 天）、`.workbuddy-ai/memory/2026-10-31…11-07.md`（5 天） | 按日流水。**只在追溯"这个结论是怎么来的/某天到底改了什么"时读**。11-01/11-02 记的多是已解决的历史（Lagom 打通、KMS、libinput、shell 合并、IME 第一版） |

---

## 3. 架构

### 3.1 数据流（真实路径，非理想图）

```
Wayland 客户端 / X11(Xwayland) / Wine / 原生 Serenity app
   │
   ├─ serenity-wlroots-host（独立进程，协议宿主）
   │    shm buffer ──► Gfx::Bitmap ──► SurfaceWidget ──► GUI::Window
   │    （damage 合并 + 只拷脏区，8ms pump 节流 frame 回调）
   │
   ▼
WindowServer（LibGUI CPU painter，**唯一合成层**）
   窗口位图 ──► m_framebuffer（双缓冲）      [拷贝 2]
   │
   ├─ LinuxScreenBackend::present_buffer_kms()
   │    按脏矩形逐行 memcpy ──► DRM dumb buffer mmap   [拷贝 3]
   ▼
drmModePageFlip ──► CRTC scanout
```

**三次拷贝**是已知性能债（1920×1080×60fps ≈ 500MB/s 内存带宽），零拷贝分期见 `WAYLAND_ROADMAP.md` §5（A 输出端 / A″ WS 直收 shm / B dmabuf+GBM）。

### 3.2 进程模型

`serenitid`（唯一系统服务，装在 **`/serenity/libexec/serenitid.real`**）按序拉起并用 SIGCHLD 监管：

```
config-server → launch-server → file-system-access-server → window-server
→ login-server → serenity-wlroots-host → shell(桌面+任务栏) → serenity-engine(IME)
```

- 崩溃重启退避 200ms×n，上限 20 次；`always_restart` 标记用于"干净退出也要拉回"的子进程。
- **FSAS 必须 `always_restart`**：否则 WS 崩溃后它干净退出，留下 stale portal socket → **所有** app 的 File/Open/Save 在 `Client::the()` 里 VERIFY abort。
- serenitid 环境已设：`XMODIFIERS=@im=serenity`、`unset GTK_IM_MODULE/QT_IM_MODULE`（这两个变量会去连宿主的 GTK/QT IM 模块，端口里没有，结果是**吞键**）。

### 3.3 关键源码落点

```
Ports/LinuxGUI/
├── main_linux.cpp              # WindowServer 入口：布局/输入/事件泵/退出
├── LinuxScreenBackend.{h,cpp}  # KMS 呈现器（dumb buffer+page flip）/ SDL 回退 / 模式探测
├── LinuxInput.{h,cpp}          # libinput(udev/seat0)
├── Common/
│   ├── hostify.h               # /bin/Name → serenity-<kebab>（唯一一份）
│   ├── keymap.h                # evdev → Serenity KeyCode
│   ├── AppIcons.h              # PortIcons：唯一图标解析器
│   ├── AppShortcut.h           # PortShortcuts：.af/.desktop 归一
│   ├── ExplorerEntries.h / ExplorerMenus.h / ArchiveOps.h / WinApps.h / PortPrint*
├── Shell/                      # 桌面外壳（Desktop+Taskbar 单进程，explorer.exe 模式）
│   ├── AppCatalog / DesktopIconsWidget / SystemMenu / StartMenuShortcuts / StartMenuOverrides / TrayAreaWidget
├── WlrootsHost/                # Wayland 协议宿主：wlroots_proto.c / main.cpp / ImeService / XimServer / tests
├── IMEEngine/serenity-engine.cpp   # 直链 librime 的引擎（**IME 主链路，唯一**）
├── IMESettings/                # serenity-ime-settings（托盘/控制面板入口）
├── SessionManager/             # serenitid
├── Services/                   # config / launch / clipboard / file-operation / FSAS / audio / print
├── SystemMonitorLite/          # 任务管理器（Win7 对齐）
├── WinAppLauncher/             # serenity-winapp（原生 C++，无 Serenity 库）
├── Notepad/ VideoPlayer/ PrintSettings/ ...
└── Apps/generated-apps.cmake   # 应用目标表（脚本生成，**新增 .cpp 必须两处都加**）
scripts/port/
├── gen-app-targets.py          # 生成 generated-apps.cmake
├── 05-install-prefix.sh        # 装运行期到 /serenity（--systemd / --clean）
├── 06-install-host.sh          # 真机安装
├── serenity-theme.sh / serenity-theme-gen.py
├── gen-filetype-icons.py       # 文件类型图标生成（纯 Python，无需 Pillow）
└── session-control.sh          # logout/shutdown/reboot
```

---

## 4. 关键架构决策记录（ADR）

| # | 决策 | 理由 | 被否决的替代方案 |
|---|---|---|---|
| D1 | 桌面外壳合为单进程 `serenity-shell` | 共享一次 app 扫描 + 图标缓存、右键菜单状态统一、少一进程/IPC | 保留 desktop-icons + taskbar 两进程 |
| D2 | 运行期装 `/serenity`，`/res`、`/etc/WindowServer.ini`、`/etc/serenity` 是指向它的**软链** | 与宿主系统目录隔离；代码里写死这三个路径 | 装 `/usr/local`（已清空） |
| D3 | X11 走 **Xwayland + 自研 XWM**，不用抓帧式桥 | 抓帧桥（XComposite + XGetImage）性能差、mask=0 会死循环、98% CPU | 抓帧式 X11 桥（11-02 退役）、第一代协议宿主（11-05 退役） |
| D4 | Wayland 协议层交由 `serenity-wlroots-host`（vendored wlroots 0.20.2 **协议版**） | 手写协议栈成本过高；vendored 只编 types/util/protocol，0 个 backend/renderer 符号，许可证干净 | 自研 `wayland_host.cpp` 全协议（缺 subcompositor 直接崩 Firefox） |
| D5 | ⛔ **合成器/presenter 只能是 WS + KMS** | wlroots presenter 在无真 GL 的环境黑块 + 无限重启 | `SERENITY_PRESENTER=wlroots`（见 §1.1） |
| D6 | 装饰**强制 SERVER_SIDE** 复古框，`GTK_CSD=0` | 用户拍板"窗口必须强制套复古边框" | 尊重客户端 CSD（11-04 上午短暂采用后回退）；Wine 例外 frameless |
| D7 | 输入法 = **`serenity-engine` 直链 librime**；三条前端共用 `/tmp/serenity-ime.sock` 行协议 | 少一层 addon；引擎不依赖任何显示服务，Xwayland 重启不会带走它 | 旧方案「外部 IM 框架 + 自写 addon + 透明代理」整链下线（会随 X11 连接断开而整树退出） |
| D8 | IME **开关放引擎服务端**（全局 `g_active`，非 per-client） | 一处拦截三条前端通吃，不必给 LibGUI/桥/XIM 各写状态同步；Windows 语义（切窗口保持英文） | 各前端各自维护状态 |
| D9 | 候选窗用 `WindowType::Autocomplete`，不用独立 `Window`/`Popup` | `Popup` 会让 WS 调 `notify_active_window_input_preempted()` → 宿主 focusout → 引擎 reset；`Autocomplete` 明确不抢焦点 | 独立窗口（"点候选抢焦点"）、编辑器内 overlay |
| D10 | 图标/快捷方式**唯一入口** `PortIcons` + `PortShortcuts` | 桌面/文件管理器/开始菜单/快速启动原先各写一份，都不认 XDG 主题名、`.ico`、exe 内嵌图标 | 各自 `Bitmap::load_from_file` |
| D11 | 宿主分支进 `LibGUI/FileIconProvider.cpp` | 让**所有** GUI 客户端自动获得宿主图标能力 | 在每个 app 里接 |
| D12 | Windows：`game → umu-run + DWProton`，`app → 原版 wine 11.0` | Proton 是游戏向分支，对办公/安装器是更多可破坏面 | 全走 DWProton（旧行为，两头不占好） |
| D13 | `serenity-winapp` 改**原生 C++** | 策略内置代码，对齐 omikuji 的 `assemble_launch`；sh 只用于 pre/post hook | sh 脚本 |
| D14 | 翻译挂在 **LibGUI 文本入口**（构造函数 + setter），不改 app | 700+ 条改 ini 免重编 | app 内逐个替换 |
| D15 | 搜索范围限定「用户目录 + 用户显式添加目录」 | 全盘搜索无意义且慢 | 全盘 |
| D16 | TTPlayer 照搬**原版 `Skin.xml` v2 格式**，不整体移植 TTPlayerRebuild | 那个项目 10 万行 Win32/GDI，`if(NOT WIN32) FATAL_ERROR`；只搬 `src/skin/*` 这层数据/绘制规则最划算 | 移植 M4Player(wx+GTK2) / TTPlayerRebuild |

---

## 5. 子系统状态矩阵（截至 2026-11-08）

### 5.1 ✅ 已完成（代码 + VM 实测）

| 子系统 | 结论 |
|---|---|
| 显示 DRM/KMS | dumb buffer + page flip，preferred 模式优先，1337×717→1872×984 自适应 |
| 输入 libinput | 移动/点击/键盘全通；绝对坐标 16 位、相对亚像素累积、按钮补发缓存位置 |
| 桌面外壳 | `serenity-shell` 单进程（桌面+任务栏）；框选多选、Ctrl+点击、批量删除/复制 |
| 应用 | 62/62 目标构建，装 `/serenity`；`/bin/Name`→`serenity-<kebab>` 统一转换 |
| 汉化 | LibGUI 运行时 `[UI]` 表 793 条，改 ini 免重编 |
| 主题 | Win2000 三套模板（GTK3/GTK4/libadwaita）+ **Vista 双态实测通过**（`serenity-theme.sh {vista\|classic}`） |
| 文件管理器 | Vista/Win7 化：命令栏/地址栏/导航/预览/中文菜单/搜索过滤；tar/7z 任意归档解压 |
| Wayland 协议宿主 | `serenity-wlroots-host` 生产路径：Firefox 启动/渲染/☰ 菜单全链、GTK4 打字 |
| 帧回调节流 | 8ms pump 按 ~16ms 批量 done —— **直接修掉 Firefox 原生 Wayland 冻结** |
| damage 跟踪 | 空闲 commit 760k px → 几百 px；KMS 按脏矩形 memcpy + 双缓冲跨帧结转 |
| xdg_popup | grab / 外部点击关闭 / FLIP+SLIDE 定位（`anchor`/`gravity` 是**组合枚举 0–8，不是位标志**） |
| 键盘焦点模型 | 按 mutter 50.5 对照重建：焦点只在 press/map/popup-dismiss 变更；只给 toplevel；必须发 ACTIVATED |
| 剪贴板 | Wayland 内部 `wl-copy`↔`wl-paste` 通；系统级 `serenity-clipboard-server` + Win+V 历史 |
| 输入法（原生） | 候选窗 Serenity 配色 + 内联预编辑 + 模糊音 5 组 + 9 候选 + 设置界面即时生效 |
| 输入法（Linux 客户端） | 桥 → 引擎 → text_input_v3.commit_string 上屏，gedit/WordCraft/titest 实测；**含 Serenity 风格候选窗** |
| 输入法（X11 客户端） | XIM：`@server=serenity` 注册、IC 焦点、行高自适应、commit 投递 |
| Ctrl+Space 开关 | 引擎服务端 `g_active`，28/28 行协议断言通过（**⚠️ 未上 VM 做 GUI 端到端**） |
| 任务管理器 | Win7 严格对齐：6 Tab / 5 菜单 / 3 段状态栏 / 进程树 / 内核进程合并（197→45）；Ctrl+Alt+Del 不再重启 |
| 图标 | 50 图标族 / ~600 模式；`filetype-*` 12 个新图标；快捷方式红叉误判已修 |
| 打印 | 服务端页面合成（PDF→pdftoppm 150dpi）、8 字段协议、系统通知、VM 端到端全绿 |
| Windows 应用 | `serenity-winapp` 原生；auto 判 app/game（PE 导入表 + 引擎标记）；注册表 `winapps.ini` 查模式 |
| 退出链路 | logout(SIGUSR1+execv)/shutdown/reboot + polkit 规则；`cad_pid` 由 ExecStartPost 写入 |
| Notepad / 视频播放器 | 原生 LibGUI；播放器"有声音没图像"根因 = 控件只登记布局、没 `add_child()` |

### 5.2 🟡 待测试 / 待补验收

| 项 | 缺口 |
|---|---|
| **Ctrl+Space** | 引擎协议级 28/28，但 **VM 当时 CLOSED**，未在 notepad/gedit/XIM 做 GUI 实测（托盘「中/EN」同步、英文直输、候选擦除） |
| 打印属性对话框 | 从未真正点开过；apply 后刷新/选项应用/配额写入/错误路径未手测 |
| X↔Wayland 剪贴板互通 | 代码就位未实测；Serenity `GUI::Clipboard` 双向桥未做 |
| Wine 中文字体 | FontSubstitutes 逐条 `reg add` 已修，未用真实中文 exe 目测 |
| 滚轮/悬停手测 | pointer_axis 已路由，Firefox/xterm 滚动只做过代码走查 |
| 退出链路 | 代码就绪，未在最新改动后完整回归 |
| 真机安装 | `06-install-host.sh` 就绪，**未执行** |
| Settings 控制面板 | 本机构建通过，未部署验证；Display 项已接通 EDID |

### 5.3 ❌ 未完成 / 已知缺口

按优先级（合并 STATUS §3 + roadmap §6 + 各 handoff 遗留）：

**P0 / 当前卡点**
1. **开始菜单拖拽归类未生效** —— 卡在 WS 没把 drag 事件派发出来。首要怀疑 QMP 注入的 MouseMove 不带 Primary 按钮位（`MenuManager.cpp:231-234` 直接把状态清了）；次怀疑 QMP abs 有 +23px y 偏移。排查顺序见 `HANDOFF_2026-11-06_STARTMENU_TTPLAYER.md` §4.4。
2. **TTPlayer 真实皮肤四窗口全黑** —— `SkinDoc` 解析成功，但 `SkinPanel::paint_event` 完全没被调用；日志有 4 条 `Failed to parse IPC message`（消息体含 GBK「经典皮肤」字节）。怀疑客户端/WS IPC 格式错位或 `liblagom-gui.so` 未同步。
3. **桌面「打开方式」导致会话重启** —— 仅对 WinApps 生成的 `.desktop`，仅桌面（FM 不复现）。线索在 `.workbuddy/memory/2026-11-06.md`。

**P1（体验/正确性）**

- 托盘：桌面图标/开始菜单**不支持拖拽**，无拖到快速启动栏的 drop 目标（唯一剩余缺口）
- 窗口管理：新窗口与已有窗口位置重叠（WS 不级联）
- popup 生命周期：菜单关闭后偶见残留黑块；点桌面/任务栏不关菜单；serial 校验与 `NOT_THE_TOPPMOST_POPUP` 未实现
- XIM **行内预编辑**未做（客户端侧画拼音）；部分 Wayland 客户端 `ti_active=0`
- Wine：popup grab、dmabuf 未做
- damage：一帧多矩形仍合并成包围盒（gedit 单次 commit 仍 82%）；不发 damage 的客户端全整幅拷贝

**P2（性能/平台，真机才有结论）**

- 阶段 2：`LinuxScreenBackend` 直接画进 dumb buffer（零拷贝 A）、异步 page flip
- 阶段 1.2/1.3：output 事件现代化、多 output + `xdg_output` + fractional-scale
- 阶段 3：`zwp_primary_selection` / `xdg_activation_v1` / `shortcuts_inhibit` / `wl_touch` / presentation-time
- 阶段 4：dmabuf + GBM + GPU 合成（**开工前必须先拿到真机 A/B 数据**）
- 热插拔真机未验证；HiDPI `scale_factor` 字段已存在未接

**P3（清理）**

- `Ports/LinuxGUI/InputBridge/`（evdev 兜底）可评估删除；`Common/keymap.h` 保留
- TextEditor 未移植（依赖 LibWeb），已用原生 Notepad 替代
- deb 打包未开始；1600×900 live ISO 未重建

---

## 6. 时间线（决策与事故史，比现状更有信息量）

| 日期 | 关键进展 | 关键否决/回滚 |
|---|---|---|
| **10-31** | 另一路会话开工 | — |
| **11-01** | Lagom 构建打通 → VM 部署 → KMS presenter 取代 SDL → libinput 取代 evdev bridge → shell 合并 → `/serenity` 前缀 → 汉化 793 条 → X11 桥 → 62/62 app | 「架构也要复古」的错误方向（evdev bridge）被 libinput 取代 |
| **11-02** | IME 体系重建（候选窗重新设计、`zwp_text_input_v3` 开通、行协议定型）、桌面卡死修复（阻塞 read → poll 30ms）、DisplaySettings 接真 EDID、自建 `serenity-portal`（gedit 出窗 18s→1.0s）、Win2000 主题 + libadwaita | labwc 路线评估后放弃；抓帧式 X11 桥退役 |
| **11-03** | Terminal 增强 + SIGCHLD 修 apt、`serenity-winapp` 原生 C++、退出链路、真机安装脚本、Wine viewporter 落地 + 双标题栏修、Xwayland 融合进协议宿主 | 真机黑屏根因 = plymouth 占着 DRM master |
| **11-04** | Firefox 原生 Wayland 冻结 → 帧节流修好；damage 跟踪；`xdg_popup.grab`；**按 mutter 50.5 重建键盘焦点模型**；wlroots 内嵌合成器尝试 | **wlroots presenter 因黑块回滚** → 定案「WS 唯一合成层」 |
| **11-05** | 生产会话切 `serenity-wlroots-host`；popup 首帧 configure + 真实点击链路四修；候选窗对比度/内联预编辑；方案 A（GTK immodule）；任务管理器 Win7；Vista 主题双态；FM Vista 化 | 「3 次/12 次压测全过」被认定为假阳性（恢复对话框）；「待 weston 对照」被用户否掉 |
| **11-06** | 桌面框选；FM 右键菜单**冻结**；视频播放器根因；FSAS 从未部署 → 全端口 Open 修复；vendored wlroots 协议版；开始菜单重构（右键✅/拖拽❌）；TTPlayer 皮肤引擎起步 | ⛔ 用户明确：合成器只能是 WS/KMS |
| **11-07** | 图标统一；打印服务；winit 崩溃真因 = **xdg-decoration 同步 schedule_configure**；text-input 缺失根因定位；IME 三阶段收束（桥内建 → XIM 前端 → 服务化），**最终定为引擎直链 librime** | 外部 IM 框架整链裁掉（会随 Xwayland 重启退出） |
| **11-08** | Ctrl+Space 开关（引擎服务端全局 `g_active`），28/28 断言 | — |

**三次"看起来好了其实没好"的教训**（值得记住）：
1. Firefox 菜单压测用同一个 profile → 反复 pkill 会弹恢复对话框，造成假阳性。必须**每轮全新 `-profile`**。
2. 旧 STATUS 说"3 次/12 次全过"就是这样来的。
3. 部署被覆盖：`serenity-file-manager` 09:57 被一次重新部署换成旧二进制（2098792 vs 修复版 2182552），右键菜单又没了。**未提交的修复一旦从干净源码构建就丢失**。

---

## 7. 部署 / 验证约定

> 原始文档里构建命令已多轮过时（`.workbuddy-ai/memory/MEMORY.md` 明确指出）。以下为当前有效流程。

```bash
# SSH（-F /dev/null 必需：宿主 ssh_config.d 属主异常）
ssh -F /dev/null -i Build/vm/id_ed25519 -p 2222 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null serenity@127.0.0.1

# 源码同步铁律：整目录带尾斜杠，禁止多文件平铺（曾把 Apps/CMakeLists.txt 覆盖到父文件）
rsync -az --delete ./ serenity@127.0.0.1:/home/serenity/serenity/ -e 'ssh -i Build/vm/id_ed25519 -p 2222'

# 宿主构建（VM libc6 已升 2.43，产物可直接部署）
cmake --build Build/lagom --target <target> -j$(nproc)

# 部署（三种"文件在跑"的处理）
sudo systemctl stop serenity-desktop.service   # 或
sudo install -m755 x /serenity/bin/x.new && sudo mv -f x /serenity/bin/x   # rename 可覆盖运行中文件
# 桥/看门狗进程：按 /proc/*/exe **case 通配** 杀（mv -f 后老进程 exe 变 "…(deleted)"，全等比较匹配不到）

# 截图：一律 VNC，勿用 QEMU screendump
python3 Build/vm/vncshot.py "shot:/tmp/x.ppm" && convert /tmp/x.ppm x.png
python3 Build/vm/vncshot.py "click:x,y"        # QMP 绝对坐标，比 poke 稳
```

**部署后必须四者一致**：源码 md5（宿主 vs VM）、构建产物 md5、`/serenity/bin` 二进制 md5、`sudo md5sum /proc/<pid>/exe`。

**全量部署的三处漏点**：
1. `serenitid` 在 **`/serenity/libexec/serenitid.real`**，不在 `/serenity/bin`；
2. `liblagom-*.so` 要同时进 `/serenity/lib` **和** `/usr/local/lib` + `ldconfig`（桥的 rpath 是 `$ORIGIN/../lib` = `/serenity/lib`，只更新一处会出现 `symbol lookup error` → 桥 127 退出 → 所有客户端 Broken pipe）；
3. `Base/etc/FileIconProvider.ini` 与 `Base/res/icons/**` 不会自动进 VM，要手工部署。

---

## 8. 通用坑速查（跨文档去重合并）

### 8.1 LibGUI / AK

- **根 widget 必须 `set_fill_with_background_color(true)`** —— 否则透明 `Label` 区域露出未绘制的黑窗口缓冲，黑字叠黑底 = "文字不见了"。
- **`layout->add_widget(*w)` ≠ `add_child(*w)`** —— 前者只登记布局（有几何、`is_visible()` 为真），**不绘制、不响应输入**。视频播放器"有声音没图像"就是这个。
- `GUI::ItemListModel` 存 `Container const&` 引用，vector 必须活得比组件久，临时量 → SIGSEGV。
- `Gfx::Rect::right()/bottom()` 是**排他**边界；逐像素拷贝循环用 `<`，`<=` 会触发 `Bitmap` 断言把桥打死。
- `Gfx::Color::darkened/lightened` 是**乘系数**（0.5 才变暗），且 `darkened` 不做 clamp：对浅色 `darkened(1.15)` 会让 >255 通道**回绕**（浅蓝 → 纯红）。皮肤/主题色一律用显式字段。
- `String::formatted` 返回 `ErrorOr<String>`；`String::matches` 是**整串 glob**（`Makefile` 匹配不到路径，要 `*Makefile`）。
- `AK::String` 对 `StringView` 是**显式**构造；`ByteString(StringView)` 是隐式；`ByteString::view()` 有 `const&&` 重载被 delete，不能对临时量调。
- `AK` 没有 `Pair<K,V>` 泛型；`ByteString::formatted("{:.0f}", int)` 会 VERIFY 崩（SIGILL）。
- `Menu::popup()` 是**异步**的：栈上 `Menu` 会先析构 → `invalid menu ID`。必须 `RefPtr<Menu>` 留活；回调捕获 `[&]` 会悬垂。
- `Menubar`（不是 MenuBar）；`PaintEvent`/`MouseEvent` 都在 `<LibGUI/Event.h>`；鼠标按下回调是 `mousedown_event`（小写 d）；`ButtonStyle` 在 `Gfx::`。
- `GUI::Window` 子控件里，`WindowType::Popup` 会让 WS 抢焦点（`notify_active_window_input_preempted`）；候选窗要用 `Autocomplete`。

### 8.2 构建 / 部署

- `generated-apps.cmake` 是 `gen-app-targets.py` 生成的：**新增 .cpp 只改 app 的 CMakeLists 不会进链接**（一堆 undefined symbol / "no work to do"），必须两处都加。
- `ninja -C Build/lagom serenity-engine` 会触发 CMake 重配并被 `Ports/LinuxGUI/CMakeLists.txt:33` 的 `sdl2` pkg-config 硬要求挡死 → 用 `compile_commands.json` 手工编链接。
- `Text file busy`：stop → rm → cp → start，或 `.new` + `mv -f`。
- `pkill -x` 对 >15 字符进程名无效（comm 截断）；`pkill -f <名字>` 会**杀掉执行它的 ssh 自己**（命令行里含同字符串）。
- zsh 不对未加引号的变量做词分割：`ssh $SSHARGS` 会把整串塞给 `-i`。用 `${=SSHARGS}` 或内联。
- VM 时钟是 UTC（比本地慢 8 小时），`ninja: no work to do` 时先确认源文件真的比 `.o` 新。
- `systemctl set-environment` 对 unit **无效**（unit 自带 `Environment=` 不继承 manager 环境），要写 drop-in。

### 8.3 Wayland / 合成器

- **「所有 Linux 应用突然都打不开」先查桥，不要先怀疑应用**：`serenity-wlroots-host` 若因缺共享库
  以 `status 127` 退出，serenitid 退避重启 7 次后打印 `crashed too often, giving up` 并**静默放弃**，
  之后 WordCraft / gedit / Firefox 全部无窗口、无报错。典型触发：桥用 XIM 前端链了
  `libxcb-imdkit.so.1`，而 VM 里这个包没装（`apt install libxcb-imdkit1`）。
  判据：`grep "wayland-host exited" /tmp/serenity-session.log` + `ldd /serenity/bin/serenity-wlroots-host | grep "not found"`。
  修复后必须 `systemctl restart serenity-desktop.service`（因为已经 give up，杀单个进程不会触发重启）。
- 键盘焦点是 **seat 级状态**：`keyboard_key()` 只按 `current_keyboard_focus_resource()` 投递；焦点只在 press/map/popup-dismiss 变更（**绝不能在 press/release 之间换焦点**）。
- 焦点只给 toplevel（先 `surface_topmost_ancestor()`），必须发 `XDG_TOPLEVEL_STATE_ACTIVATED`，且只属于 owning toplevel。
- 只有调过 `xdg_popup.grab` 的 popup 取焦点（Firefox XUL 菜单**不调** grab）；回滚开关 `SERENITY_POPUP_FOCUS_ALL=1`。
- `anchor`/`gravity` 是**组合枚举 0–8**，不是位标志；`&` 解析会算出 `configure(-99,-173)`。
- `wlr_xdg_surface_schedule_configure` **断言 `surface->initialized`**，该标志只在角色 commit 里置真。winit 首帧在首次 `wl_surface.commit` **之前**就 `set_mode(CLIENT)` → 桥里同步调 `wlr_xdg_toplevel_decoration_v1_set_mode()`（内部即 schedule_configure）必 abort。修法 = 推迟到 idle，`!initialized` 再排一轮。**教训：改前先确认断言的调用链，别猜**（第一次猜 `on_surface_commit`，改了没用，真凶是 decoration）。
- `wl_pointer.frame` 不会自动发，要手工发，否则**按下不弹起、第一次点击只按下**。
- 输入坐标必须反算 `window_geometry`（GTK 阴影 26×23），否则点击整体偏上落到标题栏触发 resize。
- `enter` 不能写死 (0,0)；`xdg_surface` 首 commit 的 configure 尺寸发 `0x0`，别用 640/480 兜底。
- X11 中文标题只设 `_NET_WM_NAME`（UTF8_STRING），`XFetchName` 拿不到 → 必须优先读它并监听 PropertyNotify。
- EWMH 窗口属性是**异步**的：Firefox 冷启动先 map 后设 WM_NAME，map 时刻 title 为空不能永久丢弃（否则白板 + "Not responding"）。
- XGetImage 对 Composite pixmap 返回的 mask 可能是 0 → `while(mask>>shift)&1` 死循环 98% CPU；所有掩码移位必须有 0 兜底。

### 8.4 输入法

- 三条前端（LibGUI `InputMethod` / 桥 `ImeService` / `XimServer`）都发 `key <keysym> <states> <release>` 到 `/tmp/serenity-ime.sock`，引擎统一处理。
- `handle_key()` 必须在 `ensure_session()` **之前**拦 Ctrl+Space —— 关闭状态下可能没有可用 session，而这时最需要能重新打开。
- 修 `handle_control()` 里 `take_token()` 会把 pos 停在命令后的空格上，`rest` 带前导空格，**比较前必须 trim**。
- `RIME_STRUCT(Type,var)` 必须用宏初始化（`data_size=0` 时 API 忽略所有字段，`user_data_dir` 退化成 `./`）。
- rime `build/luna_pinyin.table.bin` 缺失时词典为空 → 候选全是单字硬凑（出现一串「啊」）。
- 引擎进程要 **unset DISPLAY / WAYLAND_DISPLAY**，否则 Xwayland 重启会连带把输入法整棵树带走。

### 8.5 截图 / 注入

- **截图一律 VNC**（`vncshot.py`）；QEMU `screendump` 对 virtio-vga 会错位斜切（报 1539 实际 1552）；`/dev/fb0` 是陈旧 fbdev 缓冲，停在开机 splash。
- QMP `input-send-event` 的**键盘**事件在该 VM 配置下根本到不了 guest（`/dev/input/event5` 零事件）→ 键盘必须走 VNC RFB 或 HMP `sendkey`。
- QMP abs 注入要用**单个持久 socket**（多次短连接会让 guest 指针漂移）；坐标按**真实分辨率**换算（不同时期 1552×909 / 1600×900 / 1872×984）。
- 相对指针要 `SERENITY_POINTER_FLAT_ACCEL=1` drop-in，否则 libinput 加速会让第二次 delta 被缩放。
- `titest`/诊断脚本的 stdout 重定向到文件时是块缓冲，必须 `stdbuf -oL`。

### 8.6 运维

- 换内核后必须重编 `serenity_compat.ko`（6.11+ `from_timer`→`timer_container_of`、`del_timer_sync`→`timer_delete_sync`），否则 audio-bridge 崩溃连锁拖垮观感。
- `Core::command()` 在程序不存在时会 `VERIFY` abort → 调外部工具前先用 `PortIcons::tool_on_path()` 解析。
- `Core::deferred_invoke` 在事件循环已销毁时会 `EventLoop::current()` 越界 abort（"停服务=崩溃"）→ 先判 `EventLoop::is_running()`。
- 构造期调 `set_applet_area_position` 会被 WM 判 misbehaved 断开 → shell 反复重启。
- 任务栏 `skip_taskbar` 查询必须传 `client_id + window_id`。

---

## 9. 环境备忘

| 项 | 值 |
|---|---|
| 宿主 | Debian 13（无桌面），glibc 2.43，clang + `-Werror` |
| VM | Debian 13 genericcloud + `linux-image-amd64`（云内核**没有** DRM/fb 驱动），libc6 升 2.43，内核 7.2.13-liquorix |
| VM 显示 | virtio-vga 1600×900（历史 1552×909 / 1872×984），KMS preferred 模式跟随宿主窗口 |
| 图形栈 | DRM/KMS dumb buffer；EGL 只有 `kms_swrast`（无真 GL） |
| 输入法 | librime 1.13.1（`librime-dev`），`/usr/share/rime-data`，schema `luna_pinyin` |
| Windows | wine 11.0（tuna wine-builds 源，2 分钟 vs 官方 1 小时+）+ umu-run + DWProton |
| 网络 | QEMU user-mode，guest 里 `10.0.2.2` = 宿主；VM→宿主实测 ~129 MB/s |
| 字体 | Sarasa Gothic SC / Sarasa Term SC（LibGfx 无字形回退链，CJK 必须设成默认字体本身） |

---

## 10. 下一步建议顺序

1. **收尾 Ctrl+Space 的 VM 端到端实测**（唯一"代码完成但没验"的 P0 功能，成本最低）。
2. **开始菜单拖拽**：加 dbgln 确认 `mouse_event.buttons()` → 改 `m_item_drag_button_down` 布尔 → 坐标校准 → 双场景验证。
3. **TTPlayer 全黑**：先核对 VM 上 `liblagom-gui.so` 与 WS 是否同源，再查含 GBK 字节的 `set_window_*` IPC 消息；确认 `SkinPanel::paint_event` 是否因窗口未 map 而不跑。
4. 桌面「打开方式」崩溃（会话重启，影响面最大）。
5. 打印属性对话框手测 → 剪贴板 X↔Wayland → Wine 中文字体目测。
6. 真机：`06-install-host.sh` + 零拷贝 A 的 A/B 数据（决定是否值得上 GPU）。
7. 1600×900 live ISO 重建（注意 `iso/` 是 root 所有、`mksquashfs` 必须 sudo，否则静默重封旧 squashfs）。

---

## 11. 文档债务

### 11.1 🔴 引用存在但文件缺失

| 被引用 | 引用处 | 处置 |
|---|---|---|
| `Documentation/labwc-route.md` | `HANDOFF_LINUX_APPS.md` §10 | 路线已否决，不必补；引用改为「已废弃」 |
| `HANDOFF_2026-11-06.md` | 多份文档互引 | 实际文件名是 `HANDOFF_2026-11-06_STARTMENU_TTPLAYER.md` |

> `VISTA_THEME.md` 曾一度从工作区消失（被误删），但一直在 git 初始 commit 里，已用
> `git checkout -- VISTA_THEME.md` 恢复，现为 §2.2 的活专题文档。**教训：报"文档缺失"前先
> `git ls-tree HEAD` 查一下，别急着重写。**

### 11.2 🟠 已剔除出索引的文档（不再作为依据）

这些文档**已不参与本文任何结论**，仅在其承载了不可替代的原始记录时保留出处：

| 文档 | 为什么剔除 | 仍需取用的孤点 |
|---|---|---|
| `HANDOFF.md` | 430 行里 60% 与 `STATUS.md` 重叠；§2/§3 的构建部署命令已过时 | **§0 项目定位**（"界面复古≠架构复古"已抄入 §0）、§3 早期已知坑 |
| `HANDOFF_WSWLR_COMPOSITOR.md` | wlroots presenter 已否决，实现细节全废 | **§0.10 黑块根因**（⛔ 铁律的唯一技术依据）、§0.11「WS 唯一合成层」决策 |
| `HANDOFF_WAYLAND_WINE.md` | 针对已退役的 `WaylandHost`，现由 wlroots 提供协议 | §3.1 Wine 协议依赖矩阵（判断 Wine 起不来时仍可查） |
| `HANDOFF_WINAPPS.md` | WinAppsSettings 已部署，结论并入 `STATUS.md` §1.5/§2.5 | — |
| `HANDOFF_LINUX_APPS.md` | 多数章节针对已退役桥；结论并入 `STATUS.md` | **§12 视频播放器根因**、§12.4 FSAS 全端口修复（均已抄入 §8.1） |
| `HANDOFF_2026-11-07_BRIDGE_IME.md` | 旧 IM 框架（addon）链路已被 11-08 的「引擎直链 librime」取代 | §2 里四条实测证据（WordCraft 是 X11、egui 不调 `set_ime_allowed` 等）仍准确 |
| `QUICK_LAUNCH_HANDOFF.md` | 问题已全解，`STATUS.md` 已覆盖 | 根因案例（HiDPI blit / `paint_event` 不跑 / 条目递减） |
| `Notepad_Enhancement.md` | 已被 `STATUS.md` + `LINUX_GUI_PORT.md` §7 覆盖 | 配套基础设施清单（FSAS、增量查找、翻译） |
| `Ports/LinuxGUI/IMEFrontend/README.md` | 描述的是 addon 时代的行协议，主链路已换 | 行协议字段（`create/cap/focus/cursor/key` + `commit/delete/forward/panel`）仍通用 |
| `Ports/LinuxGUI/X11Compat/IMdkit/README.md` | 第三方库来源说明 | — |

### 11.3 🟡 冲突需统一口径

- `STATUS.md` 顶部 ⛔「坚决不用 wlroots」与 §一页速览「Wayland 协议层（wlroots 路线）✅ 生产会话已切 / 内嵌 wlroots 合成器 ✅」并存。**建议改写为**：「wlroots **协议宿主** ✅ 生产路径；wlroots **presenter/合成器** ⛔ 禁止」（即本文 §1.1）。
- 「输入法已打通」在不同文档里分别指原生（路径 A）、Wayland 客户端（路径 B）、X11（XIM）三条链路，建议一律标注是哪一条。

### 11.4 建议的整理动作（一次做完）

```
活文档：PROJECT_MASTER.md / STATUS.md / WAYLAND_ROADMAP.md / LINUX_GUI_PORT.md
专题  ：HANDOFF_ICONS / HANDOFF_PRINT / HANDOFF_2026-11-06_STARTMENU_TTPLAYER /
        HANDOFF_2026-11-07_WLROOTS_IME
新建  ：VISTA_THEME.md（从 2026-11-05 memory 的 Vista 章节重建）
删除或移入 docs/archive/（§11.2 全表）：
        HANDOFF.md / HANDOFF_WSWLR_COMPOSITOR / HANDOFF_WAYLAND_WINE / HANDOFF_WINAPPS /
        HANDOFF_LINUX_APPS / HANDOFF_2026-11-07_BRIDGE_IME / QUICK_LAUNCH_HANDOFF /
        Notepad_Enhancement / IMEFrontend/README
上游原样保留：README / CONTRIBUTING / SECURITY
memory：以 .workbuddy/memory/MEMORY.md 为主，把 .workbuddy-ai 的四组独有铁律并入后归档 -ai
```

---

## 12. 附录：按"我现在想干什么"查文档

| 我想… | 看 |
|---|---|
| 知道现在什么能用、什么不能用 | §5 或 `STATUS.md` |
| 接手一个卡住的 bug | §5.3 P0 + 对应 handoff |
| 改 Wayland 协议 / 性能 | `WAYLAND_ROADMAP.md` §5–§6 |
| 改主题 / 外观 | `VISTA_THEME.md`（权威）、§4 D6、`scripts/port/serenity-theme.sh` |
| 改输入法 | §4 D7–D9、`IMEEngine/serenity-engine.cpp`、`WlrootsHost/ImeService.cpp` |
| 改图标 / 快捷方式 | `PortIcons` / `PortShortcuts`、`HANDOFF_ICONS_2026-11-07.md`、`.workbuddy-ai/memory/MEMORY.md` 图标铁律 |
| 改 TTPlayer | `HANDOFF_2026-11-06_STARTMENU_TTPLAYER.md` §5、`.workbuddy-ai/memory/MEMORY.md` 皮肤 6 条铁律 |
| 部署到 VM / 真机 | §7、`LINUX_GUI_PORT.md` §1–§3 |
| 复盘某个决定为什么这么定 | §4 ADR + §6 时间线 |
| 找某天到底干了什么 | §2.5 记忆段 → 对应日期的 `.workbuddy/memory/*.md` |
