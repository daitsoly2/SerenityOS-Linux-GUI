# 交接文档 · 2026-11-06 下午场（开始菜单重构 → TTPlayer 复刻）

> **本轮在 18:13 按用户要求「暂停」**。代码改动全部落盘并已部署到 Dev VM，
> 现场（VM 进程、QMP socket、截图、`/tmp/serenity-session.log`）**保持原状未清理**，
> 下一次可直接接手。本文是唯一接手入口，请先读完再动手。

---

## 0. 一句话状态

开始菜单的 **右键确认删除已实测通过**；**拖拽归类/拖出删除未通过**，卡在
WindowServer 没把 drag 事件派发出来（诊断线索见 §4）。
**下一个主任务**是用户明确要求的 **TTPlayer 复刻千千静听并替代原生 SoundPlayer**（§5）。

---

## 1. 环境与两棵树（务必先认清，容易搞错）

| 项 | 路径 | 作用 |
|---|---|---|
| 旧树 | `/mnt/user/serenity` | 本文档所在；日常编辑、VM 脚本、ISO 脚本、memory 都在这 |
| 新树 | `/mnt/user/serenity-linuxgui` | **干净 Lagom 编译树**，`Build/lagom` 在这里，`CMAKE_HOME_DIRECTORY` → `Meta/Lagom` |
| Dev VM 磁盘 | `/mnt/user/serenity/Build/vm/serenity-mini.qcow2` | 当前**正在运行**，GUI 窗口模式 |
| ISO 工程 | `/mnt/user/serenity-iso/` | live ISO rootfs / 脚本 |
| M4Player 参考 | `/home/ovo/下载/M4Player` | 千千静听 UI 参照（只读参考，不直接用） |

**关键非对称**：
- 本轮「开始菜单 + WS 菜单协议」的 20 个改动文件，两棵树 **md5 完全一致**（已逐一比对）。
- **但 `Userland/Applications/TTPlayer/` 只存在于新树** `/mnt/user/serenity-linuxgui/`，
  旧树**没有**这个目录；`Base/res/apps/TTPlayer.af`、`Base/res/locale/zh-CN.ini` 的
  `TTPlayer=TTPlayer 音乐播放器`（第 1434 行）也是新树独有/新树更新。
  → **TTPlayer 的工作请直接在新树做**，不要按旧树同步习惯去旧树找源码。
- `Build/vm/vm.sh` 只在旧树（新树不含 `Build/`）。

### VM 现场（保持运行中，勿随手 kill）
```
qemu-system-x86_64 ... -device virtio-vga,xres=1600,yres=900 \
  -display gtk -vnc 127.0.0.1:1 \
  -monitor unix:/tmp/qemu-mon.sock -qmp unix:/tmp/qemu-qmp.sock \
  -serial file:/mnt/user/serenity/Build/vm/serial.log
```
- SSH：`ssh -i /mnt/user/serenity/Build/vm/id_ed25519 -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR serenity@127.0.0.1`
- 会话日志（**应用侧 dbgln 在这里，不在 journalctl**）：VM 内 `/tmp/serenity-session.log`；
  `journalctl -u serenity-desktop.service` 只有 start/stop，没有应用输出。
- 桌面服务：`sudo systemctl stop|start serenity-desktop.service`；日志中成功标志是
  `shell: Start menu rebuilt`。
- 桌面几何：`desktop [0,0 1600x900]` + `taskbar [0,872 1600x28]`（此前也曾报 1599 宽）。

---

## 2. 用户需求原文与 🔒 不可更改的判定（避免重蹈覆辙）

用户本轮连续纠正过三次，**这些是硬约束，不要改回去**：

1. 「桌面（单纯桌面）右键菜单的打开方式会导致桌面重启」→ 随后澄清
   **「不是未知格式，是 windows 应用管理创建的 .desktop 文件」**。别按未知格式排查。
2. 「还有开始菜单中**不是快捷方式菜单**，是**拖拽增删（拖到其它菜单之类的），右键删除快捷方式！！！**」
   → 🔒 **不要**再做独立的「快捷方式(&S)」子菜单；所有条目直接是顶层菜单项，
   靠拖拽改分类、右键删除。
3. 「**右键不是直接删除，给出一个删除提示，防止错误删除**」
   → 🔒 右键必须弹 `GUI::MessageBox` Yes/No 确认框，**只有点「是」才删**。

