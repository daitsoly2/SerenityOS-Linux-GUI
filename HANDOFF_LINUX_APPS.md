# Linux 应用菜单 + 原生 Wayland 输入 —— 交接说明

> 日期：2026-11-02 凌晨。本文档是本轮工作的权威交接，供下次会话继续。
> 前置文档：`STATUS.md`（合成器总状态，旧 `STATUS_XWM_SEGV.md` 已合并到这里）、`QUICK_LAUNCH_HANDOFF.md`（任务栏）。

## 1. 一页速览

| 项 | 状态 |
|---|---|
| 原生 Wayland 客户端输入（键盘/鼠标） | ✅ 已修通（GTK4 编辑器实测可打字） |
| GTK4 应用装饰（复古边框） | ✅ 已生效（KDE server-decoration，default=SERVER） |
| 任务栏显示 Wayland 应用窗口 | ✅ 已生效（去掉 frameless 过滤） |
| 开始菜单 Linux Apps 子菜单 | ✅ 菜单出现、条目列出 |
| **Linux Apps 点击启动** | ❌ **报"未安装"（ENOENT）—— 下次先修这个** |
| xterm 真实输入回归测试 | ⚠️ 做到一半被打断（xdotool 旧测试不算数） |
| 滚轮手测 / 剪贴板桥接实测 | ❌ 未做 |

## 2. 本轮已完成的代码修改（都在仓库里，未提交）

### 合成器（Ports/LinuxGUI/WaylandHost/）
- `xwm.cpp`：BadWindow 刷屏修复（`set_wm_state_tolerating_death`：临时 error sink + XSync）；DestroyNotify 清理 redirect 列表
- `wayland_host.cpp`：
  - **输入按客户端路由**（本轮最大修复）：新增 `for_each_pointer_of_client / for_each_keyboard_of_client`，pointer/keyboard 事件发给焦点 surface 所属客户端，不再只发给"第一个 seat"（旧逻辑 = Xwayland，原生客户端完全收不到输入）
  - **wl_subcompositor + wl_subsurface**：GTK3/4 display open 的必需 global，缺了 GTK 直接不连（这正是"GTK 应用连不上/没输入"的根因链第一环）
  - **wl_data_device_manager v3**（剪贴板地基 + GTK 绑 seat 的前置条件）：source/offer/device + set_selection 广播 + receive→send_data fd 转发，参照 weston data-device
  - **KDE org_kde_kwin_server_decoration**（GTK4 用它协商装饰，GTK3 用 zxdg）：`protocols/server-decoration.xml` 新增 + CMake 扫描规则；default_mode=SERVER，request_mode(CLIENT) 才免装饰；`Surface::decoration_mode` 语义改为 0=未协商(默认装饰)/1=服务端/2=客户端
  - 装饰策略：原生 toplevel 默认上复古框；libadwaita 类明确请求 CLIENT 的才 frameless
  - X OR 弹窗 / xdg popup → `WindowType::Popup`（不进任务栏/WM）
- `CMakeLists.txt`：server-decoration 代码生成

### 任务栏（serenity-shell，注意真正的源码在 Ports/LinuxGUI/Shell/，不是 Userland/Services/Taskbar/main.cpp！）
- `Ports/LinuxGUI/Shell/SystemMenu.cpp`：开始菜单新增 **Linux Apps** 子菜单，扫描 `~/.local/share/applications`、`/usr/local/share/applications`、`/usr/share/applications`（先到先得），解析 Name/Exec/Icon，图标探测 hicolor/pixmaps，过滤 NoDisplay/Hidden/Terminal/非 Application
- `Ports/LinuxGUI/Shell/main.cpp`：`setenv("DISPLAY", ":99")`（子进程继承，.desktop 启动走 :99 桥）
- `Userland/Services/Taskbar/TaskbarWindow.cpp:419`：任务栏过滤去掉 `is_frameless`（Wayland CSD 窗口也要按钮）；弹窗已改为 Popup 类型不会混入

### 测试工具（Build/vm/）
- `inject.py`：分辨率改为环境变量 GUEST_W/H（默认 1337x846）
- `seattest.py / gtk4input.py / xtermtest.py / menucheck.py`：VNC 注入测试脚本
- `redeploy_host.sh / start_host.sh`：部署/启动脚本（systemd-run --user 执行）

