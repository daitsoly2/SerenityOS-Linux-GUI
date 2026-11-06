# SerenityOS 桌面环境 → Debian 13（无桌面）移植

把 SerenityOS 的 **WindowServer（合成器 / 窗口管理器）** 和 **LibGUI 应用**
作为普通 Linux 进程跑起来，并渲染到一个宿主窗口里。

图形栈本身（`LibGfx`、`Compositor`、`WindowManager`、`LibIPC`、`LibGUI`）
**原样复用**，只替换了与 SerenityOS 内核打交道的那一层。

---

## 1. 环境要求

- Debian 13（trixie），无桌面环境即可
- 8 核 / 8 GB 内存以上比较舒服（首次全量构建较久）
- 磁盘：源码之外再留 ~10 GB 给 `Build/`
- 需要 root 权限（安装依赖、创建 `/res` 软链接、写 `/etc/WindowServer.ini`）

## 2. 快速开始

```bash
cd /mnt/user/serenity/scripts/port

sudo ./00-install-deps.sh      # clang / cmake / ninja / SDL2 / X11 头
sudo ./01-fetch-caches.sh      # Unicode / CLDR / TZDB / PublicSuffix 等
./02-build.sh                  # 配置 + 预生成 IPC + 编译
sudo ./03-run.sh               # 只开桌面
sudo ./04-run-app.sh           # 桌面 + 自带 AnalogClock
```

> GitHub 资源（CLDR、PublicSuffix）默认走 `https://v4.gh-proxy.org` 加速，
> 可用 `GH_PROXY=... ./01-fetch-caches.sh` 覆盖。

## 3. 脚本说明

| 脚本 | 作用 |
|------|------|
| `00-install-deps.sh` | `apt-get install` 构建依赖：`build-essential clang lld cmake ninja-build pkg-config python3 curl unzip tar libsdl2-dev libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev xvfb` |
| `01-fetch-caches.sh` | 预置 Lagom 构建所需的离线数据。CMake 自带的解压在部分 overlay/沙箱文件系统上会因 `Can't restore time` 失败，所以这里提前下载并解压 |
| `02-build.sh` | 配置 Lagom（`ENABLE_LINUX_GUI_PORT=ON`、`ENABLE_LAGOM_GUI_APPS=ON`），先单独生成 8 个 IPC 头，再并行编译 |
| `03-run.sh` | 准备 `/res` 与 `/etc/WindowServer.ini`，按环境选择 X11 / Wayland / KMSDRM / headless |
| `03-run-rootless.sh` | 同上，但不需要 root：用 user namespace + chroot 伪造 `/res` |
| `gen-app-targets.py` | 扫描 `Userland/{Applications,Games,Demos}`，把自带 app 自动转成宿主目标 |
| `04-run-app.sh` | 起 WindowServer，等 `/tmp/portal/window` 就绪后启动 `AnalogClock`，退出时清理 |
| `05-install-prefix.sh` | 把运行期装进**一个前缀**（默认 `/serenity`：bin/lib/libexec/res/share/etc），而不是散在 `/usr/local`；`--systemd` 重写服务单元，`--clean` 把旧 `/usr/local` 副本挪进前缀下的备份目录 |
| `ui-strings.py` | 汉化查漏：扫源码里会进控件的字符串，和 `Base/res/locale/zh-CN.ini` 的 `[UI]` 表对比，报覆盖率与未翻译清单 |
| `04-qemu-debian13.sh` | 可选：用 Debian 13 云镜像 + cloud-init 起一个干净 QEMU 测试床 |

## 4. 编译流程

`02-build.sh` 等价于：

```bash
cmake -S Meta/Lagom -B Build/lagom -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_LAGOM=ON \
  -DENABLE_LAGOM_LIBWEB=OFF \
  -DENABLE_LAGOM_LADYBIRD=OFF \
  -DENABLE_LAGOM_CCACHE=OFF \
  -DENABLE_JAKT=OFF \
  -DENABLE_NETWORK_DOWNLOADS=OFF \
  -DENABLE_ADOBE_ICC_PROFILES_DOWNLOAD=OFF \
  -DENABLE_COMMONMARK_SPEC_DOWNLOAD=OFF \
  -DENABLE_LINUX_GUI_PORT=ON \
  -DENABLE_LAGOM_GUI_APPS=ON

# 第一阶段：先生成 IPC 头（避免干净构建的编译竞态）
cmake --build Build/lagom --target \
  generate_WindowServerEndpoint.h generate_WindowClientEndpoint.h \
  generate_WindowManagerServerEndpoint.h generate_WindowManagerClientEndpoint.h \
  generate_ClipboardClientEndpoint.h generate_ClipboardServerEndpoint.h \
  generate_NotificationClientEndpoint.h generate_NotificationServerEndpoint.h \
  -j"$(nproc)"

# 第二阶段：编译
cmake --build Build/lagom \
  --target serenity-window-server serenity-analog-clock -j"$(nproc)"
```

产物：

```
Build/lagom/bin/serenity-window-server     # 合成器 / 窗口管理器
Build/lagom/bin/serenity-config-server     # ConfigServer（Taskbar 等 app 依赖）
Build/lagom/bin/serenity-launch-server     # LaunchServer（启动器 / 打开 app）
Build/lagom/bin/serenity-taskbar           # 桌面外壳：菜单 / 窗口列表 / 时钟
Build/lagom/bin/serenity-analog-clock      # 自带 app 示例
Build/lagom/bin/serenity-*                 # 其余 29 个 app / 游戏 / demo
Build/lagom/lib/liblagom-*.so
```

### 4.1 完整桌面会话

```
serenity-config-server   （可选，先起）
serenity-launch-server   （可选，先起）
serenity-window-server   （必须最先，客户端要连 /tmp/portal/window）
serenity-shell           （桌面 + 任务栏：菜单 / 窗口列表 / 时钟 / 图标）
serenity-<app>           （任何 app，都以 root 运行才有权限连 socket）
```

壁纸由 `Compositor` 通过 IPC 设置；宿主上没有 DisplaySettings 客户端，
所以 `main_linux.cpp` 会读 `/etc/WindowServer.ini` 的 `[Background] Path=` 自行加载：

```ini
[Background]
Path=/res/wallpapers/sunset-retro.png
Mode=Fill
```

> `/tmp/portal/{window,wm}` 是 `root:root 0600`，客户端必须同 uid（即 root）才能连接。

> **为什么要分两阶段？**
> `add_dependencies(target, generate_X)` 只保证链接前生成，不保证每个 `.o`
> 编译前生成。干净构建并行编译时会报
> `WindowServer/WindowManagerClientEndpoint.h: file not found`。
> 除了脚本里的预生成阶段，`Ports/LinuxGUI/CMakeLists.txt` 还给每个源文件加了
> `OBJECT_DEPENDS "${GENERATED_SOURCES}"`，直接 `ninja` 也能正确排序。


### 4.2 会话守护进程 serenitid

Debian 侧只拉起一个进程：

```
systemd -> serenitid -> { config-server, launch-server, window-server, login-server, shell }
```