用户通过选择确认的范围：**「扩展 WS 菜单协议」** + **「所有开始菜单条目」**
（含系统 app，通过 override 配置实现，不直接改只读 `/res/apps/*.af`）。

最新主任务原话（§5）：
> 「ttplayer这个项目是复刻windows下的千千静听，不是现在这个样子，ui布局可以参照
> /home/ovo/下载/M4Player（或者直接用这个修改，这个项目比较老，需要优化和接入serenity）
> 然后用来替代原生那个音频播放器」

---

## 3. 本轮已完成的代码改动（20 个文件，两树一致）

### 3.1 WindowServer 菜单协议扩展
| 文件 | 改动 |
|---|---|
| `Userland/Services/WindowServer/WindowClient.ipc` | 新增两条异步消息（**`.ipc` 内不能写 `//` 块注释，IPCCompiler 会 VERIFY 失败**）：<br>`menu_item_context_menu(i32 menu_id, u32 identifier, Gfx::IntPoint screen_position) =\|`<br>`menu_item_drag_dropped(i32 source_menu_id, u32 source_identifier, i32 target_menu_id, i32 target_identifier, Gfx::IntPoint screen_position) =\|`<br>约定 `target_menu_id == -1` = 拖到所有菜单之外 = 删除；`target_identifier == -1` = 落在菜单但没落到条目上 |
| `Userland/Services/WindowServer/MenuManager.h` | 加 `MenuHit`、`topmost_menu_at()`、`hit_test(require_item)`、`reset_item_drag()`、`bool handle_item_drag_or_context_menu(MouseEvent&)`，及拖拽状态成员 |
| `Userland/Services/WindowServer/MenuManager.cpp` | `handle_mouse_event()` **开头**先调 `handle_item_drag_or_context_menu()`；Primary MouseDown 武装拖拽候选，MouseMove 超过 6px 进入 dragging，Secondary MouseUp 发 context_menu，Primary 拖拽释放发 drag_dropped，两者都 `close_everyone()`；Secondary MouseDown 在菜单内吞掉（否则菜单会在 press 时关闭、release 变成普通激活）。实现见 §4 引用的行号 |
| `Userland/Services/WindowServer/Menu.h/.cpp` | 新增 public `int item_index_at_screen_position(Gfx::IntPoint)`，内部转调 private `item_index_at(position - m_menu_window->position())` |

### 3.2 LibGUI 客户端派发（🔒 ABI 约束）
- `Userland/Libraries/LibGUI/Menu.h/.cpp`：
  **不要给 `GUI::Menu` 加数据成员！** `EventReceiver.h:28` 的 `C_OBJECT` 内联 `construct()`
  在每个客户端二进制里 `new Klass(...)`，改 `sizeof(Menu)` 会让**未重编的 app 内存越界**。
  本轮改成 `Menu.cpp` 里的静态旁表：
  ```cpp
  static HashMap<Menu*, MenuItemCallbacks>& item_callbacks(); // { on_context, on_drop }
  static void set_item_callbacks(Menu&, Function<...>, Function<...>);
  static void dispatch_item_context_menu(int menu_id, unsigned identifier, Gfx::IntPoint);
  static void dispatch_item_drag_dropped(int src_menu, unsigned src_id, int tgt_menu, int tgt_id, Gfx::IntPoint);
  ```
  `Menu::~Menu()` 里 `item_callbacks().remove(this)`。
- `ConnectionToWindowServer.h/.cpp`：override 两条新消息 → 调 `Menu::dispatch_*`。
- `ActionGroup.h/.cpp`：新增 `void clear()`。**原因**：`Action` 析构不会从 `ActionGroup`
  反注册，重建菜单树会留悬垂指针；rebuild 前必须先 clear `s_themes_group`。

