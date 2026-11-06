# Windows 程序管理（WinAppsSettings）— 进度与下一步

> 更新时间：2026-11-03
> 分支状态：改动**尚未提交**，**尚未部署到 VM**
> 相关文档：`HANDOFF.md`（总交接）、`LINUX_GUI_PORT.md`（技术细节）、`.workbuddy/memory/2026-11-03.md`（当日日志）

---

## 一、当前在做什么

把「用脚本管理 exe」升级为 **omikuji 风格的 Windows 程序管理器**：一个注册表 + 每个程序一个独立 WINEPREFIX，
并在 **设置（Control Panel）** 里放一个 Serenity 风格的 applet 管理它。

用户可见的三件事必须同时成立：

| 目标 | 实现载体 | 状态 |
|---|---|---|
| 设置里有「Windows 程序管理」面板 | `Userland/Applications/WinAppsSettings/` | ✅ 宿主编译通过，未部署 |
| 每个添加的游戏生成桌面快捷方式 | `~/.local/share/serenity/winapps/wrappers/<slug>` + `~/Desktop/<slug>.desktop` | ✅ 代码就绪，未验证 |
| 开始菜单里出现「游戏」子菜单 | `~/.local/share/serenity/winapps/apps/<slug>.af`，`Category=&Games` | ✅ 代码就绪，未验证 |

---

## 二、已完成

### 1. 共享层 `Ports/LinuxGUI/Common/WinApps.h`（新增，header-only）

注册表 + 启动器的唯一真相源，`serenity-shell` 和 `WinAppsSettings` 共用。

- **注册表**：`~/.config/serenity/winapps.ini`，分组 `App-<id>`，字段 `Id/Slug/Name/Exe/Prefix/Mode`
- **每程序独立前缀**：`~/.local/share/serenity/winapps/prefixes/<slug>`（Ofuda 式，不共用）
- **启动包装器**：`~/.local/share/serenity/winapps/wrappers/<slug>`，内容为
  `exec /serenity/bin/serenity-winapp --prefix <prefix> --mode <auto|app|game> '<exe>'`
  —— 桌面快捷方式、开始菜单项、管理面板三者**共用这一个启动原语**，保证任何入口都跑在同一个 prefix 里
- **API**：`list_programs` / `find_by_id` / `find_by_exe` / `add_program` / `remove_program` / `set_mode`
  / `write_wrapper` / `write_desktop_shortcut` / `write_menu_entry` / `write_artifacts` / `remove_artifacts`
  / `remove_prefix` / `spawn_program` / `launch_program`

### 2. 管理面板 `Userland/Applications/WinAppsSettings/`（新增）

Serenity 风格控件：`GUI::ListView` 列表 + 详情行（路径 Label + 模式 ComboBox）+ 按钮行
（安装… / 启动 / 打开目录 / 卸载）。模式 ComboBox 可切 `auto / app / game`，切换即写回注册表并重写包装器。

- `Base/res/apps/WinAppsSettings.af`（新增）：`Category=Settings`，`Executable=/bin/WinAppsSettings`
- `Userland/Applications/Settings/main.cpp`（改）：Control Panel 分组加 `{ "Windows 程序", { "WinAppsSettings"sv } }`
- `Userland/Applications/CMakeLists.txt`（改）：`add_subdirectory(WinAppsSettings)`

### 3. 开始菜单接线

- `Ports/LinuxGUI/Shell/AppCatalog.cpp`（改）：`scan()` 除系统 `/res/apps` 外，**额外扫描用户 apps 目录**
  （`WinApps::user_apps_dir()`），用户 `.af` 因此自动进入开始菜单
- `Ports/LinuxGUI/Shell/DesktopIconsWidget.cpp`（改）：双击 exe 时先查注册表（`WinApps::find_by_exe`），
  命中则用该程序自己的 prefix + 存的 mode 启动；未命中才回退旧的 `--app` / `--game` 路径