- **启动顺序**：ConfigServer → LaunchServer →（关掉 framebuffer 启动画面）→ WindowServer →
  等 `/tmp/portal/window` 就绪 → 登录 → shell（桌面 + 任务栏）。
- **监控**：`SIGCHLD` 回收 + 崩溃重启（指数退避，最多 20 次），
  `[Session] Restart=no` 可关闭。
- **生命周期**：`SIGTERM` 按逆序停子进程（先 TERM，20×50ms 后 KILL）。
- **进度**：写 `/run/serenity-progress` 给启动画面。
- **日志**：`/var/log/serenity-session.log`（子进程 stdout/stderr 一并接入）。

配置 `/etc/serenity/session.conf`：

```ini
[Session]
Autologin=serenity      ; 留空则显示登录界面等待输入
Language=zh-CN          ; UI 语言（/res/locale/<lang>.ini）
Restart=yes
```

### 4.3 图形登录

`serenity-login-server` 复用上游 `LoginWindow`（GML + 主题），但认证成功后
不是 exec `SystemServer`，而是退出 0 交给 serenitid 启动桌面。
`Core::Account` 直接读 `/etc/passwd` + `/etc/shadow`，所以是真的校验。

### 4.4 中文翻译

本分支没有上游的 `LibLocale::Translations`，因此分两层做：

1. **移植层自己的字符串**（外壳菜单、登录、关机对话框）：`Ports/LinuxGUI/Common/Translation.h`
   读 `/res/locale/<lang>.ini` 的具名分组（`<分组>.<键>=值`），语言来自
   `[Session] Language=` 或 `$LANG`。
2. **所有 app 的字符串**（Serenity 自带软件）：见 §4.8 的运行时 `[UI]` 表——
   不动 app 源码，由 LibGUI 在设置控件文本时查表。

> 注意：默认字体 Katica/Csilla 没有 CJK 字形，中文需要给 FontDatabase 配
> CJK 回退字体（`Base/res/fonts` 里的 CJK 字体），否则会显示成方框。


### 4.5 字体与中文渲染

Serenity 的字体来自 `resource://fonts`（`/res/fonts`），支持 `.font`（位图）、
`.ttf`、`.woff`，**没有**字形回退链，所以中文必须用一个本身同时含拉丁和 CJK 的字体。

本移植用 **Sarasa Gothic SC**（更纱黑体 SC）做 UI 字体、**Sarasa Term SC** 做等宽：

```ini
[Fonts]
Default=Sarasa Gothic SC 10 400 0
FixedWidth=Sarasa Term SC 10 400 0
WindowTitle=Sarasa Gothic SC 10 700 0
```

字体不放进仓库（每个 23 MB），放在 `Build/fonts-extra/`，安装时拷进
`Build/lagom/Root/res/fonts/`：

```bash
mkdir -p Build/fonts-extra
cp /path/to/SarasaGothicSC-{Regular,Bold}.ttf Build/fonts-extra/
cp /path/to/SarasaTermSC-Regular.ttf        Build/fonts-extra/
cp Build/fonts-extra/*.ttf Build/lagom/Root/res/fonts/
```

> **坑**：`/res` 必须指向**当前构建树**的 `Root/res`
> （`ln -sfn <repo>/Build/lagom/Root/res /res`）。如果它还指着早期
> `install.sh` 装出来的 `/opt/serenity-port/Root/res`，新加的字体和翻译表都不会生效，
> 而 WindowServer 会直接 `VERIFICATION FAILED: s_default_font` 退出。

### 4.6 桌面外壳 serenity-shell（explorer 模式）

桌面（壁纸 + 图标）和任务栏（开始菜单 / 窗口列表 / 时钟 / 托盘）**在同一个进程里**，
相当于 Windows 的 `explorer.exe`：

```
serenity-shell
├── Desktop 窗口   WindowType::Desktop，全屏，DesktopIconsWidget（壁纸 + 图标 + 右键菜单）
└── Taskbar 窗口   WindowType::Taskbar，底部 27px，开始菜单 + 窗口列表 + 时钟 + 托盘
```

一个 `GUI::Application` 开两个窗口，`set_quit_when_last_window_deleted(false)`
保证关掉其中一个不会结束会话。窗口顺序：先建 Desktop、后建 Taskbar，
Desktop 类型本身也会被 WM 压到栈底。

合并带来的共享：

| 共享项 | 位置 | 原来 |
|---|---|---|
| app 扫描（/res/apps，含可执行性过滤） | `Shell/AppCatalog` | Taskbar 扫一遍、桌面"新建"菜单再扫一遍 |
| 图标缓存（按路径 + 尺寸） | `Shell/AppCatalog` | 两边各自解码 |
| `/bin/Name` → `serenity-<kebab>` | `Common/hostify.h` | 4 份拷贝（Process/Launcher/Taskbar/DesktopIcons） |

`AppCatalog` 只保留**宿主上真的存在**的 app（`resolve_executable` 后 `access(X_OK)`），
所以开始菜单和桌面"新建"菜单不会出现点了没反应的条目。

启动日志可以验证两个窗口都在：

```
shell: desktop [0,0 1280x960] + taskbar [0,933 1280x27] in one process, 40 apps in catalog
shell: desktop shows 4 shortcuts
```

serenitid 只拉起 `serenity-shell` 一个进程（原来分别拉 `desktop-icons` 和 `taskbar`）。
`Userland/Services/Taskbar/main.cpp` 仍保留给原生 SerenityOS 构建，
Linux 端口不再构建独立的 `serenity-taskbar` / `serenity-desktop-icons`。

### 4.7 运行期前缀 /serenity（与 Linux 系统目录分离）

运行期全部装在一个前缀里，`/usr/local` 不再放任何 serenity 文件：

```
/serenity/
├── bin/          serenity-*（78 个）+ serenitid（包装脚本）
├── lib/          liblagom-*.so*（二进制的 RUNPATH 是 $ORIGIN/../lib）
├── libexec/      serenitid.real
├── res/          Root/res：字体、图标、主题、locale、apps（≈81 MB）
├── share/desktop/ 系统默认快捷方式（4 个 .desktop，源在 Base/res/desktop）
└── etc/          WindowServer.ini、serenity/session.conf
```

装/升级：

```bash
sudo scripts/port/05-install-prefix.sh --systemd   # 首次或升级
sudo scripts/port/05-install-prefix.sh --clean     # 把旧的 /usr/local 副本挪走（可逆）
```

Serenity 的代码里 `/res`、`/etc/WindowServer.ini`、`/etc/serenity` 是写死的，
所以这三处保留为**指向前缀的符号链接**：

```
/res                  -> /serenity/res
/etc/WindowServer.ini -> /serenity/etc/WindowServer.ini
/etc/serenity         -> /serenity/etc/serenity
```

前缀路径由 `SERENITY_PREFIX` 传下去（systemd 单元的 `Environment=` 设置，
serenitid 没收到时按 `SERENITY_BIN` 的父目录推导）。用到它的地方：