## 3. 已验证（截图在 Build/vm/*.png）

- mousepad(GTK3) 走 Wayland 后端：**复古边框 + 秒开**（真实会话环境 260ms）
- gnome-text-editor(GTK4)：走 Wayland 后端建窗、复古边框、任务栏按钮、**真实硬件注入打字成功**（"abc" 上屏）
- GTK4 协议协商：绑定 org_kde_kwin_server_decoration_manager、收到 default_mode(2)、get_data_device 调用（WAYLAND_DEBUG 日志 /tmp/gte*.log）
- 开始菜单出现 "Linux Apps ▸"，条目列出（menu-final.png）

## 4. 待修：Linux Apps 启动报"未安装"（最高优先级）

**现象**：点 gedit → `错误: 未安装: gedit`；gnome-text-editor 同样。ENOENT 来自 `GUI::Process::spawn_or_show_error`（LibGUI/Process.cpp，spawn 失败 errno==ENOENT 时显示"未安装"）。

**已排除**：
- 二进制存在（/usr/bin/gedit ELF 正常；所有 .desktop 的 Exec 都能在 VM PATH 解析）
- `hostify_executable`（Ports/LinuxGUI/Common/hostify.h）只改写 /bin/、/usr/bin/ 前缀路径，相对名 "gedit" 原样通过（读过代码确认）
- 解析正确：Exec=gedit %U → program="gedit"，无参数

**下一步（按顺序）**：
1. `strace -f -p <shell pid> -e trace=execve` 挂住 serenity-shell，再注入点击 Linux Apps 条目，看 execve 实际尝试的路径和 envp（上次做到这步被打断）
2. 重点是 `Core::Process::spawn`（LibCore/Process.cpp）里 `Core::Environment::raw_environ()` 传给 posix_spawnp 的 **envp 是否包含正确 PATH**（shell 的 PATH=/serenity/bin:/serenity/bin:/usr/local/bin:/usr/bin:/bin）
3. 若 envp 正常，检查 posix_spawnp 对相对名的解析（可写 10 行 C 直接复现）
4. 顺手确认 `hostify_executable` 对 `/usr/bin/xxx` 形态 Exec 的破坏性改写问题（当前相对名没踩到，但绝对路径 Exec 的 .desktop 会踩：/usr/bin/xxx → serenity-xxx 不存在）。建议改成"仅当 serenity-<kebab> 确实存在时才改写"

## 5. 其他待办

- [ ] xterm 真实输入回归：VNC 注入点击 xterm → 打字 → 回显确认（上次脚本 xtermtest.py，点击坐标需按 xterm 实际位置调；X 窗口位置目前是级联默认而非 X 几何，P4 已知项）
- [ ] 滚轮手测（Firefox/xterm，pointer_axis 已按客户端路由）
- [ ] 剪贴板桥接实测：X 应用复制 → GTK 原生粘贴（data-device 已实现，未实测）
- [ ] GTK 应用走 Wayland 后端启动卡 25 秒：`org.freedesktop.portal` Settings 同步 dbus 调用超时（VM 无 xdg-desktop-portal）。可选项：装 portal / 启动器默认 GDK_BACKEND=x11 / 忍
- [ ] `/tmp/serenity-session.log` 会话重启后又涨到 11MB —— 确认 x11-bridge 新二进制（节流版）是否真在跑（`ls -la /serenity/bin/serenity-x11-bridge`，应为 165808 字节、11-02 00:20）
- [x] 本轮内容已并入 `STATUS.md` / `HANDOFF_WSWLR_COMPOSITOR.md`（旧 STATUS_XWM_SEGV.md 已删）
- [ ] x11-bridge 退役（P3）：用户已问"还需要吗"——答案：切产完成前保留（生产桌面 X 应用都靠它；开始菜单 .desktop 启动也默认 :99）

## 6. 部署状态与命令