### 3.3 Shell / 开始菜单
| 文件 | 改动 |
|---|---|
| `Ports/LinuxGUI/Shell/StartMenuOverrides.h/.cpp`（新增） | 用户级 override INI `~/.config/start-menu-overrides.ini`；`config_path()/get()/set_hidden()/set_category()/clear()`；文件不存在时 `ConfigFile::open(..., AllowWriting::Yes)` 创建。系统 app 只读 `/res/apps/*.af` 和 XDG `.desktop` 靠它隐藏/改分类 |
| `StartMenuShortcuts.h/.cpp` | `struct Shortcut` 加 `Category`；`load()` 读它；写 `.af` 的 `Category=` **必须写 `&Utilities`（带 &）**，写裸 `Utilities` 会多出一个英文分类；新增 `set_category(path, category)`（ConfigFile 重写并 sync）；抽出 `prompt_add(GUI::Window&)`（3 个 InputBox） |
| `SystemMenu.h/.cpp`（整体重写） | 删掉「快捷方式(&S)」子菜单；所有 app 条目作为顶层 managed action 经 `add_managed_action()` 绑定；`bind_menu()` 用 `GUI::Menu::set_item_callbacks` 注册右键/拖拽回调；`start_menu_context()` → `remove_with_confirmation()`（MessageBox Warning / YesNo，标题「删除快捷方式」）；`start_menu_dropped()` 解析目标分类（含落在 category item 上时用其 submenu binding）、调 `apply_category()`、`schedule_rebuild()`；拖出所有菜单 → 也走确认删除；顶层单条「添加快捷方式(&A)...」；`schedule_rebuild()` 用 `Core::deferred_invoke` 避免在自身 handler 里销毁菜单 |
| `Shell/main.cpp` | 安装 `set_start_menu_rebuilder([]{ ... build_system_menu; taskbar_window->add_system_menu(menu); })` |
| `Shell/CMakeLists.txt` | 加 `StartMenuOverrides.cpp` |
| `Build/vm/vm.sh`（仅旧树） | `-device virtio-vga,xres=${VM_WIDTH},yres=${VM_HEIGHT}`（默认 1600×900）+ `-display gtk`，支持 `VM_WIDTH/VM_HEIGHT` 覆盖 |
| `/mnt/user/serenity-iso/qemu-iso.sh`, `qemu-direct.sh` | `-vga virtio` → `-vga none -device virtio-vga,xres=1600,yres=900 -display gtk -vnc :0` |

### 3.4 VM 内已部署的二进制
`/serenity/bin/serenity-window-server`、`/serenity/bin/serenity-shell`、
`liblagom-gui.so.0.0.0`、`liblagom-desktop.so.0.0.0`（scp 到 `/tmp` → stop → install → start）。
VM 内测试快捷方式：`~/.local/share/start-menu/拖拽测试.af`（`Executable=/usr/bin/xterm`，
**当前没有 `Category=` 行**，所以显示在根菜单）；`~/.config/start-menu-overrides.ini`
里有一条 `[/res/apps/Eyes.af] Category=D&evelopment`（早期测试残留，可留可删）。

---

## 4. ⚠️ 卡住的问题：拖拽归类未生效（下一次从这里开始）

### 4.1 已验证通过的部分（别重复测）
- 右键「右键测试」→ 弹出确认框「删除快捷方式」，正文
  `确定从开始菜单删除「右键测试」吗？ / 仅从开始菜单移除，不会卸载或删除程序。`，按钮 是/否。
- 点「否」→ `.af` 保留；点「是」→ `.af` 被删，日志出现 `shell: Start menu rebuilt`，无崩溃。
- 重建后菜单截图 `/tmp/rebuilt-crop.png`：无「快捷方式」子菜单、「添加快捷方式(A)...」在顶层、
  中文标签、开始按钮 ladyball 徽标、28px 任务栏都正常。

### 4.2 失败现象
执行 `drag:80,737 → 80,691`（把「拖拽测试」拖到「实用工具」）后：
- `~/.local/share/start-menu/拖拽测试.af` **没有任何变化**（没写入 `Category=`）；
- `/tmp/serenity-session.log` 仍只有 2 条 `shell: Start menu rebuilt`（都是之前右键删除产生的），
  **没有新增** → 说明 `start_menu_dropped()` 没有走到 `apply_category() + schedule_rebuild()`；
- 截图 `/tmp/afterdrag-crop.png`：**菜单仍然开着**，「Linux 应用(L)」被高亮，
  右侧展开了 Linux 应用子菜单（Firefox ESR / OpenCode / UXTerm / XTerm / gedit…）。

### 4.3 诊断线索（高价值，按优先级）
1. **菜单没关 = dragging 分支根本没执行**。`MenuManager.cpp:257-277` 的
   `if (m_item_dragging) { ... async_menu_item_drag_dropped(...); close_everyone(); return true; }`
   一旦执行必然 `close_everyone()` 关掉所有菜单。截图菜单还开着 → **MouseUp 时
   `m_item_dragging == false`**，走进了 `:278` 的 `reset_item_drag()` + `return false`，
   事件被交给普通 hover/activation 路径。