- `Common/hostify.h`：`/bin/Name` → `$SERENITY_BIN` → `$SERENITY_PREFIX/bin` → `/usr/local/bin` → `$PATH`；
- `Shell/DesktopIconsWidget.cpp`：系统快捷方式读 `$SERENITY_PREFIX/share/desktop`，
  目录不存在时回退到旧的 `/usr/share/serenity/desktop`。

好处：升级/回滚就是换一个目录；`/res` 指向稳定路径而不是构建树
（以前 `/res` 直接指向 `Build/lagom/Root/res`，重建会把运行中的字体/主题改掉）。

> 源码树**没有**搬：它仍是工作区根目录（`/mnt/user/serenity`，VM 里 `~/serenity`），
> 构建产物仍在 `Build/lagom`。只有运行期进了 `/serenity`。

### 4.8 全局汉化：运行时 UI 字符串表

Serenity 的 app 把英文写死在源码里（`set_text("&Save")`、`set_title("File Manager")`），
本树也没有上游的 `LibLocale::Translations`。逐个改 78 个 app 不现实，所以把翻译放在
**LibGUI 的文本入口**上：

```
app 调用 set_text / set_title / add_submenu / set_title(Widget)
        ↓
LibGUI/UITranslation.cpp: translate_ui(text)
        ↓  查 /res/locale/<lang>.ini 的 [UI] 组
命中 → 返回中文；未命中 → 原样返回
```

挂钩的位置（都在 LibGUI 里，app 不用改）：

| 入口 | 覆盖 |
|---|---|
| `Action` 构造函数 / `Action::set_text` | **菜单项**、工具栏按钮（app 基本都是 `Action::create("&Open", ...)`） |
| `Menu` 构造函数 / `Menu::set_name` / `add_submenu` | 菜单栏与子菜单标题 |
| `AbstractButton::set_text`（含构造函数） | 按钮 / 复选框 / 单选框 / MessageBox 按钮 |
| `Label` 构造函数 / `set_text` | 所有静态文字、对话框正文 |
| `Window::set_title` | 窗口标题（任务栏按钮、标题栏） |
| `Widget::set_title` | 标签页标题 |
| `GroupBox::set_title` | 分组框（控制面板） |
| `Statusbar::set_text` | 状态栏文字 |

> 关键：很多 app 在**构造**时就传文案（`Action::create("&Save")`），不会走 `set_text`，
> 所以构造函数也必须查表——只挂钩 setter 会漏掉全部菜单。

匹配规则（`Userland/Libraries/LibGUI/UITranslation.cpp`）：

1. 精确匹配；
2. 去掉 `&` 助记符再匹配（表里写 `Save`，源码里是 `&Save`）；
3. 表里带 `{}` 的条目按前缀/后缀匹配格式化结果（`Score: {}` 命中 `Score: 42`）；
4. 都不中 → 原样返回。所以**文件名、用户输入、路径不会被误翻**。

文案表：`Base/res/locale/zh-CN.ini` 的 `[UI]` 组（键=英文原文，值=中文）。
目前约 790 条，覆盖内置 app 的菜单/按钮/标题/对话框（源码候选串按出现次数约 66%，
剩下的多是未构建 app 的长错误文案）。`zh-CN` 由 `[Session] Language=` 或 `$LANG` 选择，
`en.ini` 没有 `[UI]` 组时自动全英文。

查漏工具：

```bash
scripts/port/ui-strings.py            # 覆盖率 + 出现最多的未翻译串
scripts/port/ui-strings.py --all      # 全部未翻译
scripts/port/ui-strings.py --app FileManager
```

补一条文案 = 在 `[UI]` 里加一行 `English=中文`，然后
`sudo scripts/port/05-install-prefix.sh`（安装脚本会用 `Base/res/locale` 覆盖
`<prefix>/res/locale`，不需要重新编译）。改 `UITranslation.cpp` 才需要重编 LibGUI。

已知边界：

- 纯数据（`String::formatted` 拼出来又不在表里的）不会翻；表里加 `{}` 条目即可覆盖常见状态栏文字。
- 属性对话框里的元数据键（`audio_title` 等）已按 key 加进表；新增 key 需要同步加。
- 未构建的 app（Browser / Mail / PDF / Spreadsheet）的文案也在表里备了一部分。

### 4.9 Windows 应用：自动兼容层

双击 `.exe` / `.msi` / `.bat` / `.cmd` 不需要选任何东西，桌面的双击和右键"用兼容层运行"
都走同一个入口：

```
双击 .exe
   └── <prefix>/bin/serenity-winapp <file>
         ├── 判定为游戏 → <prefix>/bin/umu-run（Steam Runtime + protonfixes）
         └── 判定为应用 → <prefix>/opt/dwproton/files/bin/wine（Proton 的 wine，直跑）
```

**怎么判断是游戏还是应用**（exe 本身不会说，只能猜，猜错可覆盖）：

1. 导入表里有没有游戏专属 DLL —— `d3d9/d3d10/d3d11/d3d12`、`dxgi`、`xinput*`、
   `dinput8`、`vulkan-1`、`openal32`（这些名字以 ASCII 躺在 PE 里，`grep -a` 就能看）；
2. 同目录有没有引擎/资源特征 —— `UnityPlayer.dll`、`steam_api64.dll`、`*.pak`、
   `*.uasset`、`*.pck`、`*.rpa`、`*.assets`。

命中任一 → 走 umu；都没有 → 走 Proton wine 直跑（更快，也不会触发 protonfixes 的
首次下载）。实测：`光影魔术手.exe` → app（D3D 导入 0 个）。

覆盖方式：

```bash
serenity-winapp --game app.exe     # 强制当游戏（走 umu）
serenity-winapp --app game.exe     # 强制当普通应用
SERENITY_WINAPP_MODE=game GAMEID=1234 serenity-winapp game.exe   # 带 Steam AppID
serenity-winapp --dry-run app.exe  # 只打印判定结果
```

桌面右键还有"以游戏模式运行（umu）"，等于 `--game`。`GAMEID=0` 表示没有 per-game
修复脚本，只走运行时；知道 Steam AppID 时设 `GAMEID` 命中率更高。

`serenity-winapp`（`scripts/port/winapp-launcher.sh`）负责环境：
`DISPLAY=:99`、`WINEPREFIX=/root/.wine-dw`、`WINEDEBUG=-all`、`WINEESYNC/FSYNC=1`。
会话以 root 跑，所以前缀写死 `/root/.wine-dw`，不依赖 `$HOME`。

显示链路在**会话启动时**就绪（serenitid 的子进程）：

```
Xvfb :99 -screen 0 1280x960x24          # X 服务器
serenity-x11-bridge --display :99        # X 顶层窗口 -> Serenity 窗口
```

所以用户不需要先开 Xvfb / 桥接。桥接会把 Wine 的隐藏窗口过滤掉，否则一个 exe 会冒出
七八个空窗口：