```bash
# VM 内二进制均为最新（11-02）：/serenity/bin/{serenity-wayland-host,serenity-shell,serenity-x11-bridge}
# 生产会话 = serenity-desktop.service（已重启两轮）；wlhost 测试会话 = systemd-run --unit start-host /tmp/start_host.sh

ssh -i Build/vm/id_ed25519 -p 2222 serenity@127.0.0.1
cd /home/serenity/serenity && cmake --build Build/lagom --target serenity-wayland-host serenity-shell -j6
sudo rm -f /serenity/bin/serenity-shell && sudo cp Build/lagom/bin/serenity-shell /serenity/bin/   # rm+cp 防 Text file busy
sudo systemctl restart serenity-desktop.service   # 任务栏/菜单改动需要重启会话
```

**踩坑提醒**（本轮全部踩过）：
- ssh 命令行里含 "serenity-wayland-host" 字样会被 VM 脚本内 `pkill -f` 误杀自身 → 一律走 /tmp 脚本文件（/tmp 会被清，丢了从 Build/vm/ 重新传）
- `pkill -x` 进程名 >15 字符匹配不到（comm 截断）：用 `pkill -f "xxx[y]"` 技巧
- xdotool 是 X 层注入，**绕过 wl_pointer/wl_keyboard 转发**——输入链路测试必须用 QMP/VNC 硬件注入（vnc.py）
- 真正的 shell 源码在 Ports/LinuxGUI/Shell/（Taskbar/main.cpp 不参与构建）
- GTK 应用 Wayland 后端首次打开要 25s（portal 超时），测试时等够时间再截图

---

## 7. 2026-11-02 上午：切产完成，X11 桥接退役

### 结论：不再有 Xvfb + serenity-x11-bridge

`Ports/LinuxGUI/X11Bridge/` 已删除，`Meta/Lagom/CMakeLists.txt` 的 add_subdirectory 同步移除，
`serenity_manager.cpp` 只装配 `serenity-wayland-host`。旧栈的 bug 是结构性的（每窗每 tick 全帧拷贝、
天然无法显示原生 Wayland 客户端），在 host 里重写才是正解，不再保留双栈。

会话职责划分（当前实现）：

| 层 | 归属 |
|---|---|
| 窗口装饰（复古边框、标题栏、按钮、焦点） | WindowServer + wayland_host.cpp（协议层强制 SERVER） |
| 菜单 / 弹出层 | wayland_host.cpp（子表面 → 无边框 Popup 窗） |
| 控件渲染、布局、应用内容 | GTK/libadwaita 客户端 |

`serenitid` 为子进程导出 `WAYLAND_DISPLAY=wayland-serenity-0` 与 `SERENITY_APP_DISPLAY=:100`；
shell 把 `DISPLAY` 交给子进程，X11-only 程序落到 host 自带的 Xwayland :100（由 host 内 XWM 管理）。

### 本轮修掉的四个真 bug

1. **Linux Apps 点不动**：`ProcessSpawnOptions::search_for_executable_in_path` 默认 false，
   `.desktop` 的裸名 Exec 走 `posix_spawn`（不查 PATH）→ 必定 ENOENT。
   `LibCore/Process.cpp` 现在对无斜杠的可执行名改用 `posix_spawnp`；SystemMenu 另外用
   `PortHostify::resolve_executable` 先解析成绝对路径。
2. **CSD/SSD 不统一**：`org_kde_kwin_server_decoration.request_mode(CLIENT)` 原先会让窗口 frameless。
   现在一律回 SERVER，libadwaita 的阴影/自带边框被收回，headerbar 退化为窗口内容。
3. **菜单点不开（"鼠标没反应"）**：根因有两个，都已修
   - 客户端从未收到 `wl_keyboard.enter`（只在按键时才补发）→ GTK 认为窗口没焦点，拒绝开 popover。
     现在 `keyboard_focus_surface()` 在 pointer press 时也发。
   - `xdg_popup.configure` 尺寸为 0 → GTK 映射出 8×8；且 GTK 的 popover 实际走的是**子表面**
     （日志 `xdg=false`），被当成普通窗口加了框、进了任务栏。现在子表面一律 Popup + frameless，
     位置 = 父窗口 + set_position。
4. **liblagom 库没跟着更**：`/serenity/lib` 比构建树旧一天，LibCore 修复因此不生效。
   `Build/vm/build_*.sh` 现在一并 `cp -a lib/liblagom-*.so*` + `ldconfig`。

### 新增脚本（Build/vm/）

