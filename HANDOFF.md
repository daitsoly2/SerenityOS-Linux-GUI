# 交接文档：SerenityOS Linux GUI 端口

> 写于 2026-11-01。给下一个会话用：先说清"现在是什么状态"，再说"下一步做什么、为什么"。
> 更详细的技术记录见 `LINUX_GUI_PORT.md`（分章节），踩坑过程见 `.workbuddy/memory/2026-11-01.md`。

---

## 0. 项目定位（先读这条，不然会走偏）

**"复古"指界面复古，不是架构复古。**

这个系统跑在**现代** Linux 上，界面模仿 SerenityOS / 90 年代桌面。底层不追求"地道"：

- 优先用成熟组件（libinput、DRM/KMS、fcitx5），能用现成的就不自造；
- 不要为了"更底层/更原生"去写内核模块或替换可用栈，除非有可测量收益；
- 力气花在**外观层**：主题配色、候选窗样式、窗口装饰、字体/DPI。

（曾经因为误判成"架构也要复古"，写了一个 evdev → `/dev/mouse/0` 的 bridge，后来被 libinput 方案取代了。）

---

## 1. 当前状态总览

| 子系统 | 实现 | 状态 |
|---|---|---|
| 显示 | 直接 DRM/KMS（dumb buffer + page flip） | ✅ 稳定，1280×960 |
| 输入 | libinput（udev / seat0） | ✅ 移动、点击、键盘全部实测通过 |
| 合成器/窗口管理 | Serenity WindowServer（核心未改） | ✅ |
| 桌面外壳 | `serenity-shell`：桌面 + 任务栏单进程（explorer 模式） | ✅ 2026-11-01 合并完成 |
| SDL | 仅作窗口化开发回退 | 未调用（`SERENITY_PRESENTER=sdl` 才启用） |
| 输入法 | fcitx5 + 自写前端 addon | ⚠️ addon 已加载，但 **rime 未激活** |
| 主题桥接 | `serenity-theme-gen.py` 生成 GTK/Qt 主题 | ✅ GTK 实测生效 |
| X11 兼容 | `serenity-x11-bridge`（X 窗口 → Serenity 窗口） | ✅ 出图；输入注入未验证 |
| 应用 | 62/62 个 app 目标全部构建成功（`bin/` 共 76 个 serenity-*） | ✅ 含终端/文件管理器/运行 |
| 运行期前缀 | 全部装在 `/serenity`（bin/lib/libexec/res/share/etc），`/usr/local` 已清空 | ✅ 2026-11-01 |
| 汉化 | LibGUI 运行时 `[UI]` 字符串表（700+ 条），菜单/按钮/标题/对话框中文 | ✅ 2026-11-01 |
| Windows 应用 | `serenity-winapp` 自动选兼容层（DWProton → wine），Xvfb+桥接随会话启动 | ✅ 2026-11-01 |
| 系统 wine | 已移除；DWProton（`/serenity/opt/dwproton`）替代，i386 运行库单独补装 | ✅ 2026-11-01 |

---

## 2. 环境与常用命令

```bash
# SSH 进 VM（从仓库根目录执行，密钥相对路径要注意 cwd）
ssh -i Build/vm/id_ed25519 -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null serenity@127.0.0.1

# 构建 WindowServer（在 VM 内）
cd ~/serenity && cmake --build Build/lagom --target serenity-window-server -j6

# 重启桌面会话（只重启 GUI，不用重启整机）
sudo systemctl restart serenity-desktop.service

# 安装/升级运行期到 /serenity（在 VM 内、仓库根执行；--clean 把旧的 /usr/local 副本挪走）
sudo scripts/port/05-install-prefix.sh --systemd

# 看服务状态 / 日志
systemctl is-active serenity-desktop.service; pgrep -xc serenity-window
sudo journalctl -u serenity-desktop.service -n 40 --no-pager
```

**宿主机侧**（注入 + 截屏，都在 `/mnt/user/serenity/Build/vm/` 下执行）：

```python
# QMP：绝对坐标注入（HMP 的 mouse_move 对 virtio-tablet 无效，必须用 QMP）
s.connect('/tmp/qemu-qmp.sock'); s.sendall(json.dumps({'execute':'qmp_capabilities'}).encode()+b'\n')
ev = [{'type':'abs','data':{'axis':'x','value':16384}},
      {'type':'abs','data':{'axis':'y','value':16384}}]        # 0..32767 → 屏幕比例
s.sendall(json.dumps({'execute':'input-send-event','arguments':{'events':ev}}).encode()+b'\n')

# QMP：鼠标按键
{'type':'btn','data':{'down':True,'button':'left'}}

# HMP：键盘（连 /tmp/qemu-mon.sock，命令 sendkey a / sendkey ret / sendkey down）
m.sendall(b'sendkey a\n')

# 截屏
m.sendall(b'screendump /tmp/x.ppm\n')   # 再 convert /tmp/x.ppm out.png
```