### 4. 启动器 `--mode` 打通（本轮修复）

`scripts/port/winapp-launcher.sh` 原来只认 `--game` / `--app` / 环境变量 `SERENITY_WINAPP_MODE`，
**没有 `--mode`**，而包装器、`spawn_program`、桌面双击三处都发 `--mode` → 全部落空。

已补：解析 `--mode <auto|app|game>`、校验非法值退出 2、更新 usage 文案。

### 5. 宿主编译：已通过 ✅

```
cmake --build Build/lagom --target serenity-win-apps-settings -j6
→ Build/lagom/bin/serenity-win-apps-settings
```

### 6. 顺手修掉的两个真实 bug

**（a）`Category=Games` → `Category=&Games`（本轮刚改）**
分类字符串是**精确 key**：`SystemMenu.ini` 用 `&Games=/res/icons/16x16/games.png` 挂图标，
现有系统游戏也写 `Category=&Games`。写裸 `Games` 会**多出一个没有图标的重复「游戏」子菜单**，
而且 `translate_ui` 只在去掉 `&` 后才查词条（`zh-CN.ini` 里是 `Games=游戏`），两种写法显示名相同、
但会并排出现两次。已把 `WinApps.h` 的 `write_menu_entry` 改成 `Category=&Games`。

**（b）`Core::FileSystem` → `FileSystem`**
`LibFileSystem` 的命名空间是 `FileSystem`（不是 `Core::FileSystem`），`AppCatalog.cpp` 同步改正。

---

## 三、接下来做什么

### P0 — 部署到 VM 并端到端验证（**当前唯一待办主线**）

宿主 Lagom 只能查编译错误，**部署的二进制必须在 VM 内构建**（宿主 glibc 更新，宿主产物会报
`GLIBC_2.43 not found`）。

```bash
# 1) 同步源码到 VM（整目录带尾斜杠，禁止多文件平铺 rsync）
rsync -az --delete ./ serenity@127.0.0.1:/home/serenity/serenity/ \
    -e 'ssh -i Build/vm/id_ed25519 -p 2222'

# 2) VM 内构建
ssh -i Build/vm/id_ed25519 -p 2222 serenity@127.0.0.1 \
  'cd ~/serenity && cmake --build Build/lagom --target serenity-shell serenity-win-apps-settings -j$(nproc)'

# 3) 部署（05-install-prefix.sh 会顺带把改过的 winapp-launcher.sh 装成 /serenity/bin/serenity-winapp）
ssh ... 'cd ~/serenity && PREFIX=/serenity scripts/port/05-install-prefix.sh'
```

> ⚠️ 部署 `serenity-shell` 时会 `Text file busy` —— 必须先 `systemctl stop` 再 `cp` 再 `start`。

### P1 — VM 内验证清单

- [ ] `serenity-shell` 与 `serenity-win-apps-settings` 在 VM 内编译无错（`AppCatalog.cpp` /
      `DesktopIconsWidget.cpp` 的改动只有 VM 构建才真正编译过，宿主没验）
- [ ] `/serenity/bin/serenity-win-apps-settings` 存在且可执行
- [ ] `/serenity/bin/serenity-winapp` 支持 `--mode`：`serenity-winapp --mode game --dry-run <exe>` 打印 `-> game`
- [ ] 设置 → 「Windows 程序」分组里能看到并点开面板
- [ ] 装一个测试 exe → 自动生成：wrapper、桌面 `.desktop`、开始菜单 `.af`
- [ ] 开始菜单「游戏」子菜单里出现该程序，**且与系统游戏在同一个子菜单下**（不是多一个）
- [ ] 双击桌面图标 → 在**该程序自己的 prefix** 里跑起来（`WINEPREFIX` 指向 `prefixes/<slug>`）
- [ ] 卸载 → 确认弹窗 → 三样产物都被清掉，注册表条目消失

### P2 — 已知需要复核的点