- `build_all.sh`：serenitid + shell 一起重建部署
- `build_wlhost.sh` / `build_bridge.sh`(已废) / `build_session.sh`
- `probe.py`：从点击到窗口出现的计时探针（probe.py 记基线，触发后逐帧比对）
- `diag_gtk.sh` / `launch_probe.sh`：单独复现 GTK 启动行为

### 仍待处理

- [ ] gedit 启动仍要 ~18s：`org.freedesktop.portal.Desktop` 激活失败，`Settings.ReadAll` 超时 25s。
      VM 里 portal 二进制齐全但 systemd user 单元起不来（Result: timeout）。
      可选：修 portal 单元 / 起一个最小 stub / 打 GTK patch 跳过 portal（见 `Ports/gtk-serenity/`）。
- [ ] 弹出层外圈还有一圈白色 margin（子表面窗口背景），需要按 geometry 裁一次。
- [ ] 桌面 1337 宽 vs VNC 1344：右侧 7px 黑边（probe_display_size 少算）。
- [ ] 滚轮 / 剪贴板仍只做过代码走查，没有实测。
- [ ] GTK patch 系列：目录与构建管线已就位（`Ports/gtk-serenity/`），patch 本体待写。

---

## 8. 2026-11-02 中午：portal 自建、GTK 后端起步

### portal：自建 `serenity-portal`（Ports/LinuxGUI/Portal/）

发行版的 `xdg-desktop-portal` 在本桌面起不来：systemd user 单元 `Type=dbus` 等不到名字，
手动跑也卡在 `g_initable_new` 里的同步 bus 调用（无 document portal / 不认的 session）。
GTK 因此每次启动等 `org.freedesktop.portal.Settings` 整整 **25 秒**。

自建了一个只实现 Settings 的 stub（sd-bus，约 200 行），serenitid 在 shell 之前拉起：
`Read` / `ReadOne` / `ReadAll` + `Desktop.GetVersion`，答 color-scheme=0、accent-color=我们主题的
#ab6e4a。**效果：gedit 从点击到出窗 18s → 1.0s。**

坑（已写进代码注释）：真 portal 的 `Read` 返回的是**双层 variant**——`g_variant_new_variant()`
再被 `g_variant_new("(v)")` 包一次；libadwaita 就是 `g_variant_get(ret,"(v)"); g_variant_get(child,"v")`
这么解包的。只包一层 → libadwaita 拿到 `u`、断言失败、**段错误**（`adw_settings_get_default`）。
另外 `ReadAll` 现在返回空字典：libadwaita 遍历 `a{sa{sv}}` 时会崩，空字典让它用内建默认值。

### 截图：不要再信 QEMU screendump

guest 的 KMS 模式是 1337 宽，QEMU 的 VNC 表面是 1344 宽，`screendump` 按 1344 读 1337 的行
→ 每行错 7px，整屏斜切。**这不是桌面的 bug**。正确做法：
`kill -USR1 $(pidof serenity-window-server)` → `/tmp/serenity-shot.ppm`（WindowServer 内存帧缓冲，
`Build/vm/guest_shot.sh` + `gshot.py` 一键取回）。这条已加进 `main_linux.cpp`。

### GTK 接入（Ports/gtk-serenity/）

- 补丁：`patches/gtk3/`（3.24.52 sid）、`patches/gtk4/`（4.18.6 trixie）、`patches/libadwaita/`（1.7.6）。
  内容：默认主题=Serenity、Wayland 恒为服务端装饰、libadwaita 调色板/方角/常亮。
  生成器 `Build/vm/gen_gtk_patches.py` 直接改真源码再 diff，保证上下文与版本一致。
- 引擎：`engine/serenity-theming-engine.c`（GtkThemingEngine 子类）已能编译成 17KB 模块并装进
  `.../theming-engines/`。**但实测 GTK 3.24 基本不经过引擎**（3.20 起控件走 CSS），
  外观变化很小——这条不要继续投入，除非整widget重绘。
- sid 已加入（`Pin-Priority: 100`，只当源码池用）。

### 待办

- [ ] GTK4 4.24.1 源码包 dpkg-source 解包失败，补丁目前对 4.18.6；解包正常后用生成器重跑。
- [ ] libadwaita 补丁还没实际编译安装验证（需要重建 libadwaita）。
- [ ] 弹出层外圈白边、滚轮/剪贴板实测。