坐标换算：`abs_x = x / 1280 * 32767`，`abs_y = y / 960 * 32767`。

---

## 3. 已知的坑（别再踩一遍）

### 显示

1. **模式必须在分配 framebuffer 之前定下来**。`probe_display_size(w,h)` 在 `main` 里先问出显示模式建初始布局；后端在 `set_head_mode_setting()` 里偷改尺寸会导致黑边/越界/崩溃。
2. **不要读 CRTC 当前模式当分辨率**。virtio-gpu 的模式跟着 QEMU 宿主窗口走，读回来会漂成 1235×682。要按"精确 → 最小覆盖 → 最接近"从模式表里选。
3. **退出必须 `restore_display()` + `_exit()`**。完整析构会触发 `!m_ref_count` 断言；崩溃退出会让 systemd 重启时旧进程仍持 DRM master，新实例 `SetCrtc` 失败。
4. **退出时不要恢复 `saved_crtc->buffer_id`**（可能是已销毁的 FB），应 `drmModeSetCrtc(fd, crtc, 0, ...)` 禁用 CRTC 交回 fbcon。

### 输入

5. `ScreenInput` 的**绝对坐标是 16 位**（`x * width / 0xffff`），不是像素。libinput 用 `get_absolute_x_transformed(e, 0xffff)` 正好对上。
6. 相对指针要做**亚像素累积**，否则慢速移动被 round 成 0，指针"卡住"。
7. **按钮事件不带坐标**，绝对设备必须补发缓存的位置，否则点击打在旧位置。
8. 手工监听单个 `/dev/input/eventN` 会漏设备 —— QEMU 的按键事件可能路由到别的节点。这就是改用 libinput 的直接原因。

### 进程管理

9. `pkill -f <名字>` 会自杀（命令行里含同样的字符串）。用 `pkill -x` + 15 字符截断名。
10. VM 时钟是 UTC（比本地慢 8 小时），`ninja: no work to do` 时先确认 scp 的源文件是否真的比 .o 新。

### 构建

11. 改完源文件必须 scp 到 `~/serenity/`（**不是** `Build/`），再在 VM 内构建。
12. 端口构建用 clang + `-Werror`，未使用变量、designated initializer 缺字段都会直接失败。
13. **宿主机构建出来的二进制不能在 VM 里跑**：宿主 glibc 比 VM 新（VM 是 2.41），
    直接 scp 过去会报 `version 'GLIBC_2.43' not found`。宿主 `Build/lagom` 只能用来
    快速查编译错误；**要部署的二进制必须在 VM 内构建**（源码 tar 过去 → VM 里 ninja）。
14. `/dev/fb0` 是 fbdev 模拟缓冲，停在开机 splash，**不能当截图**；视觉验证只能走 QEMU
    `screendump`（宿主 `/tmp/qemu-mon.sock`，注意 screendump 写在 QEMU 进程侧）。

### 运行期布局

15. **运行期在 `/serenity`，不在 `/usr/local`**：`bin/ lib/ libexec/ res/ share/desktop/ etc/`，
    装/升级用 `sudo scripts/port/05-install-prefix.sh --systemd`。二进制的 RUNPATH 是
    `$ORIGIN:../lib`，所以 bin 和 lib 必须保持同级。
16. **`/res`、`/etc/WindowServer.ini`、`/etc/serenity` 是指向前缀的符号链接**（代码里写死这三个路径），
    别当成真实目录改；要改配置去 `/serenity/etc/`。
17. 前缀通过 `SERENITY_PREFIX` 传给子进程；`serenitid` 没收到时按 `SERENITY_BIN` 的父目录推导，
    所以**不要把 `SERENITY_BIN` 指到前缀外面**，否则 `/bin/Name` 解析和系统快捷方式都会找错地方。

### 汉化