- **桌面目录一致性**：`WinApps::desktop_dir()` 用 `Core::StandardPaths::desktop_directory()`，
  `DesktopIconsWidget.cpp` 自己有一份 `desktop_directory()`（line 90）。两者在 Linux 上是否指向同一处
  （`~/Desktop` vs XDG `user-desktop`）需要实测；不一致会导致「装完看不到图标」。
- **`WinApps.h` 里 `FileSystem::remove` 的 `RecursionMode::Disallowed`** 用于删包装器/快捷方式，
  删目录（prefix）时才用 `Allowed`。`remove_artifacts` 里的返回值被丢弃，删不掉时无提示——
  可考虑加错误上报。
- **注册表并发**：现在是 ini 明文读写，多实例同时装程序可能互相覆盖。暂时可接受。

### P3 — 后续可做（不阻塞）

- 终端里同步一份 `serenity-winapp` 的 CLI（当前只有 GUI 入口）
- 面板里加「运行参数」输入框（`AppFile` 的 `Arguments` 按空格切分，含空格路径会坏，故目前走包装器绕开）
- prefix 创建 / 首次引导（现在由 `serenity-winapp` 首次运行时自行 bootstrap）
- 卸载时可选「同时删除运行环境」——已实现（两次确认弹窗），需在 VM 验证

---

## 四、文件清单

### 新增
| 文件 | 作用 |
|---|---|
| `Ports/LinuxGUI/Common/WinApps.h` | 注册表 + 启动器共享层（header-only） |
| `Userland/Applications/WinAppsSettings/main.cpp` | 管理面板 UI |
| `Userland/Applications/WinAppsSettings/CMakeLists.txt` | 构建定义（链接 `LibURL`） |
| `Base/res/apps/WinAppsSettings.af` | 开始菜单/Control Panel 入口 |

### 修改
| 文件 | 改动 |
|---|---|
| `scripts/port/winapp-launcher.sh` | **新增 `--mode` 解析 + 校验** |
| `Ports/LinuxGUI/Shell/AppCatalog.cpp` | 扫描用户 apps 目录；`Core::FileSystem`→`FileSystem` |
| `Ports/LinuxGUI/Shell/DesktopIconsWidget.cpp` | 双击 exe 优先走注册表（独立 prefix） |
| `Userland/Applications/Settings/main.cpp` | Control Panel 加「Windows 程序」分组 |
| `Userland/Applications/CMakeLists.txt` | `add_subdirectory(WinAppsSettings)` |
| `Ports/LinuxGUI/Apps/generated-apps.cmake` | 重新生成（`gen-app-targets.py`），加 `LibURL` |

---

## 五、本轮踩到的编译坑（宿主 AK/GUI API 差异）

写这个面板时在 AK/GUI 上踩了一串，都已修，记下来省得下次再踩：

| 症状 | 真实原因 | 正确写法 |
|---|---|---|
| `no matching constructor for 'GUI::Button'` | `Button`/`Label` 构造收 `String`，**不是** `StringView` | `"安装..."_string` |
| `no viable conversion from 'const char[N]' to 'StringView'` | `StringView` **没有** `char const*` 单参构造（只有两参版） | `"文字"sv` |
| `no member named 'SelectionMode' in namespace 'GUI'` | `SelectionMode` 嵌套在 `GUI::AbstractView` 里 | `GUI::AbstractView::SelectionMode::SingleSelection` |
| `call to deleted constructor of 'ModelSelection'` | `ModelSelection` 不可拷贝 | `auto const& selection = list_view.selection();` |
| `cannot form a reference to 'void'` | `Core::File::close()` 返回 `void`，`TRY()` 不能套 | 直接 `file->close();` |
| `no member named 'FileSystem' in namespace 'Core'` | 命名空间是 `FileSystem` | `FileSystem::is_directory(...)` |
| `undefined symbol: URL::create_with_file_scheme` | 漏链库 | `target_link_libraries(... LibURL)` |