---

## 9. Win2000 主题 + libadwaita 实装

### 主题改成 Windows 2000 风格

`serenity-theme-gen.py` 里的 GTK3/GTK4/libadwaita 三套模板已整体重写为 Win2000 外观：

| 元素 | 取值 |
|---|---|
| 面板/按钮 | `#d4d0c8`，高光 `#ffffff`/`#dfdfdf`，阴影 `#808080`/`#404040` |
| 标题栏 | `#0a246a → #a6caf0` 横向渐变，白色粗体；失活为灰渐变 |
| 选中/菜单高亮 | `#000080` + 白字 |
| 输入框 | 白底 + 内凹（左/上 `#808080`，内阴影 `#404040`） |
| 菜单/弹出层 | 面板色 + 1px `#808080` 边框 + 硬投影 2px |
| 按钮按下 | 边框翻转 + 文本右移 1px、下移 1px |
| 滚动条 | 16px 宽，3D 滑块 + 箭头按钮 |
| 字体 | `MS Sans Serif` → `Tahoma` → `DejaVu Sans` |

装法：`sudo python3 scripts/port/serenity-theme-gen.py --out /usr --theme Default`，
libadwaita 覆盖另拷到 `~/.config/gtk-4.0/gtk.css`（libadwaita 只认用户 CSS 的最后一段）。

### libadwaita 实装

- 三个补丁已按 Win2000 调色板重新生成并通过 `patch --dry-run`。
- 源码编译：`Build/vm/build_libadw.sh`（meson + sassc；需要 `libgtk-4-dev`、`libappstream-dev`，
  用 `apt-get install -t trixie` 装，别用 build-dep——sid 的 pin 会让它解不出来）。
- 产物覆盖 `/usr/lib/x86_64-linux-gnu/libadwaita-1.so.0`，原库备份在
  `/root/libadwaita-1.so.0.dist`（要回滚：`sudo cp` 回去 + `ldconfig`）。

### 观察到的坑

- `pkill -f "<程序名>"` 会匹配到自己那条 ssh 命令行，把会话杀掉（exit 255）。脚本里一律
  避免在同一命令行出现被搜索的字面量，或改用 `pidof` / `pkill -x`。
- libadwaita 的 meson 会去拉 gtk/appstream 子项目；用 `--wrap-mode=nofallback` + 装 `-dev` 包更省事。
- `meson` 产物里 `libadwaita-1.so.0` 本身就是最终文件（不是符号链接链），直接覆盖即可。

---

## 10. labwc 路线评估（Documentation/labwc-route.md）

- labwc 0.8.3 已在 VM 里验证过（headless + grim）：GTK3/GTK4 都能正常出窗。
- **"GTK4 卡死"的真因是 GtkApplication 单实例**：主实例存在时新进程只 Activate 旧窗口然后退出；
  合成器被杀后旧进程残留、窗口消失 → 看起来就是卡死。用独立 `dbus-run-session` +
  独立 `XDG_RUNTIME_DIR` 才能做干净对照（`Build/vm/labwc_probe2.sh`）。
- 结论：短期不迁 labwc（需要 WindowServer 改成每窗口一个 surface，数周级）；
  先按清单收集"真正不兼容"的应用行为，再决定是否走该路线。
- 事故记录：为了跑对照测试停掉了生产会话的 wayland-host，导致在跑的编辑器失去合成器。
  以后测试一律用嵌套会话，不动生产会话。

---

## 11. 2026-11-02 下午：输入法（LibGUI 客户端）+ Wine 中文字体

### 11.1 输入法：补上缺失的客户端

引擎侧（`Ports/LinuxGUI/IMEFrontend/serenityime.cpp`，fcitx5 addon）早就写好了，
但 Serenity 这边没有任何东西去连它的 socket —— 输入法实际是"半截"的。本轮补的是
客户端：

- `Userland/Libraries/LibGUI/InputMethod.{h,cpp}`：POSIX socket 客户端。
  `key` 的回复可能是 `commit`/`panel` 若干行再加 `handled`，所以
  `read_until_handled()` 一直读到 `handled` 为止（本地 socket 也常只读到 panel 那行）。