18. 翻译不在 app 里，在 **LibGUI 的文本入口**：`LibGUI/UITranslation.cpp` 的 `translate_ui()`
    在 `Action`/`Menu`/`Label` **构造函数**以及 `set_text/set_title/add_submenu/Menu::set_name/
    Widget::set_title/Statusbar::set_text` 里查 `/res/locale/<lang>.ini` 的 `[UI]` 组。
    只挂钩 setter 会漏掉全部菜单（app 用 `Action::create("&Save")` 在构造时传文案）。
    改文案只改 ini，**不用重编**；改 `UITranslation.cpp` 才要重编 LibGUI。
19. 改完 ini 必须重跑 `sudo scripts/port/05-install-prefix.sh`：
    `<prefix>/res` 是从 `Build/lagom/Root/res` 复制 + `Base/res/locale` 覆盖出来的，
    不重装的话运行中的桌面读的还是旧表。
20. `[UI]` 的键**不能含换行**（ini 是按行的），所以 `"Unable to open file.\n"` 这类
    带 `\n` 的串没法进表；`scripts/port/ui-strings.py` 已经把它们过滤掉。
21. 查漏用 `scripts/port/ui-strings.py`（`--all` 看全部，`--app X` 看单个 app）。

### Windows 兼容层

22. **purge wine 会带走 Proton 需要的 i386 运行库**（autoremove 一并删），之后
    Proton 的 32 位 wine 报 `cannot find the FreeType font library`。补装：
    `libfreetype6:i386 libasound2t64:i386 libavcodec61:i386 libavformat61:i386
    libavutil59:i386 libdrm2:i386 libdrm-amdgpu1:i386 libglib2.0-0t64:i386
    libgstreamer1.0-0:i386 libgstreamer-plugins-base1.0-0:i386 libgstreamer-gl1.0-0:i386
    libpipewire-0.3-0t64:i386 libpulse0:i386 libudev1:i386 libwayland-client0:i386
    libwayland-egl1:i386 libx11-6:i386 libxext6:i386 libxkbcommon0:i386
    libpcap0.8t64:i386 libpcsclite1:i386 libusb-1.0-0:i386 libxkbregistry0:i386`
23. 运行入口统一是 `<prefix>/bin/serenity-winapp`（`scripts/port/winapp-launcher.sh`）：
    先判"游戏还是应用"——PE 导入表里有 `d3d*/dxgi/xinput/dinput8/vulkan-1/openal32`，
    或同目录有 `UnityPlayer.dll`/`steam_api64.dll`/`*.pak`/`*.uasset`/`*.pck`/`*.rpa`
    就按游戏走 umu，否则走 `<prefix>/opt/dwproton/files/bin/wine` 直跑。
    猜错用 `--game` / `--app` / `SERENITY_WINAPP_MODE=game` 覆盖，`--dry-run` 只看判定。
    `WINEPREFIX` 默认 `/root/.wine-dw`（会话是 root，`$HOME` 在 systemd 单元里可能是空）。
24. Xvfb + `serenity-x11-bridge` 由 **serenitid 在启动时拉起**（`xvfb` / `x11bridge` 子进程），
    不用手动开。桥接启动时 Xvfb 还没就绪会退出，靠 supervisor 重试。
25. `umu-run`（`/serenity/bin/umu-run`）是给游戏用的：需要 `UMU_ALLOW_ROOT=1`（会话是 root），
    `PROTONPATH` 不设会自动找 `/serenity/opt/dwproton`。DWProton 的 protonfixes 首次会下载
    FSR4 upscaler，没网会卡住 —— 跑普通应用别用 umu，直接 `serenity-winapp`。