- 小于 200×120 的跳过（消息窗、IME 窗、1×1 socket）；
- 无标题的跳过（Wine 的隐藏工作区）；
- 标题里含 `.exe` / `\` 的跳过（Wine 的启动器窗口）；
- 标题读 `_NET_WM_NAME`（UTF-8），否则中文标题读成空、整个窗口被误杀；
- 装了 `XSetErrorHandler`：客户端消失时的 BadWindow 只记日志，不再让桥接 `exit(1)`。

游戏可以用 `umu-run`（`/serenity/bin/umu-run`，C 版 umu-launcher）：
`UMU_ALLOW_ROOT=1 GAMEID=<id> umu-run <exe>`，`PROTONPATH` 不设时自动找
`/serenity/opt/dwproton`。注意 DWProton 的 protonfixes 首次会下载 FSR4 upscaler，
没网会卡住，跑普通应用建议直接用 `serenity-winapp`（绕开 protonfixes）。

> **系统 wine 已移除**。DWProton 自带 wine，够用；但 Proton 的 32 位部分依赖
> `wine-stable-i386` 带进来的 i386 运行库，purge wine 时必须补装（见 HANDOFF §3 坑 22）。


### 4.10 浏览器：用宿主 Firefox 当引擎

Serenity 的 Browser 基于 **LibWeb**（自研排版/渲染引擎）+ LibJS，兼容性只够看简单页面，
而且这套引擎不在 Lagom 构建里（`bin/` 里没有 Browser）。想把 Gecko/Blink 塞进 LibWeb
不现实——那等于重写浏览器。

实际做法和 Wine 应用同一条路：**让 Firefox 跑在 Xvfb 上，由 X11 桥映射成 Serenity 窗口**。

```
Firefox (GTK) -> Xvfb :99 -> serenity-x11-bridge -> GUI::Window（Serenity 窗框）
```

- `<prefix>/bin/serenity-webbrowser`（`scripts/port/webbrowser-launcher.sh`）：
  设置 `DISPLAY=:99`、`MOZ_ENABLE_WAYLAND=0`，exec `firefox-esr`；
- `Base/res/apps/WebBrowser.af`：`Executable=/bin/WebBrowser` → 映射到 `serenity-webbrowser`，
  出现在开始菜单"网络"里，名字经 `[UI]` 翻成"浏览器"；
- 依赖：`apt install firefox-esr`（Debian 13 是 ESR 153）。

桥接对这类客户端的两个必要修复（`x11_backend.cpp`）：

1. **`ConfigureRequest`**：我们自称 WM，客户端改自己大小是"请求"而不是直接改。不处理的话
   Firefox 会一直卡在映射时的尺寸（实测 500×120）。
2. **`apply_size_hints` 只对退化窗口生效**（宽或高 ≤ 64）：xterm 那种 1×1 + hints 的才需要
   强制尺寸，正常客户端保留自己的大小。

代价：一个 X 顶层窗口 = 一个 Serenity 窗口（标签页/弹窗各自成窗）、每帧 `XGetImage` 有开销、
输入法受限。

### 4.11 输入法（fcitx5 + LibGUI 客户端）

`TextEditor` 原本只认 `KeyEvent::code_point()`，没有输入法概念。现在拆成引擎侧和
客户端侧，中间是一条 unix socket：

```
LibGUI::InputMethod ── /tmp/serenity-ime.sock ── fcitx5 + serenityime 插件
        │                                              │
        └── InputMethodCandidateWindow（预编辑 + 候选窗）
```

- **引擎侧**：`Ports/LinuxGUI/IMEFrontend/serenityime.cpp`，一个 fcitx5 addon，
  直接拿 `InputPanel`，不需要 D-Bus。
- **客户端侧**：`Userland/Libraries/LibGUI/InputMethod.{h,cpp}`，纯 POSIX socket，
  不链接 fcitx。
- **挂钩点**：`TextEditor` 的 `keydown_event` / `focusin_event` / `focusout_event` /
  `cursor_did_change`，新增 `cursor_screen_rect()`。
- **会话**：`serenitid` 在 Wayland host 之后、shell 之前拉起 `fcitx5`
  （`GTK_IM_MODULE` / `QT_IM_MODULE` / `XMODIFIERS` 一并导出）；没装就跳过。

构建与安装：

```bash
sudo apt install fcitx5 fcitx5-chinese-addons libfcitx5core-dev
sudo scripts/port/build-ime.sh
fcitx5 -r
```

不装 fcitx5 也能测客户端：`scripts/port/ime-fake-engine.py &` 起一个同协议的假引擎，
在文本框里打 `nihao` 回车，应看到候选窗并上屏"你好"。

细节、协议表、限制见 [`Ports/LinuxGUI/IMEFrontend/README.md`](Ports/LinuxGUI/IMEFrontend/README.md)。

### 4.12 Windows 应用的中文字体

Wine 应用里中文显示成方框，不是 Serenity 的字体问题，而是 Wine 前缀里没有把
`SimSun` / `Microsoft YaHei` 这些名字映射到含 CJK 的字形：

```bash
sudo <prefix>/bin/serenity-wine-fonts --prefix /root/.wine-dw
# 或直接跑仓库里的脚本
sudo scripts/port/wine-fonts.py --prefix /root/.wine-dw
```

它做四件事：把 Sarasa Gothic SC 拷进 `drive_c/windows/Fonts/`、在
`...\CurrentVersion\Fonts` 注册、用 `FontSubstitutes` 把 19 个常见中文字体名
（含 `宋体` / `微软雅黑`）指过去、再加 `FontLink\SystemLink` 让 Tahoma/Arial
等拉丁字体在遇到 CJK 时回退。`serenity-winapp` 首次启动会自动跑一次（一次 grep
的开销），`05-install-prefix.sh` 会把它装成 `<prefix>/bin/serenity-wine-fonts`。

## 5. 运行

### 5.1 有 X11 / Wayland

```bash
sudo ./03-run.sh
# 或
sudo ./04-run-app.sh
```

### 5.2 无桌面、但有 DRM/KMS

```bash
sudo SERENITY_KMSDRM=1 ./03-run.sh
```

SDL 会走 `kmsdrm` 后端，直接扫帧缓冲，不需要 X/Wayland。

### 5.3 纯 headless（无显示设备）

```bash
sudo ./03-run.sh
```

没有 `DISPLAY` / `WAYLAND_DISPLAY` 时自动用 `SDL_VIDEODRIVER=dummy`，
并在 1.5 秒后把桌面写成 `Build/screenshot.ppm`：

```bash
# 转成 PNG 查看
convert Build/screenshot.ppm Build/screenshot.png   # ImageMagick
```

### 5.4 运行时依赖

`03-run.sh` / `04-run-app.sh` 会自动处理：

1. 把 `Base/res/{themes,cursor-themes,icons,color-schemes,...}` 软链进
   `Build/lagom/Root/res`，并把 `Base/res/fonts` 拷进去（含 Katica/Csilla）。
2. 创建 `/res -> Build/lagom/Root/res`（Serenity 代码硬编码读取 `/res`）。
3. 确保 `/etc/WindowServer.ini` 存在。
4. WindowServer 在 `/tmp/portal/{window,wm}` 上监听 Unix socket。

## 6. 目录结构

```
Ports/LinuxGUI/
  LinuxScreenBackend.{h,cpp}   SDL2 呈现后端（内存帧缓冲 -> 纹理 -> 窗口）
  main_linux.cpp               Linux 入口（替代 pledge/unveil + /dev/gpu）
  CMakeLists.txt               WindowServer 宿主目标
  Common/hostify.h             /bin/Name -> serenity-<kebab> 解析（唯一一份）
  Common/Translation.h         /res/locale/<lang>.ini 文案表
  Shell/                       桌面外壳：Desktop + Taskbar 同进程
    main.cpp                   一个 Application，两个窗口
    AppCatalog.{h,cpp}         app 扫描 + 图标缓存（开始菜单与桌面共用）
    DesktopIconsWidget.{h,cpp} 壁纸 + 图标 + 右键菜单
    SystemMenu.{h,cpp}         开始菜单（分类 / 主题 / 运行 / 退出）
  IPC/CMakeLists.txt           WindowServer/Clipboard/Notification 共享 IPC 头
  Apps/CMakeLists.txt          自带 app 宿主目标（当前 AnalogClock）