- `Userland/Libraries/LibGUI/InputMethodCandidateWindow.{h,cpp}`：
  `WindowType::Autocomplete` 无边框弹窗，第一行预编辑、第二行候选，配色走 `palette()`。
- `TextEditor`：`keydown_event` 开头交给 IME；`focusin/focusout` 连接/断开；
  `cursor_did_change` 重发光标矩形；新增 `cursor_screen_rect()`（内容坐标 → 屏幕坐标）。
- `serenitid`：Wayland host 之后、shell 之前拉起 `fcitx5`（`/usr/bin/fcitx5`，
  不自动重启，缺失时被 X_OK 过滤掉），并导出 `GTK_IM_MODULE`/`QT_IM_MODULE`/`XMODIFIERS`。
- `InputMethod`：新增周边文本删除（`delete`）与上传（`surround`）。fcitx5 的
  `deleteSurroundingText()` 在周边文本为空时会被直接吞掉，所以客户端在 `focus_in`
  之后、每次按键往返后都把整段文本 + 字符偏移光标/锚点推给引擎；引擎回
  `delete <offset> <size>` 时按 UCS4 字符换算成 `TextRange`，走 `delete_text_range()`。

已验证：`LibGUI` 与 `serenitid` 编译链接通过；用 `scripts/port/ime-fake-engine.py`
（同协议假引擎）跑通 `nihao` → 预编辑 → 回车提交"你好"，且验证了"一次 read 可能
只有 panel 行"这一点（所以客户端必须循环读）。

构建/安装：`sudo scripts/port/build-ime.sh`（需要 `libfcitx5core-dev`）。
协议与限制见 `Ports/LinuxGUI/IMEFrontend/README.md`。

### 11.2 Wine 中文字体

`scripts/port/wine-fonts.py`：把 Sarasa Gothic SC 拷进前缀 `drive_c/windows/Fonts/`，
注册 `...\CurrentVersion\Fonts`，用 `FontSubstitutes` 把 19 个中文字体名
（`SimSun`/`宋体`/`Microsoft YaHei`/`微软雅黑`…）指过去，并加 `FontLink\SystemLink`
让 Tahoma/Arial 等拉丁字体在遇到 CJK 时回退。用 `wine reg import` 导入
（失败回退 `regedit /S`）。

- `05-install-prefix.sh` 装成 `<prefix>/bin/serenity-wine-fonts`；
- `winapp-launcher.sh` 首次启动时 grep 一次 `system.reg`，缺了就自动修。

离线已验证：字体完整拷贝（24 MB）、`.reg` 为 UTF-16 且中文键正确、
`hex(7)` 的 multi-sz 能还原成 `C:\windows\Fonts\SarasaGothicSC-Regular.ttf,Sarasa Gothic SC`。
**未在真前缀上跑 `wine reg import`**：本沙箱起不了 Wine（无显示 + 会杀后台 Xvfb），
到 VM 上先跑一次 `serenity-wine-fonts` 确认。

### 11.3 待办 / 风险

- [ ] VM 上实跑 `serenity-wine-fonts`，用一个中文 exe 看是否还出方框。
- [ ] VM 上装 `fcitx5` + `fcitx5-chinese-addons`，确认 `serenityime` addon 被加载
      （`fcitx5 -d` 后看 `/tmp/serenity-ime.sock` 是否出现）。
- [ ] `TextEditor` 只转发 key down；需要 key up 的引擎再补 `keyup_event`。
- [x] `delete`（周边文本删除）+ `surround` 上传已实现（客户端 + addon）；本沙箱
      无 `libfcitx5core-dev`，addon 侧未编译，到 VM 上核对 API 版本。
- [ ] `forward` 仍是空实现：引擎"消费了键又想转给应用"的情形不会重放。
- [ ] 周边文本每次整篇重传，未做光标附近窗口裁剪。
- [ ] 每次按键一次同步往返（poll 上限 100ms）；如果引擎卡住会拖慢输入。

---

## 12. 视频播放器（libmpv 后端）