26. 桥接只映射"像窗口"的 X 顶层窗口：<200×120、无标题、标题含 `.exe`/`\` 的都跳过，
    否则 Wine 应用会冒出七八个空窗口。标题必须读 `_NET_WM_NAME`（UTF-8），`XFetchName`
    拿不到中文标题。

---

## 4. 下一步任务清单（按优先级）

> P0 / P1 / P2 已于 2026-11-01 完成：62/62 个 app 目标全部链接成功。
> 修法摘要：
> - `Core::System::beep` / `enter_jail_mode_until_{exec,exit}` 在 `System.h` 非 Serenity 分支加 no-op；
> - Terminal `main.cpp` 的 `act.sa_mask = 0` 改 `sigemptyset`；
> - `gen-app-targets.py` 支持 `stringify_gml(...)`（`port_stringify_gml` 函数，输出到
>   `${CMAKE_BINARY_DIR}/Userland/<AppDir>/`，target 加该 include 路径，`add_dependencies` 保证先生成）；
> - LibCards `<cfloat>`、Maps `<climits>`、Run `PAGE_SIZE`→`4096`；
> - FileManager `DirectoryView.cpp`：去掉多余 `<serenity.h>`，Linux 下 `disown` no-op
>   （main.cpp 已设 SA_NOCLDWAIT）、`posix_spawn_file_actions_addchdir`→`_np`；
> - LibCore `Command.cpp`：`Core::command()` 从 Serenity-only 放开到 `AK_OS_LINUX`
>   （`addchdir`→`_np` 宏映射），users-settings 链接因此恢复。

> **下一轮主任务（用户反复强调，最优先）**：见文末「交接给下个会话」。

### P1：desktop-icons + taskbar 合并为单进程外壳（explorer.exe 模式）✅ 已完成 2026-11-01

用户明确要求把桌面（图标+壁纸）和任务栏（开始菜单+窗口列表+时钟+托盘）合并成一个进程，像 Windows 的 `explorer.exe`。理由：共享一份 app 列表扫描和图标缓存、右键菜单状态统一、少一个进程/一次 IPC 连接。

**已实现**：`serenity-shell`（`Ports/LinuxGUI/Shell/`）一个 `GUI::Application` 开两个窗口：

- `main.cpp`：先建 Desktop 窗口（`WindowType::Desktop`，全屏），再建 Taskbar 窗口；
  `set_quit_when_last_window_deleted(false)`，关一个窗口不结束会话；SIGCHLD 统一回收子进程。
- `AppCatalog.{h,cpp}`：**唯一一次** `/res/apps` 扫描（`resolve_executable` + `access(X_OK)` 过滤），
  按路径+尺寸缓存图标；开始菜单、桌面"新建"菜单都读它。桌面右键"刷新"会 `rescan()`。
- `DesktopIconsWidget.{h,cpp}`：从 `Ports/LinuxGUI/DesktopIcons/desktop_icons.cpp` 平移过来，
  只改了 app 列表来源和 `hostify` 调用。
- `SystemMenu.{h,cpp}`：从 `Userland/Services/Taskbar/main.cpp` 平移过来，菜单项直接捕获
  `NonnullRefPtr<AppFile>`（不再用下标索引 `g_apps`，rescan 后不会错位）。
- 删除 `Ports/LinuxGUI/DesktopIcons/`；`serenity-taskbar` 不再由 Linux 端口构建
  （`Userland/Services/Taskbar/main.cpp` 保留给原生 SerenityOS 构建）。
- serenitid 的子进程表：`desktop-icons` + `taskbar` → 一个 `shell`（`serenity-shell`，always_restart）。

**顺带收拢**：`/bin/Name` → `serenity-<kebab>` 的转换原来有 4 份
（`LibCore/Process.cpp`、`LaunchServer/Launcher.cpp`、`Taskbar/main.cpp`、`desktop_icons.cpp`），
现在统一到 `Ports/LinuxGUI/Common/hostify.h`（`hostify_executable` / `resolve_executable` /
`find_tool` / `kebab_case`）。

**验证（VM 实测）**：

```
ps:  只有 /usr/local/bin/serenity-shell（无 serenity-taskbar / serenity-desktop-icons）
log: shell: desktop [0,0 1280x960] + taskbar [0,933 1280x27] in one process, 40 apps in catalog
log: shell: desktop shows 4 shortcuts
      Create applet: 0/1/2（托盘三件套挂到 shell 的 applet area）
      serenity-calculator 另起后 shell 仍存活