scripts/port/
  00-install-deps.sh
  01-fetch-caches.sh
  02-build.sh
  03-run.sh
  04-run-app.sh
  04-qemu-debian13.sh
```

## 7. 改了哪些上游文件

| 文件 | 改动 |
|------|------|
| `Userland/Services/WindowServer/ScreenBackend.h` | 非 SerenityOS 下本地定义 `FBRect` / `GraphicsHeadModeSetting`，避免与 glibc 的 `Kernel/API/Ioctl.h` 冲突 |
| `Userland/Services/WindowServer/Screen.cpp` | 屏蔽 `<sys/devices/gpu.h>`；`Mode::Virtual` 在宿主上改用 `LinuxScreenBackend`；`Mode::Device` 在宿主上直接返回失败 |
| `Userland/Services/WindowServer/EventLoop.cpp` | 宿主上不监听 `/dev/input` 与 `/tmp/system/devicemap`；改为自行 `listen` `/tmp/portal/{window,wm}` |
| `Userland/Services/WindowServer/KeymapSwitcher.cpp` | 宿主上没有 `/sys/kernel/keymap` 与 `/bin/keymap`，改为惰性桩 |
| `Userland/Libraries/LibKeyboard/CharacterMap.cpp` | 宿主上 `getkeymap`/`setkeymap` 返回 `ENOSYS` |
| `Userland/Libraries/LibCore/Process.cpp` | 非 SerenityOS 下把 `/bin/Name` 交给 `PortHostify::hostify_executable`（`Ports/LinuxGUI/Common/hostify.h`）解析成宿主二进制；`working_directory` 用 `posix_spawn_file_actions_addchdir_np` |
| `Userland/Services/LaunchServer/Launcher.cpp` | 同一份 `hostify` 规则，handler 注册前先解析成宿主路径 |
| `Userland/Services/FileSystemAccessServer/ConnectionFromClient.cpp` | `#ifdef __serenity__` 包住 `die()` 里的 `GUI::Application::the()->quit()`：宿主上没有 SystemServer 按需重启该服务、本进程也没有 `GUI::Application`（会空指针崩），改为断开后继续 listen |
| `Userland/Libraries/LibConfig/CMakeLists.txt` | 宿主上自行 `compile_ipc` 生成 ConfigServer 端点 |
| `Userland/Services/CMakeLists.txt` | `ConfigServer` 改为仅 SerenityOS 构建，避免重复生成 IPC 目标 |
| `Meta/CMake/lagom_options.cmake` | 新增 `ENABLE_LINUX_GUI_PORT`、`ENABLE_LAGOM_GUI_APPS` |
| `Meta/Lagom/CMakeLists.txt` | 构建 `LibKeyboard`/`LibConfig`/`LibImageDecoderClient`；完整 `LibGUI`；按开关加入 `Ports/LinuxGUI` 与 `Apps` |
| `Meta/download_file.py` | `mkdir(exist_ok=True)`，避免 `rmtree` 失败导致构建中断 |
| `Ports/.gitignore` | 放行 `LinuxGUI/`（`Ports/*/*` 默认被忽略） |

## 8. GPU 加速

分两个层次：

1. **呈现层（已启用）**：`LinuxScreenBackend` 请求
   `SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC`，SDL 自动选择
   OpenGL / Vulkan 后端，把 CPU 合成好的帧上传为纹理并 vsync 显示。
2. **合成层（未做）**：把每个窗口的 backing store（本身就是 `Gfx::Bitmap`）
   上传为 GL 纹理，用四边形 + 着色器在 GPU 上混合。仓库里已有 `LibAccelGfx`
   （OpenGL/EGL 的 `Painter` 与 `GlyphAtlas`）可复用；`ARGB32` 是预乘 alpha，
   GL 混合要用 `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`。

对无桌面的 Debian，最直接的是 `SDL_VIDEODRIVER=kmsdrm`。

## 9. 当前状态

- ✅ WindowServer（合成器、窗口管理、IPC、主题、字体）作为 Linux 进程运行
- ✅ SDL2 呈现，支持 X11 / Wayland / KMSDRM / dummy
- ✅ 完整 `LibGUI`（117 个控件源文件）在宿主编译通过
- ✅ 自带 `AnalogClock` 已接入并成功链接
- ✅ 已验证：LibGUI 117 个源文件 + WindowServer 24 个源文件在 Debian 13 / clang 21 上全部编译通过
- ⏳ 其余 50 个自带 app 需要按依赖逐个加目标；多数还差 `LibDesktop`/`LibConfig`
  的运行时服务（LaunchServer / ConfigServer）
- ⏳ 输入映射目前覆盖常用键；完整的 Serenity `KeyCode` 映射表可按需补全

## 10. 排错

| 现象 | 处理 |
|------|------|
| `Package 'sdl2' not found` / `Could not find SDL2` | `sudo apt-get install libsdl2-dev` |
| `WindowServer/WindowManagerClientEndpoint.h: file not found` | 干净构建的顺序竞态；重跑 `02-build.sh`（已含预生成阶段）。直接 `ninja` 时先跑那 8 个 `generate_*` 目标 |
| `Failed to open /res/themes/Default.ini` | 先跑 `03-run.sh`（建 `/res` 软链接并接入 `Base/res`） |
| `Unable to open cursor theme` | 同上；确认 `Base/res/cursor-themes/Default` 存在 |
| `KeymapSwitcher` 构造时 abort | 确认 `KeymapSwitcher.cpp` 的 `#ifndef __serenity__` 分支已生效 |
| 链接时报 `libSDL2-2.0.so.0: not found` | 补运行时包 `libsdl2-2.0-0` |
| `Can't restore time` | 文件系统不允许恢复 mtime；`01-fetch-caches.sh` 已改为提前解压，构建时关闭联网下载 |
| 其它编译错误 | 把 `02-build.sh` 的第一段报错发出来，按文件逐个修 |

首次全量构建会编译大量 Lagom 库（`LibGfx`/`LibIPC`/`LibUnicode`/`LibJS` 等），
耗时取决于机器；之后增量构建很快。