`Ports/LinuxGUI/VideoPlayer/` 是移植版播放器（替换上游 LibMedia 版本）。上游只能解
Matroska 里的 VP8/VP9，本移植用 `dlopen("libmpv.so.2")` + `MPV_RENDER_API_TYPE_SW`
把帧渲染到内存 BGRx 表面，再包成 `Gfx::Bitmap` 交给 `VideoFrameWidget` 画，
不需要 GL/Vulkan（VM 只有 llvmpipe / kms_swrast）。构建目标 `serenity-video-player`，
安装到 `/serenity/bin/serenity-video-player`，`VideoPlayer.af` 已登记常见视频扩展名/MIME。

### 12.1 2026-11-06 修复：「有声音、没图像」

**根因（不是 mpv，是 LibGUI 的挂载方式）**：`main.cpp` 里所有控件都用
`main_layout->add_widget(*w)` 挂上去。`Layout::add_widget()` **只把控件登记进布局**
（于是 `BoxLayout::run()` 会算出并写进 `relative_rect`），但**不会调用 `add_child()`**。
而绘制/事件分发只走控件树（`Window` → 主控件 → `for_each_child_widget`），
所以视频控件尺寸正确（`rel=0,0 820x544`）、`is_visible()==true`，
但 `paint_event()` **一次都没被调用**；整块客户区只剩主控件的灰底。
`VideoFrameWidget::paint_event` 里加计数器验证：0 次调用。

**正确写法**：挂到父控件（`Widget::child_event` 在 `ChildAdded` 时自动
`layout()->add_widget()`）：

```cpp
main_widget->add_child(*video_display);   // ✅ 进控件树 + 自动登记布局
// main_layout->add_widget(*video_display); // ❌ 只有几何，不绘制、不派发事件
```

或直接用 `parent.add<T>(...)`（`Core::EventReceiver::add` = construct + add_child）。
同批把 `bottom_container` / `seek_slider` / `controls` / 各按钮 & 标签全部改掉：
修复前底部的进度条、时间、音量、按钮同样不显示，点击也不响应。

### 12.2 顺带修掉的两个 mpv 用法错误

- `set_paused()` / `set_volume()` 之前用 `mpv_set_option_string()`。**option 只在
  `mpv_initialize()` 之前有效**，之后调只返回错误、静默无效（＝暂停键和音量条没反应）。
  改为 `mpv_set_property("pause", MPV_FORMAT_FLAG)` / `("volume", MPV_FORMAT_DOUBLE)`，
  并在 `MpvApi` 里补 `dlsym("mpv_set_property")`。VM 实测：点视频区能暂停/继续。
- 「文件 → 打开」原本会崩（`VERIFICATION FAILED: !is_error() at Error.h:252`，
  栈顶 `FileSystemAccessClient::Client::the()`）——见 §12.4，**已由部署
  FileSystemAccessServer 根治**，现在弹的是真正的 Serenity 文件选择器。
  `choose_file()` 仍保留一层探测：`/tmp/session/<sid>/portal/filesystemaccess`
  不存在（服务崩了/没起）就弹提示而不是 abort。

### 12.3 踩坑：不要把 SW 表面缩到控件大小

试过让 mpv 直接渲到控件尺寸（1280x720 → 816x458）以省掉 LibGUI 每帧的双线性重采样，
结果画面全黑（表面里确实有像素，`pixsum` 非 0，但画出来是黑的；同一时刻改成原生
尺寸就正常）。结论：**`render_frame()` 的 SW 尺寸保持视频原生尺寸**（`video_size()`
里已按 1280x720 封顶并按比例缩放），缩放在 LibGUI 的 Fit 分支做。已在代码里注释说明。

### 12.4 全端口「打开文件」都不可用的真因：FSAS 服务没部署

不是 API 没了，也不是上游改了接口：`FileSystemAccessServer` 的源码和端口入口
（`Ports/LinuxGUI/Services/main_file_system_access_server_linux.cpp`）一直在，
`serenity_manager` 也早就写了 `spawn(*filesystem_access_server)`，但
**`Build/lagom/bin/serenity-file-system-access-server` 从来没构建过**，
`/serenity/bin` 里也没有这个二进制 —— 于是 spawn 静默失败、portal socket
`/tmp/session/<sid>/portal/filesystemaccess` 永远不存在，
`LibFileSystemAccessClient::Client::the()` 连不上就在 `Error::release_value()`
上 `VERIFY` abort。表现就是：播放器/Notepad/TextEditor/PixelPaint/ImageViewer
等所有用 `FilePicker` 的应用「文件 → 打开」要么没反应、要么把应用带崩。