```

> ⚠️ 本会话没有 QEMU 监视器通道（`/tmp/qemu-*.sock` 不在本机），**没能截屏**做视觉确认；
> 上面是进程 + 启动日志层面的验证。下次有截屏通道时补一张"桌面图标 + 任务栏 + 开始菜单"的图。
> 另：`/dev/fb0` 是 fbdev 模拟出来的**陈旧**缓冲（停在开机 splash 5%），不能当截图用。

### P2：控制面板（Settings）部署验证 + 补缺

- 已把 `Userland/Applications/Settings/main.cpp` 改成 Win9x 控制面板（分组 GroupBox：System / Network & Internet / Display / Other，每个分组一个 IconView）。**本机构建通过，尚未部署到 VM 验证。**
- 部署：scp 该文件 + `ninja serenity-settings` + cp 到 `/usr/local/bin` + 重启桌面。
- 已知缺口：Display 项（DisplaySettings 依赖未移植的 LibEDID，二进制不存在，点了失败）；
  Browser/Mail/Maps Settings 依赖 LibWeb 同样点开失败（但 app 本身在 /res/apps 所以会列出）。
- 若合并 shell 后统一做 UI，建议在合并之后再一起调控制面板外观，避免两处 UI 改动冲突。

### P3：输入法接上 rime

> 2026-11-01 追加（本轮完成）：桌面右键菜单 Win9x 化 + 压缩/解压 + wine 支持，app 全部装入 /usr/local/bin。
> 详见"2026-11-01 追加"小节。

### P3.5：桌面右键菜单 Win9x 化（已完成）

- `Ports/LinuxGUI/DesktopIcons/desktop_icons.cpp`：右键菜单改为 Win98 结构
  （排列图标(A)▶ / 刷新(E) / 粘贴(P)灰 / 粘贴快捷方式(S)灰 / 新建(W)▶=apps 列表 / 属性(R)；
  图标上右键追加 压缩(Z)▶{ZIP,Tar.gz}、解压到这里(E)（仅压缩包）、用 Wine 运行（仅 .exe）、移除此图标）。
- 压缩/解压走 `Core::command`（上一轮放开的 Linux 实现），工具探测 find_tool 搜 $PATH：
  zip=7z/7za/7zz/zip，tar=tar，解压按扩展名选 7z/tar/unzip。结果用 GUI::MessageBox 提示。
- Entry 新增 `Path=`（desktop.ini），支持纯文件图标（Exec 可空）；双击 .exe → wine 运行（find_tool 找 wine，缺 wine 弹窗提示）。
- 主题 `Base/res/themes/Default.ini`：MenuBase/MenuStripe=#d4d0c8、MenuSelection=#000080、MenuSelectionText=white（Win9x 灰底深蓝高亮，全部菜单生效）。
- 翻译键在 `Base/res/locale/{en,zh-CN}.ini` [Desktop]；部署时拷进 `Build/lagom/Root/res/locale/`。

### P3.6：桌面改为目录驱动（已完成，2026-11-01）

- **桌面 = 文件夹**：图标来自快捷方式文件，不再是 desktop.ini 配置。两种格式都支持：
  - `.af`（Serenity）：`[App] Name/Executable/Icon`
  - `.desktop`（freedesktop）：`[Desktop Entry] Name/Exec/Icon/Path`（Exec 的 `%` 字段码自动剥掉）
- 目录：`~/Desktop`（用户，自动创建）+ `/usr/share/serenity/desktop/`（系统默认 4 个 .desktop：Calculator/FileManager/Settings/Minesweeper）。
- 同名 basename 去重，用户覆盖系统。
- 右键"新建"→ 往 `~/Desktop` 写 `.desktop`；"移除此图标"→ 删源文件（系统目录只读不可删）。
- 链接到 LibFileSystem（CMakeLists 加了 LibFileSystem）。
- 验证：桌面显示 4 个快捷方式 ✓；双击 Calculator.desktop → Calculator 窗口 ✓。
- **待办**：desktop-icons 与 taskbar 合并为单进程外壳（explorer.exe 模式）——共享 app 列表扫描、统一右键菜单、少一进程。

### 2026-11-01 追加：app 安装与运行修通（重要）

- **app 全部安装到系统目录**：`Build/lagom/bin/serenity-*` → `/usr/local/bin/`（80 个），
  `lib/liblagom-*.so*` → `/usr/local/lib/` + ldconfig；`/res` 仍指向构建树 Root/res。
- **serenitid 包装脚本** `/usr/local/bin/serenitid`：`export SERENITY_BIN=/usr/local/bin; exec /usr/local/libexec/serenitid.real`。
  ⚠️ 坑：不能把真二进制也叫 /usr/local/bin/serenitid 再写同名包装脚本（会互相覆盖 → 脚本 exec 自己 → 99% CPU 死循环）。
  真二进制放 `/usr/local/libexec/serenitid.real`。
- **"所有 app 打不开"的根因（重要）**：Serenity 的 AppFile(.af) 与 Taskbar 里写的是
  `/bin/Calculator`、`/bin/Settings`、`/bin/Run` 这类 **Serenity 内部路径**，Linux 上不存在。
  连带影响：
  - `LaunchServer::load_handlers` 里 `access("/bin/Calculator", X_OK)` 失败 → **所有 handler 都没注册** → 开始菜单/运行/设置全空。
  - Taskbar 直接 spawn `/bin/Settings` 等 → ENOENT。
  修法（`Userland/Libraries/LibCore/Process.cpp` 的 `Process::spawn(ProcessSpawnOptions)` 入口统一转换）：
  `/bin/Name` → basename → CamelCase 转 kebab-case → `serenity-<name>` → 依次试
  `$SERENITY_BIN/`、`/usr/local/bin/`（存在才用）。一处修改覆盖 LaunchServer/Taskbar/desktop-icons/所有 app。
  （`LaunchServer/Launcher.cpp` 与 `DesktopIcons/desktop_icons.cpp` 里也各有一份同样的转换，
   前者因为 `access()` 检查需要转换后的路径才能注册 handler。）
- **修复的运行时 bug**（都是"编过但跑不起来"）：
  1) `Core::Process::spawn` 带 working_directory 在 Linux 是 `TODO()`（=断言崩溃）→
     `Process.cpp` 用 `posix_spawn_file_actions_addchdir_np` 实现（LibCore，影响所有 spawn）。
  2) Terminal：`/bin/utmpupdate`（Serenity 特有）spawn ENOENT → 用 `#ifdef AK_OS_SERENITY` 包住；
     `/proc/<pid>/children` 不存在 → shell_child_process_count 返回 0；shell fallback `/bin/Shell`→`/bin/bash`。
  3) desktop-icons：相对名 exec 用 `SERENITY_BIN`/PATH 解析成绝对路径（resolve_executable）；
     Entry 支持 `Path=`（纯文件图标，Exec 可空）。