2. **首要怀疑 `MenuManager.cpp:231-234`**：
   ```cpp
   if (!(mouse_event.buttons() & MouseButton::Primary)) {
       reset_item_drag();
       return false;
   }
   ```
   QMP `input-send-event` 把 `abs`（移动）和 `btn`（按下）作为**独立事件**注入，
   WS 合成出来的 MouseMove 可能不带 Primary 按钮位 → 第一次 move 就把 drag 状态清了。
   需要在 MouseMove 分支临时打 `dbgln("buttons={}", (unsigned)mouse_event.buttons())` 确认。
3. **次要怀疑：坐标注入有约 +23px 的 y 偏移**。请求终点 y=691（「实用工具」），
   实际高亮却在 y≈714（「Linux 应用(L)」）。一个吻合的假说：
   QEMU abs 按 **872**（=900−28 任务栏）或类似高度做 0..32767 映射，
   `691 × 900/872 ≈ 713`。
   → 下次先做**坐标校准**：`move:80,691` 后截图，看高亮行到底是哪一条，
   再决定用 `abs` 缩放补偿，或改用 memory 里记的 `poke:x,y` 相对增量 +
   `systemctl set-environment SERENITY_POINTER_FLAT_ACCEL=1`。
4. 还需排除：MouseDown 时 `hit_test(position)` 是否真的命中了「拖拽测试」
   （若起点落空，`m_item_drag_candidate` 不会置位）；以及 `SystemMenu.cpp:205`
   `if (!accepts || new_category == entry->category) return;` 的早退。

### 4.4 建议的排查顺序
1. 在 `handle_item_drag_or_context_menu()` 的 MouseDown / MouseMove / MouseUp 三处加临时
   `dbgln`（armed? buttons? dragging? target_menu_id?），只重编
   `cmake --build Build/lagom --target serenity-window-server`（新树），部署 VM，
   用单次 QMP 连接重放拖拽，读 `/tmp/serenity-session.log` 尾部。
2. 确认是 buttons 位丢失后，改成不依赖 `mouse_event.buttons()`：
   用已经记录的 `m_item_drag_candidate` + 一个显式 `m_item_drag_button_down` 布尔，
   由 MouseDown/MouseUp 自己维护，MouseMove 只看这个布尔。
3. 坐标校准后再重测两个场景：
   - 拖到「实用工具」→ `.af` 出现 `Category=&Utilities`，日志 `Start menu rebuilt`，
     重开菜单后条目出现在「实用工具」子菜单里（根菜单不再有它）；
   - 拖到菜单外空白处（如 `x=600,y=400`）→ 弹「删除快捷方式」确认框，点是才删。
4. 通过后把临时 dbgln 删掉。

### 4.5 复现工具（已写好，直接用）
- `/tmp/menuin.py`：**单个持久 QMP socket**（多次短连接会让 guest 指针漂移，这是之前踩过的坑），
  参数：`click:x,y` / `rclick:x,y` / `move:x,y` / `drag:x1,y1,x2,y2` / `shot:/tmp/x.ppm` /
  `key:<qcode>` / `sleep:<sec>`。例如：
  ```
  /usr/bin/python3 /tmp/menuin.py "click:25,886" "sleep:0.5" "shot:/tmp/open.ppm" "drag:80,737,80,691" "shot:/tmp/after.ppm"
  ```
- 截图：`/usr/bin/python3 /mnt/user/serenity/Build/vm/vncshot.py "shot:/tmp/x.ppm"`
  → `convert /tmp/x.ppm /tmp/x.png` → Read。**不要用 QEMU screendump**。
- 图像处理用 `/usr/bin/python3`（有 PIL 12.3.0），**venv 里没装 PIL**。
- 当前菜单坐标表（1600×900，开始按钮 `25,886`，菜单 x∈[8,167]）：
  `示例 505 / 开发 528 / 游戏 551 / 图形 575 / 网络 598 / 媒体 621 / 办公 645 /
  设置 668 / 实用工具 691 / Linux 应用(L) 714 / 拖拽测试 737 /
  添加快捷方式(A) 760 / 主题(T) 785 / 设置(S) 808 / 运行(R) 832 / 退出(X) 855`
  （带标签的截图：`/tmp/open-grid.png`、`/tmp/grid2.png`）
- zsh 坑：`SSH="-i ... -p 2222 ..."` 这种变量**不会分词**，整串会变成 `-i` 的参数；
  ssh 选项直接内联写，或用 `${=SSHARGS}`。

---

## 5. 下一个主任务：TTPlayer 复刻千千静听（用户 17:xx 明确要求）