修法（3 步，缺一不可）：

1. 构建：`cmake --build Build/lagom --target serenity-file-system-access-server`
   （端口 `Ports/LinuxGUI/Services/CMakeLists.txt` 里目标已定义，输出照常落在
   `Build/lagom/bin/`；`05-install-prefix.sh` 用 `serenity-*` 通配安装，无需改脚本）。
2. 部署：`sudo cp Build/lagom/bin/serenity-file-system-access-server /serenity/bin/` + `chmod 755`。
3. 启动：已经登录的会话里手动起一次（`/serenity/bin/serenity-file-system-access-server &`）；
   下次登录由 `serenity_manager` 自动拉起（它在 window-server 之前 spawn，没问题：
   只有真正弹选择器时才会连 WindowServer）。

上游应用确实带着这套 API：`nm -D --undefined-only /serenity/bin/serenity-image-viewer`
里 3 个 `FileSystemAccessClient` 符号、`serenity-pixel-paint` 里 5 个 —— 说明
二进制链接了客户端库、Open/Save 走的就是 FSAS，缺的只是服务进程。

**第二个坑：客户端一断开服务就自杀。** 上游 `ConnectionFromClient::die()` 里是
`GUI::Application::the()->quit()`（SystemServer 会在下次请求时重新拉起），但端口这个
进程没有 `GUI::Application`（宿主入口只建了 `Core::EventLoop`）→ 客户端断开时
要么空指针崩、要么连进程一起退掉，留下**陈旧的 portal socket**，下一个应用
`connect()` 直接 ECONNREFUSED → `Client::the()` 再 `VERIFY`。表现就是"第一次能用、
之后又不行"。修法：给 `die()` 加 `#ifdef __serenity__` 分支，宿主上只
`dbgln("client N disconnected, still listening")` 并继续 serve
（已在 `LINUX_GUI_PORT.md` §7 的改动表登记）。裸 socket 连断 3 轮实测：进程 pid 不变、
日志 3 行 "still listening"。

**第三个坑（2026-11-06 12:57，"重启后失效"的真因）：会话管理器本身是旧二进制。**
上面两步都做完、当前会话手起服务也能用，但**一重启就没了**：`serenity_manager`
里的 `spawn(*filesystem_access_server)` 这段代码确实在源码里，可是 VM 上
`/serenity/libexec/serenitid.real` 是 **09:57 的旧构建（163552 B，`strings` 里没有
`file-system-access-server`）**，而有这条 spawn 的 `Build/lagom/bin/serenitid`
是 11-05 17:34 的（168200 B）。也就是说会话启动时那行根本没被执行，日志里连
`started file-system-access-server` 都没有。

- 部署注意：会话管理器的安装路径是 **`/serenity/libexec/serenitid.real`**（不是 `/serenity/bin`），
  `05-install-prefix.sh` 里是 `install_atomic ${BUILD}/bin/serenitid ${PREFIX}/libexec/serenitid.real`；
  只 `cp` `/serenity/bin/*` 会漏掉它。
- 正在运行时替换会 `Text file busy`：`systemctl stop serenity-desktop` → `rm` → `cp` → `chmod 755` → `start`。
- 替换后重启会话，日志出现 `started file-system-access-server (pid 1089)`、
  `/tmp/session/0/portal/filesystemaccess` 由启动流程自己创建 —— 这才是重启后也生效的状态。

**验证**：VM 里点播放器「文件 → 打开」，弹出的是完整 Serenity 文件选择器
（左侧 Root/Home/Desktop/…/Videos 导航 + Filename + `Files of Type: Video Files (*.avf; …)`），
Esc 可取消且播放器不崩。

### 12.5 调试手段

`SERENITY_VP_DEBUG=1` 打开每秒一次的 `VPDIAG`（控件/内区尺寸、`render_ok`、
`render_null`、位图尺寸、8 个采样点像素和、`vsize`），并把第一帧 dump 到
`/tmp/video-player-first-frame.ppm`。默认关闭（否则刷日志）。

VM 内截图不用 QEMU screendump（会斜切），用内置服务：

```bash
echo screenshot | socat - UNIX-CONNECT:/tmp/serenity-recorder.sock   # 返回 PNG 路径
```