- **验证结果**：双击桌面图标 → Calculator 窗口正常打开；开始菜单 → 设置 → Settings 窗口打开 ✓；
  开始菜单 → 运行… → Run 对话框打开 ✓；calculator/file-manager/terminal 探活 rc=124；Win9x 右键菜单中英文截图确认。
- **已知小问题**：
  - 新窗口与已有窗口**位置重叠**（Serenity WM 不级联），多个同款 app 叠在一起看起来像"没打开"。
  - Taskbar 的"帮助(H)"指向 /bin/Help（需 LibWeb，未构建）→ 点了没反应。
  - AppFile 名称里的 `&` 助记符在桌面图标标签上原样显示（"&Sound Player"）。
- **注入测试经验**（QEMU 测试通道，勿踩）：
  - QMP `input-send-event` 的 key 用 QKeyCode 字符串（"esc"）不是数字；
  - 桌面重启后第一次 abs 注入可能丢失，重试即可；弹出的菜单要 Esc 关掉否则挡住后续点击；
  - `screendump` 写在**宿主机** /tmp（QEMU 进程侧），不在 VM 里；
  - Read 工具预览的截图是缩放的（1080/1280≈0.844），量坐标要换算（实际 = 预览/0.844）。

### P3：输入法接上 rime

现状：`serenityime` addon 已被 fcitx5 加载、socket 协议跑通（`create`→ok、`key`→handled），但 **rime 没有进入 `AvailableInputMethods`**，`CurrentInputMethod` 是空字符串 → 没有组合态，拿不到 preedit/commit。

- fcitx5 重启会把 profile 里无效的 rime 项重写掉，需要先让 rime 注册成功。
- 相关文件：`Ports/LinuxGUI/IMEFrontend/`（`serenityime.cpp` 是 addon，`ime_probe.py` 是协议探针）。
- 通了之后的下一步是**用 LibGUI 画候选窗**（配色取 fcitx5 theme：`NormalColor=#000000`、`HighlightColor=#ffffff`、TextMargin 5px），这是纯外观层、收益最直接的一块。

### P4：显示的性能与自适应

- **atomic KMS + GBM**：现在是 dumb buffer + 每帧 memcpy，可以做到零拷贝。
- **热插拔**：`take_display_change()` 已实现（读 `DRM_EVENT_HOTPLUG` 后重探测），但**没在真机上验证过**。
- **HiDPI**：`ScreenLayout.scale_factor` 字段已存在，接上即用。

### P5：清理

- `Ports/LinuxGUI/InputBridge/`（evdev bridge）和 `/dev/mouse/0` FIFO 现在只是 libinput 失败时的兜底，可以评估删除。删之前确认 `LinuxInput` 在所有目标机上都能起来。
- `Common/keymap.h` 是共享键映射表（bridge 与 WindowServer 共用），删 bridge 时保留。

---

## 5. 关键文件索引