### 5.1 目标
现有 `TTPlayer` 只是**单窗口** LibGUI 播放器（有播放列表 / LRC 歌词 / FFT 频谱），
**不是**用户要的千千静听。要求：
- UI 布局参照 `/home/ovo/下载/M4Player`（`Screenshots/m4_1.png` 是主参考图，
  另有 `m4_2.png` / `m4_3.png` / `OOPlayer.png`）；
- 千千静听经典形态：**主窗口 / 播放列表 / 歌词秀 / 10 段均衡器 四个可独立浮动窗口**、
  迷你模式、皮肤；
- **替代原生音频播放器 SoundPlayer** 成为默认。

### 5.2 现有代码位置（**只在新树**）
```
/mnt/user/serenity-linuxgui/Userland/Applications/TTPlayer/
  main.cpp  TTPlayerWidget.{h,cpp}  TransportBar  SpectrumWidget  LedDisplay
  LyricsView  MarqueeLabel  PlaylistStore  PlaylistTableModel  SkinText  CMakeLists.txt
/mnt/user/serenity-linuxgui/Ports/LinuxGUI/Apps/generated-apps.cmake:558-576
  → 目标名 serenity-t-t-player（注意不是 serenity-ttplayer），
    复用 ../SoundPlayer/{M3UParser,PlaybackManager}.cpp
/mnt/user/serenity-linuxgui/Base/res/apps/TTPlayer.af
/mnt/user/serenity-linuxgui/Base/res/locale/zh-CN.ini:1434  TTPlayer=TTPlayer 音乐播放器
```
VM 里已部署：`/res/apps/TTPlayer.af`、桌面 `/home/serenity/Desktop/_TTPlayer.af`。

### 5.3 替换 SoundPlayer 的落点
`Base/etc/LaunchServer.ini` 第 18–23 行：
```
wav=/bin/SoundPlayer   flac=/bin/SoundPlayer   m3u=/bin/SoundPlayer
m3u8=/bin/SoundPlayer  mp3=/bin/SoundPlayer   qoa=/bin/SoundPlayer
```
→ 全改 `/bin/TTPlayer`；再补 `ape/ogg/wma`（按实际解码能力决定，别盲目加）。
`Base/res/apps/SoundPlayer.af` 的 `[Launcher] FileTypes=` 也要同步处理
（删除 / 重定向 / 保留但降权），并给 `TTPlayer.af` 加对应 FileTypes+MimeTypes。
**注意**：SoundPlayer 的 `M3UParser.cpp` / `PlaybackManager.cpp` 被 TTPlayer 复用，
不要删源码目录。

### 5.4 M4Player 不能直接复用（已调研结论）
C++ / **wxWidgets 3.0** / FFmpeg **2.8.3** / 32 位 / PulseAudio，与 Serenity 的
LibGUI + AudioServer + 现代 64 位工具链不兼容；移植成本高于重写。
→ **方案：以 M4Player 为纯 UI/布局参照，在现有 LibGUI TTPlayer 上重画**。
参考文件：`M4Player/MainPanel.cpp`（主窗口）、`EqPanel.cpp`（10 段 EQ）、
`LyricPanel.cpp`（歌词秀）、`OOPList.cpp`（播放列表）、`Screenshots/m4_1.png`。
`README.md` 有原项目说明。

### 5.5 建议拆解
1. 先看 `m4_1.png` 与现有 `TTPlayerWidget.cpp` 的差异，定四个窗口的 Widget 划分；
2. 主窗口：LED 时间显示、marquee 曲名、传输条、频谱、音量、迷你模式切换；
3. 拆出独立 `GUI::Window` 的播放列表 / 歌词秀 / EQ，相互可显示隐藏、位置记忆（ConfigFile）；
4. EQ 接 `LibDSP` 的 10 段均衡（确认端口里 LibDSP 是否可用）；
5. 皮肤/配色走现有 `SkinText` + 复古配色，别引入图片皮肤包；
6. 替换 LaunchServer.ini / .af，VM 实测双击 mp3 打开的是 TTPlayer；
7. 新树 `cmake --build Build/lagom --target serenity-t-t-player`，部署验证。

---

## 6. 其它未完成（保持记录，别丢）

### 6.1 🐞 桌面「打开方式」导致会话重启（用户 17:20 报告，**先记录、晚修，本轮未动**）
- 只在**桌面** `DesktopIconsWidget` 上、对 **WinApps 程序管理生成的 `.desktop` 文件**
  点右键「打开方式」会重启整个桌面会话；文件管理器里未复现。