> 规律：本仓库里 **`String` 系（Label/Button）用 `_string`，`StringView` 系用 `sv`，不要混。**

---

## 六、环境备忘

- VM：`serenity@127.0.0.1:2222`，密钥 `Build/vm/id_ed25519`，VM 源码树 `~/serenity`
- 部署前缀：`/serenity`；桌面外壳单进程 `serenity-shell`（桌面 + 任务栏）
- 当前 VM 上跑的是 **11-02 19:57 构建的旧 `serenity-shell`**，本轮改动一个都还没生效
- `/serenity/bin/serenity-win-apps-settings` **尚不存在**
- 截图：`Build/vm/shot.py`；交互：`Build/vm/vncshot.py`

---

## 七、2026-11-03 19:00 优化轮（已在 VM 内构建 + 部分运行验证）

本轮目标是健壮性与一致性，不新增功能。改动 5 个文件：

| 文件 | 改动 |
|---|---|
| `Ports/LinuxGUI/Common/WinApps.h` | 列表按 id 稳定排序；`add_program` 查重 Exe；`is_valid_mode` + `set_mode` 校验；`remove_artifacts` 改 `ErrorOr<void>` 并返回首个错误；`.desktop` 去掉 `Path=` |
| `Userland/Applications/WinAppsSettings/main.cpp` | 装前查重并选中已有行；`write_artifacts` 失败回滚注册表；卸载先问 prefix 再动手、错误汇总一次上报；`reload` 按 program id 选行；详情行改显示 `运行环境：<slug>`，状态栏显示完整 prefix |
| `Ports/LinuxGUI/WinAppLauncher/main.cpp` | 注册表路径优先 `XDG_CONFIG_HOME`；统一 `chdir(dirname(exe))`；`cjk_font_repair` 移到 `--dry-run` 之后 |
| `Build/vm/vncshot.py` | 指针坐标按 VNC 实测分辨率归一化；QKeyCode 改 union 对象（原来键注入完全无效）；新增 `type:TEXT` |
| `HANDOFF_WINAPPS.md` | 本节 |

### 三个值得记住的坑

1. **`selection().set()` 在 `set_model()` 之后无效** —— 选择会丢，按钮集体变灰。改用
   `table.set_cursor(index, GUI::AbstractView::SelectionUpdate::Set)`。
2. **`Path=` 不是工作目录** —— 本端口 `DesktopIconsWidget::file_target()` 把它当"快捷方式指向
   的文件"（右键压缩目标）。cwd 统一由 `serenity-winapp` 的 `chdir` 负责，任何入口都一致。
3. **`pkill -f win-apps-settings` 会杀掉自己的 ssh**（远程命令行里含同串）→ 退出码 255。
   用 `pkill -f "^/serenity/bin/serenity-win-apps-settings"`。

### 已验证（VM，1600x831）

- [x] 面板在 VM 内编译、部署、启动，列表按 id 排序、两行数据显示正确
- [x] 选中行 → `运行环境：<slug>` + 模式下拉 + 状态栏完整 prefix + 按钮启用
- [x] 模式切 `自动 ↔ 游戏`：注册表 `Mode=` 落盘、wrapper 重写为 `--mode game`，**切换后该行仍保持选中**（原先会丢选中、按钮全灰）
- [x] `serenity-winapp --dry-run` 走 `XDG_CONFIG_HOME` 注册表命中 `Mode=game`；无注册表回落启发式 `app`
- [x] `serenity-winapp --mode app` 显式覆盖生效

### 仍未验证

- [ ] FilePicker → `add_program` → `write_artifacts` 全链路（宿主机打字进 VM 会走 IME 合成）
- [ ] 卸载确认/错误汇总弹窗
- [ ] 桌面 `.desktop` / 开始菜单 `.af` 的实际生成内容（应与本节描述的格式一致）