```
Ports/LinuxGUI/
├── main_linux.cpp            # WindowServer 入口：布局、输入、事件泵、退出
├── LinuxScreenBackend.{h,cpp}# 显示后端：KMS 呈现器 + SDL 回退 + 模式探测
├── LinuxInput.{h,cpp}        # libinput 输入后端
├── Common/keymap.h           # evdev → Serenity KeyCode（唯一一份）
├── Common/serenity_keycode.h # 从 Kernel/API/KeyCode.h 生成的键码常量
├── Common/hostify.h          # /bin/Name → serenity-<kebab>（唯一一份，4 处共用）
├── Common/Translation.h      # /res/locale/<lang>.ini 文案表
├── Shell/                    # 桌面外壳（Desktop + Taskbar 单进程）
│   ├── main.cpp              # 一个 Application，两个窗口
│   ├── AppCatalog.{h,cpp}    # app 扫描 + 图标缓存（开始菜单 / 桌面共用）
│   ├── DesktopIconsWidget.{h,cpp}
│   └── SystemMenu.{h,cpp}
├── Apps/generated-apps.cmake # 应用目标（脚本生成，勿手改）
├── IMEFrontend/              # fcitx5 前端 addon + 协议探针
├── InputBridge/              # evdev bridge（兜底，可评估删除）
└── X11Bridge/                # X11 → Serenity 窗口桥

scripts/port/
├── gen-app-targets.py        # 生成 Apps/generated-apps.cmake
└── serenity-theme-gen.py     # 从主题 .ini 生成 GTK/Qt 主题
```

---

## 6. 验证方法速查

| 想验证 | 怎么做 |
|---|---|
| 桌面是否活着 | `pgrep -xc serenity-window`，再截屏看任务栏 |
| 外壳是否单进程 | `ps -ef \| grep -E "serenity-(shell\|taskbar\|desktop-icons)"`（应只有 `serenity-shell`）；`/var/log/serenity-session.log` 里应有 `shell: desktop ... + taskbar ...` |
| 显示是否走 KMS | 启动日志里 `LinuxScreenBackend: KMS presenter on WxH` |
| 输入是否走 libinput | 启动日志里 `WindowServer: input via libinput` |
| 鼠标移动 | QMP 注入 50% 位置 → 光标应在 (640,480) |
| 鼠标点击 | 点 (35,940) 开始按钮 → 菜单弹出 |
| 键盘 | 菜单打开时按三次 `down` → 选中项移动 |
| 应用是否编出来 | `ls Build/lagom/bin | grep serenity-<名字>`（62/62 已编出；下一步是**运行**验证，重点：terminal / file-manager / run） |

---

## 7. 交接给下个会话（2026-11-01 下午，本轮总结）

> 读完这一节就能接上：先知道「上一步做到哪了」，再按 §4 的 P1/P2 继续。

### 本轮做完了什么（都已在 VM 实测验证）

1. **62/62 个 app 目标全部构建成功**，并**全部安装到系统目录**：
   - `Build/lagom/bin/serenity-*` → `/usr/local/bin/`（80 个二进制）
   - `lib/liblagom-*.so*` → `/usr/local/lib/` + `ldconfig`
   - `/res` 仍指向 `Build/lagom/Root/res`（构建树）

2. **"所有 app 打不开" 的根因已修**（这是本轮最重要的认知）：
   - 根因：Serenity 的 AppFile(.af) 与 Taskbar 写的是 `/bin/Calculator` 这类 **Serenity 内部路径**，Linux 上不存在。
   - 修法：在 `Userland/Libraries/LibCore/Process.cpp` 的 `Process::spawn(ProcessSpawnOptions)` 入口统一转换
     `/bin/Name` → basename → CamelCase→kebab-case → `serenity-<name>` → 试 `$SERENITY_BIN/`、`/usr/local/bin/`。
     一处修改覆盖 LaunchServer/Taskbar/desktop-icons/所有 app。
   - **同类路径转换代码有 3 份**（`Process.cpp` 入口 + `LaunchServer/Launcher.cpp` + `DesktopIcons/desktop_icons.cpp`），
     因为 LaunchServer 和 Taskbar 各自有 `access()` 检查，需要用转换后的路径才能注册 handler。做 explorer 合并时应该收拢成一份。

3. **桌面改为目录驱动**（用户设计要求，已实现）：
   - 桌面 = 文件夹，图标 = 里面的快捷方式文件。两种格式都支持：`.af` 和 `.desktop`（freedesktop）。
   - 目录：`~/Desktop`（用户）+ `/usr/share/serenity/desktop/`（系统默认 4 个 .desktop）。
   - 右键"新建"写 `.desktop` 到 `~/Desktop`；"移除此图标"删源文件（系统目录只读不可删）。
   - `desktop.ini` 那套配置加载已**完全移除**。