## 11. 回滚

```bash
git checkout -- \
  Userland/Services/WindowServer/ScreenBackend.h \
  Userland/Services/WindowServer/Screen.cpp \
  Userland/Services/WindowServer/EventLoop.cpp \
  Userland/Services/WindowServer/KeymapSwitcher.cpp \
  Userland/Libraries/LibKeyboard/CharacterMap.cpp \
  Userland/Libraries/LibConfig/CMakeLists.txt \
  Userland/Services/CMakeLists.txt \
  Meta/CMake/lagom_options.cmake \
  Meta/Lagom/CMakeLists.txt \
  Meta/download_file.py \
  Ports/.gitignore

rm -rf Ports/LinuxGUI scripts/port
# 运行期创建的软链接
rm -f /res
```

## 6. 内核模块 serenity_compat

Serenity 的用户态有几处直接跟"内核提供的东西"打交道，Linux 上并不存在。
与其在每个 app 里加 `#ifdef`，不如把这些接口在内核侧补齐：`KernelModules/serenity_compat`。

### 6.1 /dev/audio/0

`AudioServer`（上游代码，未改）打开 `/dev/audio/0` 只写，混音成 **S16_LE 立体声**，
并用 `SOUNDCARD_IOCTL_{GET,SET}_SAMPLE_RATE` 协商采样率；它依赖 `write()` 按实时速率阻塞。

模块对外就是一个虚拟声卡：

| 操作 | 语义 |
|---|---|
| `write()` | 写入环形缓冲，满了阻塞（可 `O_NONBLOCK`） |
| `read()` | 供用户态桥接取走 PCM，取走即算"已播放" |
| `ioctl 56` | 读采样率（`u32*`） |
| `ioctl 55` | 设采样率（按值传参，不是指针 —— 与 Serenity 一致） |
| `SERENITY_AUDIO_IOCTL_STATS` | written/played/underruns/pending，调试用 |

环形缓冲默认 32 KiB（`ring_kib=` 可调，2 的幂）：它同时是"最大未播音频"，
也就是延迟上限（44.1 kHz 下约 0.18 s）。

**时钟**：有人 `read()` 时由读端驱动；没人读时由 10 ms 定时器按真实流逝时间推进，
所以没有桥接也不会卡死 AudioServer。

**为什么是虚拟节点而不是直连硬件**：内核模块不能当 ALSA 客户端。真正出声的是
用户态的 `serenity-audio-bridge`（ALSA `default` 设备），因此宿主其他程序照常混音，
模块并不独占声卡 —— 它只独占自己的 `/dev/audio/0`（同一时刻一个写者，和真实声卡一致）。

### 6.2 Serenity 专有的虚拟文件

| 路径 | 消费者 | 内容 |
|---|---|---|
| `/sys/kernel/stats` | ResourceGraph | `{"total_time":ms,"idle_time":ms}` |
| `/sys/kernel/memstat` | ResourceGraph | kmalloc/physical 页数（映射自 `si_meminfo`） |
| `/sys/kernel/net/adapters` | ResourceGraph、Network | 网卡名/IPv4/链路/速率/收发字节 |

注意 `stats`、`memstat` 必须是 `kernel_kobj` 的**属性文件**（`sysfs_create_file`），
用 `kobject_create_and_add` 会变成目录，`cat` 直接报 "Is a directory"。

### 6.3 构建与安装

```bash
cd KernelModules/serenity_compat
make                 # 需要 linux-headers-$(uname -r)
sudo make install    # 装 .ko、udev 规则、tmpfiles、modules-load.d，然后 modprobe
```

udev 规则把 `/dev/serenity_audio0` 链成 `/dev/audio/0`（`/dev` 下的目录由 tmpfiles 建），
所以 AudioServer 的路径不用改。开机自动加载走 `/etc/modules-load.d/serenity-compat.conf`。

### 6.4 音频桥接

```bash
cd Ports/LinuxGUI/AudioBridge && make
sudo install -m755 serenity-audio-bridge /usr/local/bin/
```

`serenitid` 会在启动时拉起 `serenity-audio-server` 与 `serenity-audio-bridge`；
二进制缺失时只记录一行日志并跳过，不会进入重启循环。

**一个内核侧限制**：`keymap` applet 走的是 `getkeymap(2)` 系统调用，不是文件，
模块无法补（要补得改 syscall 表或给 LibKeyboard 加 Linux 分支），所以会话暂不启动它。

## 7. Ctrl+Alt+Del 与图形崩溃恢复

### 7.1 为什么不在 GUI 里监听

任务管理器是普通 GUI 客户端，图形栈挂了它自己也起不来。所以这个快捷键落在
`serenitid`（会话守护进程）里，它是唯一不依赖 WindowServer 的常驻进程。

实现只用 Linux 原生机制：`serenitid` 启动时把自己的 pid 写进
`/proc/sys/kernel/cad_pid`，内核检测到 Ctrl+Alt+Del 就会对它发 `SIGINT`。
副作用是**按 Ctrl+Alt+Del 不再重启机器**（原先由 init 处理），这正是我们要的。

### 7.2 按下之后做什么

```
CAD(SIGINT) -> 200ms 定时器取出事件
   ├─ WindowServer 健康？(进程状态 + /tmp/portal/window)
   │    否 -> SIGKILL，等它真正退出（DRM master 释放），重新拉起
   │         -> 等 socket 就绪 -> start_desktop() 补回图标/任务栏/托盘
   └─ 存在 serenity-system-monitor 就启动它，否则只记录日志
```

健康检查不能只看 `kill(pid, 0)`：被 `SIGSTOP` 或卡在 `D` 状态的进程照样"活着"，
socket 文件也还在。所以要读 `/proc/<pid>/stat` 的调度状态，`T/t/Z/D` 都算不健康。

### 7.3 两个坑

- **必须等旧进程真的没了再拉起**。SIGKILL 之后如果立刻 spawn，新实例拿不到 DRM
  master（旧 fd 还没关闭），SDL 会回退到软件路径，画面停在启动 logo 上。
- **重启定时器必须被持有**。`Core::Timer::create_single_shot()` 返回 `RefPtr`，
  存在局部变量里会立刻析构，`start()` 等于没调用 —— 表现就是"日志说在重启，进程却
  再也没出现"。用全局 `Vector<NonnullRefPtr<Timer>>` 挂住。

### 7.4 任务管理器的现状

Serenity 的 `SystemMonitor` 目前**无法在宿主上构建**：它依赖 `LibSymbolication`，
后者依赖 `LibDebug`，而 `LibDebug` 引用了 Serenity 专有的 `sys/arch/regs.h`。
另外它的进程列表来自 `Core::ProcessStatisticsReader`，读的是 Serenity 的 `/proc/all`。

所以现在的行为是：CAD 一定会把图形栈救回来；`serenity-system-monitor` 一旦可执行
（将来移植好），同一套逻辑会自动把它拉起来，不需要再改 `serenitid`。

## 8. Wayland 客户端支持（serenity-wayland-host）