- 现场样本、首轮代码定位（`ExplorerMenus.h:137-140`、
  `DesktopIconsWidget.cpp:1117-1130` 的 `fill_open_with`、`:1373-1381` 的 `file_target`）、
  历史 VERIFICATION FAILED 堆栈、以及「晚修建议」都已完整写在
  **`.workbuddy/memory/2026-11-06.md` §「待修 BUG（17:20 用户报告）」**，直接看那段，
  本文不重复。核心怀疑：空子菜单 / 悬空 Action / `file_target()` 对非 `/` 开头的
  `Exec` 返回空 ByteString / `Entry::from_shortcut` 引用生命周期。

### 6.2 Task #16：GUI 模式 1600×900 重建 live ISO 并验证
- `qemu-iso.sh` / `qemu-direct.sh` 已改成 `-vga none -device virtio-vga,xres=1600,yres=900
  -display gtk -vnc :0`，但 **ISO 尚未重建**。
- 重建时务必遵守 `2026-11-06.md` 记的两个必修坑：
  ① `iso/` 是 root 所有 → 用 `sudo find -delete + chown` 清理，否则 xorriso 静默重封旧 squashfs；
  ② `mksquashfs` 必须 `sudo` 跑，否则 `/root`(0700) 整棵被跳过。
  判据：squashfs 1.1G / ISO 1.2G，且 `unsquashfs -l` 能看到快速启动项。
- 另外要**把本轮 20 个改动文件 + 新二进制 + zh-CN.ini 一起带进 ISO**，否则 ISO 里
  还是旧开始菜单（历史上已被"部署覆盖"坑过两次）。

### 6.3 任务清单现状
`#14/#17/#18/#19/#20/#21` 已完成；`#22` 仍是 in_progress（右键通过、拖拽未通过）；
`#16` 及 ISO 相关 `#3/#4/#5/#8/#9/#10/#11/#12/#13` 保持 pending（`#6/#7` 已完成）。
TTPlayer 与桌面崩溃修复已另建任务。

---

## 7. 通用坑速查（本轮再次踩到 / 需常记）

- `GUI::Menu` 加数据成员 = ABI 破坏（`C_OBJECT` 内联 `construct()`），用静态旁表。
- `GUI::ItemListModel` 存 `Container const&`，vector 必须活得比组件久。
- `Gfx::Rect::right()/bottom()` 是**排他**边界，逐像素拷贝用 `<`。
- `String::formatted` 返回 `ErrorOr<String>`；`Statusbar::set_text` 收 String，
  `ComboBox::set_text` 收 ByteString。
- 根 widget 必须 `set_fill_with_background_color(true)`，否则 Label 区域露黑底。
- `.ipc` 文件**不支持 `//` 注释**（IPCCompiler `assert_specific` 直接挂）。
- `ByteString` 没有 `.to_byte_string()`（它本身就是）。
- QMP 注入要用**单个持久 socket**；`pkill -f` 会杀到自己的 ssh，用 `pkill -x`。
- 替换运行中二进制报 `Text file busy` → stop 服务 → rm → cp → start（或 `.new` + `mv -f`）。
- VM 源码同步必须 `rsync -az --delete ./ user@host:dir/`（整目录带尾斜杠）。
- 改 LibGUI 单文件只需 `cmake --build Build/lagom --target liblagom-gui.so`。

---

## 8. 下一次开工建议顺序

1. 读本文档 + `.workbuddy/memory/2026-11-06.md` + `STATUS.md`；
2. **先收尾 §4 的拖拽**（加 dbgln → 定位 buttons 位 → 修 → 校准坐标 → 双场景验证 → 删 dbgln）；
3. 再进入 **§5 TTPlayer 复刻**（这是用户当前最关心的产出）；
4. 之后视时间处理 §6.1 崩溃、§6.2 ISO 重建。

> 现场 VM 未清理：`/tmp/menuin.py`、`/tmp/*.ppm|png`、`/tmp/serenity-session.log`、
> QMP socket 都还在，可直接继续。若需重启 VM：
> `cd /mnt/user/serenity/Build/vm && DISPLAY=:0 ./vm.sh`（GTK 窗口会被 Mutter 自动最大化，
> 需先取消最大化再 resize 到 1600×927 客户区，才能拿到 1600×900 drawable；
> 否则菜单布局会错位——本轮已踩过）。