4. **右键菜单 Win9x 化**：排列图标▶/刷新/粘贴(灰)/新建▶/属性；图标右键追加 压缩(Z)▶{ZIP,Tar.gz}、
   解压到这里（压缩包）、用 Wine 运行（.exe）、移除此图标。压缩/解压走 `Core::command` + 7z/tar/unzip。

5. **wine 11.0 已装**（清华源 `mirrors.tuna.tsinghua.edu.cn/wine-builds/debian`，2 分钟；官方源要 1 小时+），
   软链 `/usr/local/bin/wine`。双击 .exe 走 wine。

6. **Settings 已改造成 Win9x 控制面板**（分组 GroupBox + IconView），**本机构建通过但未部署验证**。

### 下一步（按优先级，详见 §4）

- **P1（已完成 2026-11-01）**：desktop-icons + taskbar 已合并成单进程 `serenity-shell`（explorer.exe 模式），见 §4 P1。
- **P2**：控制面板部署验证 + 补 Display 缺口（LibEDID 未移植）。
- **P3**：输入法 rime 激活（老问题，未动）。
- **候选（用户提过）**：把整个工程/安装前缀搬到 `/serenity`，与 Linux 系统目录分离。
  可行，但要先定"只搬运行期前缀"还是"连源码树一起搬"（后者会改工作区根目录）。

### 关键坑（本会话新踩，务必记住）

- **serenitid 包装脚本**：真二进制必须放 `/usr/local/libexec/serenitid.real`，`/usr/local/bin/serenitid` 是包装脚本。
  两者同名互相覆盖会导致脚本 exec 自己 → 99% CPU 死循环。
- **新窗口与旧窗口位置重叠**（Serenity WM 不级联），多个同款 app 叠一起看起来像"没打开"。判据看任务栏条目数。
- **测 app 别用 `cmd | head`**（SIGPIPE 让进程假退出 rc=0 误导），用 `timeout 3 <app>` 看 rc=124（存活）。
- **部署顺序**：`systemctl stop serenity-desktop.service` → cp 二进制 → `systemctl start`（cp 时进程在跑会 `Text file busy`）。
- **注入测试**（QEMU 通道）：QMP key 用 QKeyCode 字符串（"esc"）；菜单开着会挡点击要先 Esc；
  `screendump` 写在宿主机 /tmp；Read 预览截图 0.844 缩放，坐标要 ÷0.844；重启后首次 abs 注入偶发丢失需重试。

### 待清理的重复代码（已完成 2026-11-01）

`hostify_executable` / `/bin/Name` 路径转换原来在 4 处重复（Process.cpp / Launcher.cpp /
Taskbar/main.cpp / desktop_icons.cpp），合并 shell 时已收拢到
`Ports/LinuxGUI/Common/hostify.h`。新增调用点请直接用它，不要再抄一份。

### 任务管理器 / 快速启动 / 托盘（2026-11-01 晚）

27. 上游 SystemMonitor 依赖 LibSymbolication（Lagom 没编），app 生成器会跳过它。
    现在由 `Ports/LinuxGUI/SystemMonitorLite/main.cpp` 提供一个同名替代
    （`serenity-system-monitor`）：读 /proc，列 PID/名称/CPU/内存/状态，支持结束进程、
    刷新、单实例（flock `/tmp/.serenity-task-manager.lock`）。任务栏"系统监视器"、
    Ctrl+Alt+Del 都指向它。
28. `GUI::Process::spawn_or_show_error` 现在只在真正 ENOENT 时提示"未安装"。
    之前用 `access(path)` 预检查是错的：`/bin/Settings` 在宿主上不存在，
    但 `Core::Process::spawn` 会把它映射成 `serenity-settings`，于是误报未安装。
29. 快速启动栏改成读目录 `~/.local/share/quick-launch/`（首次运行自动建目录并放
    FileManager/Terminal/Calculator 的 .af 链接），不再写死应用列表。
    **已知问题**：widget 里 5 个条目、宽度 120 都正常，但任务栏上不显示——
    `TaskbarWindow::add_system_menu` 里的 `insert_child_before` 重排没生效，待查。
30. 音量 applet（`serenity-volume-applet`）在任务栏里被当成普通窗口（任务按钮），
    点击只会激活窗口，不会弹音量条——需要把 `WindowType::Applet` 的窗口放进
    托盘区（`m_applet_area_container`），而不是进窗口列表。
31. 桌面图标/开始菜单还不支持拖拽，也没有拖到快速启动栏的 drop 目标。