一个**协议前端**：`libwayland-server` 一侧说 Wayland，`LibGUI` 一侧开 Serenity 窗口。
不需要改 WindowServer，也不用 Weston。

### 8.1 已实现的协议

| 协议 | 状态 |
|---|---|
| `wl_compositor` / `wl_surface` / `wl_region` | ✅ |
| `wl_shm` + `wl_shm_pool` | ✅（客户端画进共享内存） |
| `xdg_shell`（toplevel / popup） | ✅ |
| `wl_seat` / `wl_pointer` / `wl_keyboard`（xkb keymap） | ✅ 协议已实现 |
| `wl_output` | ✅（固定 1280×800，整数缩放） |
| `xdg-decoration` | ✅ 声明 server-side |

**没有**：`linux-dmabuf`（无 GPU 零拷贝）、`wl_data_device`（剪贴板/拖放）、
`fractional-scale`、`text-input`、`idle-inhibit`。所以 OpenGL 客户端会回退到软件，
输入法不可用。

### 8.2 装饰只显示一层

Wayland 客户端分两派：一派自己画标题栏（CSD，如 weston-terminal），一派让合成器画
（SSD）。两边都画就会出现"两层标题栏"。

处理方式：`Surface::decoration_mode` 三态 —— 只有**显式绑定 `xdg-decoration` 并要求
server-side** 的客户端才保留 Serenity 窗口边框，其余一律 `set_frameless(true)`，
让客户端自己的标题栏当唯一装饰。代价是这类窗口不能用 Serenity 的标题栏拖动。

### 8.3 运行

```bash
export XDG_RUNTIME_DIR=/tmp/xdg-wl        # 会自动创建 wayland-serenity-0
serenity-wayland-host &
WAYLAND_DISPLAY=wayland-serenity-0 weston-terminal &
```

已验证 weston-terminal 能显示、能进任务栏；**键盘输入尚未打通**（wl_keyboard 的
enter/key 已实现，但 Serenity 侧焦点没落到 SurfaceWidget 上），这是下一步。

### 8.4 黑色边框 / 阴影

CSD 客户端（weston-terminal、GTK 等）的 buffer 比可见窗口大：四周留了一圈阴影，
真正的窗口区域通过 `xdg_surface.set_window_geometry` 告知合成器。忽略这个请求就会
把阴影当内容画出来，看起来像一圈黑边。

处理：`Surface::window_geometry` 记录该矩形，`SurfaceWidget` 只从 buffer 里截取这一块
绘制，窗口尺寸也按它设置。geometry 为空（客户端没设）时才用整块 buffer。

### 8.5 关于"强制 SSD（统一装饰）"

结论：**在 Wayland 下做不到强制**。`xdg-decoration` 是"协商"，不是命令：

- 客户端可以不绑定 `zxdg_decoration_manager_v1`，照样自绘（实测 GTK3 完全不绑定，
  日志里一条 `get_toplevel_decoration` 都没有）；
- 绑定了也可以忽略我们回的 `server_side`；
- `GTK_CSD=0` 只对 **X11 后端**有效，GTK3/GTK4 在 Wayland 下始终 CSD。

因此"所有应用统一用 Serenity 窗框"只有两条路：

1. **X11 后端**：GTK 在 X11 下默认使用服务端装饰（`GTK_CSD=0` 也生效），
   配合 Xvfb/Xephyr 桥接，窗口由我们的 WM 装饰——代价是整帧拷贝、无 GPU。
2. **主题化 CSD**：接受客户端自己画标题栏，用 GTK CSS / Qt QSS 把配色、控件
   做成 Serenity 风格。结构仍是"客户端装饰"，但观感统一。

Qt 客户端会绑定 `xdg-decoration` 并接受 `server_side`（当前未在镜像内验证）。

## 9. 主题桥接（serenity-theme-gen）

目标：让 GTK/Qt 应用在**观感**上贴近 SerenityOS，而不是各走各的默认主题。

```bash
scripts/port/serenity-theme-gen.py --list                     # 列出可用主题
sudo SERENITY_THEME_DIR=/res/themes \
     scripts/port/serenity-theme-gen.py --out /usr --theme Default
```

产物：

```
/usr/share/themes/Serenity/index.theme
/usr/share/themes/Serenity/gtk-3.0/gtk.css
/usr/share/themes/Serenity/gtk-4.0/gtk.css
/usr/share/serenity/qt-style.qss
/usr/share/serenity/qt-colors.conf
```

颜色全部来自 `Base/res/themes/<name>.ini` 的 `[Colors]` 段（`Window` / `WindowText` /
`Button` / `Base` / `Selection` / `Accent` / `ActiveWindowTitleStripes` …），支持
`#rgb`、`#rrggbb`、`#rrggbbaa` 和 Serenity 的颜色名（`white` / `black` / `orange`…），
以及键之间互相引用（`Button=Window`）。

拷贝的视觉规则：**方角（border-radius 一律 0）、1px 硬边框、灰底 3D 面板、
单一强调色用于标题条和选中**——正是 SerenityOS 的观感。

配合 `serenity-wayland-run` 使用，它会自动：

```sh
GTK_THEME=Serenity            # 存在上面那个主题时
QT_STYLE_OVERRIDE=Fusion      # Qt 侧建议，配合 -stylesheet qt-style.qss
QT_WAYLAND_DISABLE_WINDOWDECORATION=1
```

> 注意：主题改变的是**配色与控件**，不改变"谁画标题栏"。GTK 在 Wayland 下仍然自绘
> 标题条（§8.5），只是现在这条标题条使用 Serenity 的强调色。

## 10. X11 桥接（统一窗框的正路）

`Ports/LinuxGUI/X11Bridge` → `serenity-x11-bridge`。

### 10.1 为什么需要它

Wayland 下装饰归客户端，`xdg-decoration` 只是建议，GTK 根本不绑定（§8.5）。
X11 下装饰归 **窗口管理器**。所以桥接做的事很简单也很关键：**自称 WM**。

```c
XChangeProperty(root, _NET_SUPPORTING_WM_CHECK, ...);   // 让工具包相信有 WM
XSelectInput(root, SubstructureRedirectMask | ...);     // 接管 MapRequest
```

一旦工具包认为有 WM，它就不再画 CSD；每个顶层 X 窗口被映射成一个 `GUI::Window`，
于是 Serenity 的窗框、任务栏条目、焦点处理全部自动适用。

### 10.2 结构

```
X11 客户端 -> Xvfb :99 -> serenity-x11-bridge -> GUI::Window（Serenity 窗框）
                               ^ XGetImage 抓帧 / XTest 注入输入
```

- `x11_backend.{h,cpp}`：**唯一**包含 Xlib 的编译单元。X.h 定义了 `KeyCode`、`None`、
  `Bool`，与 LibGUI/AK 冲突，所以对外只暴露纯 C++ 接口。
- `x11_bridge.cpp`：每个窗口一个 `X11SurfaceWidget`，定时器里抓帧 + 派发事件。
- `Application::set_quit_when_last_window_deleted(false)`：桥接必须比客户端活得久。

### 10.3 运行

```bash
Xvfb :99 -screen 0 1024x768x24 &
serenity-x11-bridge --display :99 --interval 33 &
DISPLAY=:99 GDK_BACKEND=x11 xclock -geometry 240x240 &
```

### 10.4 实测与边界

- `xclock`：**单层** Serenity 标题条 + 任务栏条目，正是想要的效果。
- `gtk3-demo`：进入 Serenity 窗口，但 `GtkApplicationWindow` 自己仍画 headerbar
  —— 那是应用的选择（它显式设置了 titlebar），不是 WM 的锅。
- 代价：每窗口每帧一次 `XGetImage` + 整帧 blit。这是"统一外观"的性能价格，
  真要优化得用 SHM pixmap + damage 区域。
- **输入注入（XTest）尚未实机验证**：代码已接好（鼠标坐标换算到 root，键盘用
  keysym），但没有用能回显输入的客户端（如 xterm）跑过。

## 11. 输入路径：evdev → Serenity 设备语义

### 11.1 为什么不再走 SDL

端口的 WindowServer 原来用 `SDL_PollEvent` 喂 `ScreenInput`。实测（guest 内核确认收到事件、
window-server 也打开了全部 `event*` 节点，但屏幕无光标、点击无反应）说明事件在 SDL 这一层丢了。
SDL 在 kmsdrm 下如何处理 virtio-tablet 的 `EV_ABS` 是个黑盒，不适合作为输入的唯一来源。

改为和音频一致的**两段式**：

```
内核 (serenity_compat):  /dev/mouse/0  /dev/keyboard/0   ← Serenity 原生记录格式
用户态: serenity-input-bridge → 读 evdev、翻译、写入上面两个设备
端口:  WindowServer 直接 read() 设备，SDL 只保留视频职责
```

### 11.2 设备 ABI（与内核头文件逐字段一致）

```c
struct MousePacket { int x, y, z, w; unsigned char buttons; bool is_relative; };   // 20 字节
struct KeyEvent    { KeyCode key; u8 map_entry_index; u64 scancode;
                     u32 code_point; u8 flags; bool caps_lock_on; };              // 24 字节
```

`KeyCode` 的数值由 `scripts` 从 `Kernel/API/KeyCode.h` 的 `ENUMERATE_KEY_CODES` 生成（见
`InputBridge/serenity_keycode.h`），不要手抄——枚举顺序就是 ABI。
`flags` 里 `0x80` 是 Is_Press，低 6 位是 `Mod_*`。

### 11.3 用户态 bridge

```bash
cd Ports/LinuxGUI/InputBridge && make
sudo ./serenity-input-bridge                      # 自动扫描输入设备
sudo ./serenity-input-bridge --debug \            # 只打印翻译结果，不写设备
     --mouse /dev/input/event4 --keyboard /dev/input/event5
```

- 相对指针 → 发增量（`is_relative = true`）；绝对指针（tablet）→ 按 `EVIOCGABS` 的
  maximum 换算成屏幕像素后发绝对坐标。
- 键盘：evdev → Serenity KeyCode 表 + 修饰键状态 + US 布局的 `code_point`（含 Shift/Caps）。
- 未识别的键直接丢弃，不猜。

### 11.4 已验证 / 未验证

- ✅ 绝对指针：QMP 注入 `abs 3000/32767` → `x=117`（3000/32767×1280），换算正确。
- ✅ 键盘：`a`/`b` 的按下抬起、`Shift+a` → `char='A'` 且 flags 为 `0x84`、`ret` → `Key_Return(4)`。
- ⬜ 鼠标按键位：QMP 的 `btn` 事件被 QEMU 路由到了 legacy 鼠标节点，没进被监听的 event4，
  所以按键位这一段还没在实机上验证（真实窗口的点击走的是 tablet，行为和注入不同）。
- ⬜ 内核侧 chardev 尚未接入；bridge 目前只能 `--debug` 输出。

### 11.5 后续

1. `serenity_compat` 加 misc 字符设备（ring buffer + `poll`），格式即 11.2。
2. WindowServer 的 `ScreenInput` 改成 read 设备线程，删掉 `pump_sdl_events`。
3. 热插拔：监听 `EV_*` 设备的 inotify / netlink uevent，而非固定 event 号。

## 12. 显示与输入：KMS + libinput（对齐主流合成器）

### 12.1 显示

`LinuxScreenBackend` 现在有两条呈现路径，默认 KMS：

```
SERENITY_PRESENTER 未设置 -> DRM/KMS（dumb buffer + drmModePageFlip + vblank 等待）
SERENITY_PRESENTER=sdl   -> 旧 SDL 路径（仅窗口化开发用）
```

关键约束（踩过的坑都写在这）：

- **模式必须在分配 framebuffer 之前定下来**。`static probe_display_size(w,h)` 在 `main`
  里先问出显示模式，用它建初始布局；启动后不再改分辨率。后端在 `set_head_mode_setting()`
  里偷改尺寸会让布局与缓冲不一致（黑边/越界/崩溃）。
- **不要读 CRTC 的当前模式当分辨率**。virtio-gpu 的模式跟着宿主窗口尺寸走，读回来会漂
  （实测 1235x682）。主流合成器同样是从模式表里选：精确匹配 → 最小覆盖 → 最接近。
- **退出**：`restore_display()` 后 `_exit()`。完整析构会触发 `!m_ref_count` 断言，
  崩溃退出会让 systemd 重启时旧进程仍持 DRM master，新实例 SetCrtc 失败。
- **退出时不要恢复 saved_crtc 的 buffer_id**（可能是已销毁的 FB），应 `drmModeSetCrtc(fd, crtc, 0, ...)`
  禁用 CRTC 交回 fbcon。

### 12.2 输入

改用 **libinput**（Mutter/KWin/wlroots 用的同一套），不再需要 evdev bridge 与 `/dev` 设备：

```
libinput（udev 枚举、热插拔、绝对/相对、加速度）
   -> MousePacket / KeyEvent
   -> ScreenInput
```

- `Ports/LinuxGUI/LinuxInput.{h,cpp}`：初始化 seat0，8ms 定时器里 `dispatch()`。
- 绝对设备用 `libinput_event_pointer_get_absolute_x_transformed(e, 0xffff)` —— 正好是
  ScreenInput 期望的 16 位范围。
- 相对设备做了亚像素累积，慢速移动不会"卡住不动"。
- 按键映射走 `Common/keymap.h`（evdev → Serenity KeyCode + US 布局 code_point + 修饰键）。
- `/dev/mouse/0`、`/dev/keyboard/0` 保留为 libinput 起不来时的兜底。

### 12.3 实测状态（VM，virtio-gpu + virtio-tablet）

| 项 | 结果 |
|---|---|
| 桌面渲染 | 1280x960，KMS 页翻转，无黑边 |
| 鼠标移动 | 注入 50% 屏幕位置 → 光标落在 640,480 |
| 鼠标点击 | 点击任务栏"开始" → 菜单弹出 |
| 键盘 | 方向键在菜单中移动选中项（关于→主题→设置） |

SDL 在 KMS 模式下不再被调用（库仍因链接关系映射进进程）。
