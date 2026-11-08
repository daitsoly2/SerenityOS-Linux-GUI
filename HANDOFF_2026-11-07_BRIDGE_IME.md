# 桥内建输入法：一套 Serenity UI，多种前端协议

> 日期：2026-11-07 晚
> 状态：**设计定稿 + 第一阶段（Wayland text-input-v3 ↔ fcitx5）已打通并实测**
> 前序文档：`HANDOFF_2026-11-07_WLROOTS_IME.md`（崩溃修复 + 根因分析）

---

## 0. 决策（本轮用户拍板）

> 「输入法 UI 始终是 Serenity 版本在工作，Linux 软件全靠桥把输入的文字发过去；
> 如果可以，桥里放一个专门的输入法，把各种协议的实现都放进去，甚至直接集成
> fcitx5 的部分代码。」

一句话：**IME 由桥统一提供**，对下适配各客户端协议，对上只做「引擎 + Serenity 候选窗」。

```
         ┌────────────── serenity-wlroots-host（单进程） ───────────────┐
         │                                                             │
 Wayland │  text_input_v3 前端 ─┐                                      │
 客户端  │                      ├─→ ImeService ─→ 引擎适配 ─→ fcitx5    │
 X11     │  XIM 前端（待建）  ───┘        │                            │
 客户端  │                                └─→ Serenity 候选窗(LibGUI)   │
         └─────────────────────────────────────────────────────────────┘
```

候选窗只有一个实现（`LibGUI::InputMethodPanelWidget`，跟随主题），位置由**协议里
客户端自己报的光标矩形**算出来 —— 这正是当前 WordCraft 候选窗「定位不准」的病根
所在（见 §2）。

---

## 1. 为什么要「桥内建」而不是让 fcitx5 自己画

| 做法 | 问题 |
|---|---|
| fcitx5 的 Wayland 前端（`input-method-v2`） | 候选窗是 fcitx5 的 popup surface，外观/主题跟 Serenity 无关；且 `grab_keyboard` 独占，Ctrl+C 一类快捷键要靠 fcitx5 回灌 |
| fcitx5 的 X11 前端（XIM + classicui） | 候选窗是 fcitx5 的 override-redirect X 窗口，位置是 fcitx 自己算的（XIM spot），**在桥这套「WS 摆窗口、X root 只是假坐标」的架构里必然偏** |
| 桥内建（本方案） | UI 一处实现；位置由我们算；不依赖 fcitx 的 UI 插件；X11/Wayland 共用一套引擎逻辑 |

---

## 2. 实测证据（本轮）

### 2.1 Wayland 通路已经打通

`zenity --entry`（GTK4，`GTK_IM_MODULE` 未设 ⇒ 走 GTK 原生 Wayland IM）：

```
客户端 WAYLAND_DEBUG：
  -> zwp_text_input_manager_v3#24.get_text_input(new id zwp_text_input_v3#25, wl_seat#10)
  -> zwp_text_input_v3#25.enable()          # 点进输入框之后
  -> zwp_text_input_v3#25.set_surrounding_text("", 0, 0)
  -> zwp_text_input_v3#25.set_cursor_rectangle(58, 129, 0, 34)
  -> zwp_text_input_v3#25.commit()

桥 /tmp/serenity-session.log：
  ws-proto: new text_input 0x…3d040 client=0x…08b20
  ws-proto: text_input commit enabled=1 focused=0x…422b0 kb_focus=0x…422b0 im=0x…74880
  ws-proto: input-method grabbed keyboard          ← fcitx5 已接手按键
```

⇒ `text_input_v3` + `input-method_v2` + grab + `set_keyboard` 全部正常，
**缺的只是「把 fcitx 的候选窗换成 Serenity 的」**。

### 2.2 WordCraft 根本不是 Wayland 程序

- `~/.config/wordcraft/ui.json` 里 `"displayServer": "auto"`，winit 的 auto
  **优先 X11**（`DISPLAY` 存在时），日志一直是 `display: X11`。
- 改成 `"displayServer": "wayland"` 后立刻变成 `display: native Wayland` 并正常出窗口
  ⇒ 想走 Wayland 通路，这个开关必须先改（已验证，改回 `auto` 即可还原）。
- **但**：egui/winit 只有在 App 调过 `set_ime_allowed(true)` 才会 `enable()` text_input。
  WordCraft 没调 ⇒ 桥收不到 `enable`，拼音永远组不出来（ASCII 仍靠 raw key 直接进）。
  ⇒ **对 WordCraft 真正管用的是 XIM**，不是 text-input。这也解释了「GTK 应用没问题、
  WordCraft 不行」——GTK 会 enable，egui 不会。

### 2.3 候选窗「定位不准」的直接原因

X11 侧 override-redirect 窗口在 `main.cpp::pair_x11_surface()` 里：

```cpp
if (popup && info.transient_for) { /* 用父窗口做锚点换算 */ }
// 否则 x = y = -1
```

fcitx5 候选窗是 override-redirect 且**没有 WM_TRANSIENT_FOR** ⇒ 落到
`ensure_hosted_window()` 的默认级联位（`80 + slot*24`）⇒ 表现为「在窗口左上角附近乱飘」。
即使有 transient_for，锚点用的是 `window->x()`（含标题栏/边框），不是内容区原点，
也会偏一个框宽。两者都要修（§4 P0）。

### 2.4 顺带发现

- **fcitx5 会因为 X11 连接断开而整体退出**（`XCB connection ":100" got error: 1`
  → `Disconnected from X11 Display :100` → 卸载全部 addon 退出）。桥一重启 Xwayland，
  fcitx5 就跟着死，输入法全灭。桥内建 IME 后应让 fcitx5 只做引擎（关掉 xcb/classicui/xim）。
- `serenityime` addon 的 socket **只接受一个客户端**（`accept_client()` 里先 `drop_client()`）。
  桥要当客户端就必须先把它改成多客户端，否则原生应用和 Linux 客户端互相踢。

---

## 3. 引擎接法：链路 vs 直接集成

| 方案 | 优点 | 代价 |
|---|---|---|
| **A. 复用 `serenityime` addon 的 socket 行协议**（推荐） | 已有实现、已被原生应用验证；协议简单（`create/cap/focus/cursor/surround/key/select` ↔ `commit/delete/forward/panel/active`）；桥不需要链 fcitx5 头文件 | 需要把 addon 改成多客户端；候选窗数据是 `panel` 行，够用 |
| B. 桥直接链 `libFcitx5Core`，内建 InputContext | 少一跳 socket，能拿到 `InputPanel` 强类型 | 桥从此绑死 fcitx5 ABI；桥是多客户端（一个进程服务 N 个窗口）⇒ 要管 N 个 InputContext；许可/依赖面变大 |

建议 A：**socket 这一层已经是「协议」，等价于自己定义的一套 IM 接口**，
换成别的引擎（ibus、自研）只改 addon。真要做 B，也是把 addon 的
`SerenityInputContext` 搬进桥，协议层不变。

---

## 4. 分阶段实现

### P0（先修痛点，小改动，不动架构）
1. `pair_x11_surface()`：override-redirect 且无 `transient_for` 时，用**当前激活的
   X11 窗口**当锚点（内容区原点 `widget->screen_relative_rect()`）；找不到锚点才用
   root 绝对坐标。有 `transient_for` 时也改用内容区原点（少一个标题栏的偏差）。
   ⇒ fcitx 的 X11 候选窗立刻贴到光标附近。
2. `serenityime` addon 改多客户端：`fd/buffer/context/read_event` 收进
   `struct Client`，`send()` 带 client 参数，`SerenityInputContext` 持 `Client*`。

### P1（Wayland 客户端用 Serenity 候选窗）
3. 桥内新增 `ime_relay.{h,cpp}`：socket 客户端 + `panel` 解析。
4. 候选窗：复用 `LibGUI::InputMethodPanelWidget`，作为宿主窗口 `SurfaceWidget`
   的子控件（与原生 `InputMethodCandidateWindow` 同款，天然不吃焦点）。
5. 光标矩形：`text_input_v3.set_cursor_rectangle`（surface 坐标）+ 窗口内容区原点
   ⇒ 屏幕坐标 ⇒ 发给 addon 的 `cursor` 行。**位置从此由协议决定，不再靠 XIM spot 猜。**
6. 关掉 `input-method-v2` 通路（`SERENITY_IM_V2=1` 保留为实验开关），
   避免 fcitx 自己弹候选窗。

### P2（X11 客户端 —— WordCraft 的真正解药）
7. 桥里实现 **XIM server**（`@im=serenity`）：
   - `XOpenIM/ XCreateIC / XIM_SET_IC_VALUES / XIM_FORWARD_EVENT` 这些回调
   - 按键：XIM `ForwardEvent` → 桥 → 引擎 `key` → 未消费则用
     `XSendEvent` 回投给客户端（等价 addon 的 `forward` 行）
   - 上屏：`XIM_COMMIT`（`XimCommit`/`XimPreedit` 回调）
   - 候选窗：同一套 Serenity `InputMethodPanelWidget`
8. 让 fcitx5 别再抢 XIM：关掉它的 `xim`/`classicui`/`xcb` 插件，
   只留 `rime` 等引擎 + `serenityime` addon。
9. 客户端侧：`XMODIFIERS=@im=serenity`（session manager 里统一设）。

> 第 7 步是这一轮之后的主要工作量：XIM 协议本身不小（IC/焦点/几何/预编辑/前向），
> 但它一次性解决**所有 X11 程序**（WordCraft、Wine、老 Qt/GTK2、Java），
> 且 UI 与 Wayland 侧完全共用。

---

## 5. 本轮已落地的代码（未 commit）

`Ports/LinuxGUI/WlrootsHost/wlroots_proto.c`：
- `text_input_v3` 前端：`on_new_text_input` / enable / commit / disable / destroy、
  焦点跟随（`keyboard_enter/leave`）、`text_input_take_focus/drop_focus`。
- `input_method_v2` 前端：`on_new_input_method` / commit 回灌 /
  `grab_keyboard` → `set_keyboard` / `send_unavailable` 顶替旧 IM。
- `zwp_virtual_keyboard_v1`：接收 fcitx 回灌的「不消费按键」，直连 seat
  （**不能**再走 grab 路由，否则死循环）。
- 键路由：grab 生效时键只发 grab，modifiers 双发。
- `wlroots_proto_destroy()` 里先摘除三个 manager 的 listener
  （否则 `wl_display_destroy` 会命中 wlroots 的 `assert(wl_list_empty(...))`）。
- 诊断日志：`new text_input` / `text_input commit enabled=…` / `kb_enter` /
  `input-method grabbed keyboard`。

---

## 6. 复现/验证命令（本轮用过的）

```bash
# 编译 + 部署（会带走 Xwayland，X11 客户端与 fcitx5 一起结束，需重启）
ninja -C Build/lagom serenity-wlroots-host
sshpass -p serenity rsync -az -e "ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  Build/lagom/bin/serenity-wlroots-host serenity@127.0.0.1:/tmp/serenity-wlroots-host.new
# VM 内：sudo -n mv -f /tmp/… /serenity/bin/ && 按 /proc/*/exe case 通配杀旧进程

# WordCraft 切 Wayland（ui.json）
sed -i 's/"displayServer": "auto"/"displayServer": "wayland"/' ~/.config/wordcraft/ui.json

# 观察协议（客户端侧）
WAYLAND_DEBUG=1 zenity --entry      # 看 zwp_text_input_v3.enable/commit

# 观察桥侧
grep -E "text_input|input-method|grabbed" /tmp/serenity-session.log
```

---

## 7. fcitx5 也要裁剪（本轮用户要求）

现状：VM 装的是 Debian 全量 fcitx5（xcb / classicui / xim / waylandim / dbus /
notification / kimpanel / ibus / fcitx4 … 全在）。这些插件对「桥内建 IME」不仅没用，
还在跟我们抢：classicui 抢着画候选窗、xim 抢 XIM 服务名、xcb 让 fcitx 因为 Xwayland
重启而整体退出。

### 目标形态（和 vendored wlroots 同款做法）

```
/serenity/opt/fcitx5/  （prefix，独立于发行版）
  lib/fcitx5/            core + rime(+lua/table 等 rime 依赖) + serenityime.so
  share/fcitx5/          只留这些 addon 的 conf + rime 数据
  bin/                   不需要（由 serenitid 拉起，或桥按需 spawn）
保留：frontend 只有 serenityime（socket），UI 一个都不要（桥自己画）
砍掉：xcb / xim / waylandim / classicui / dbus / notification / kimpanel /
      ibus / fcitx4frontend / virtualkeyboard / emoji / clipboard / spell …
```

### 做法

1. 源码放进仓库（`fcitx5/` 与 `fcitx5-chinese-addons/`，各带 LICENSE，和 `wlroots/` 同级），
   `meson setup --prefix=$PWD/fcitx5/prefix -Denable-x11=disabled -Denable-wayland=disabled
   -Denable-dbus=disabled -Denable-enchant=disabled -Denable-spell=disabled
   -Denable-table=disabled -Denable-emoji=disabled -Denable-clipboard=disabled
   -Denable-notifications=disabled -Denable-kimpanel=disabled`；
   rime 由 `fcitx5-chinese-addons` 提供，只装 rime + pinyin 数据。
2. 装到 prefix 后按白名单删插件目录（比逐个 meson option 更可靠，版本相关）。
3. `serenityime.so` 用 `scripts/port/build-ime.sh` 对着这个 prefix 的
   `libFcitx5Core` 编（ABI 必须自洽，不能混发行版的）。
4. session manager 里把 fcitx5 的环境收敛成「只有 XDG_RUNTIME_DIR + 数据目录」，
   不再需要 DISPLAY/WAYLAND_DISPLAY —— 引擎不需要任何显示服务，
   **Xwayland 重启也不会再带走它**。

### 为什么不把 fcitx5 编进桥

- fcitx5 是个完整框架（Instance/AddonManager/配置系统/事件循环），嵌进来等于
  在合成器进程里跑一个引擎；**引擎崩了会把整个桌面带走**（今天 fcitx 挂掉只是输入法没了）。
- rime/librime 的 ABI 与 fcitx5 版本强绑定，桥以后要换引擎（ibus/自研）就得重编。
- 现在这层 socket 行协议只有 ~20 行命令，已经是干净的边界；裁剪掉的只是**进程的体重**，
  不是协议。

---

## 8. XIM 前端（下一步的代码清单）

X11 客户端（WordCraft、Wine、老 Qt/GTK2）不认 Wayland 协议，只能走 XIM。
桥已经握着 Xwayland 的连接（`X11Compat/xwm.cpp`），实现 XIM 服务端是一个独立模块。

### 8.1 握手与注册

1. `XSetSelectionOwner(dpy, XInternAtom("XIM_SERVERS"), win, CurrentTime)` —
   服务名 `@server=serenity`（客户端用 `XMODIFIERS=@im=serenity`）。
2. `XIM_SERVERS` 上的 `SelectionRequest`：回 `XIM_SERVERS` atom 列表（含自己的 atom）。
3. 客户端用 `XSendEvent` 发 `XIM_CONNECT` / `XIM_CONNECT_REPLY`（双向），
   随后 `XIM_QUERY_EXTENSION`（要支持 `XIM_EXT_MOVE` 才能拿到光标本体位置）、
   `XIM_OPEN` / `XIM_OPEN_REPLY`（分配 IM id）。

### 8.2 输入上下文

- `XIM_CREATE_IC` → 建 IC，回 `XIM_CREATE_IC_REPLY`；
- `XIM_SET_IC_VALUES` → `XICFocus`（进出焦点）、`XNSpotLocation`（**候选窗定位**）、
  `XNFilterEvents`、`XNInputStyle`；
- `XIM_SET_IC_FOCUS` / `XIM_UNSET_IC_FOCUS` → 对应 `ImeService::focus_in/out`；
- `XIM_RESET_IC`、`XIM_DESTROY_IC`、`XIM_CLOSE`、`XIM_DISCONNECT`。

### 8.3 按键

`XIM_FORWARD_EVENT`（客户端已经在自己的事件循环里做过 `XFilterEvent`）：
- 解析 `XKeyEvent` → keysym(=`XKeycodeToKeysym`/xkb) + states + release；
- `ImeService::handle_key(...)`；
- 返回 `handled=1` → 回 `XIM_SYNC_REPLY`（客户端据此丢弃该事件）；
- 返回 0 → 回 `XIM_SYNC_REPLY` 但 `forward`：用 `XSendEvent` 把原 `XKeyEvent`
  重新投递给客户端自己的窗口（等价 addon 的 `forward` 行）。

### 8.4 上屏与预编辑

- `ImeService` 的 `on_commit` → `XIM_COMMIT`（`XimCommit` + 文本，UTF-8）；
- `panel` → `XIM_PREEDIT_START` / `XIM_PREEDIT_DRAW` / `XIM_PREEDIT_DONE`
  （客户端自己画预编辑时），或什么都不发（我们自己的候选窗负责显示）。

### 8.5 需要复用的东西

- 键码：`XKeycodeToKeysym` + 我们已有的 `serenity_key_to_evdev`（反向转发用）。
- 坐标：IC 的 `XNSpotLocation` 是**相对窗口**的，加上窗口根坐标即屏幕坐标，
  正好喂 `ImeService::update_caret()` —— 和 Wayland 侧同一个入口。
- 候选窗：`ImeService` 里已有的 `InputMethodPanelWidget`，一行都不用改。

### 8.6 落地顺序

1. `XimServer.cpp`：selection + CONNECT/OPEN 握手（客户端能连上，日志可见）；
2. IC 生命周期 + 焦点 + spot location（候选窗能定位）；
3. FORWARD_EVENT + commit（能打字）；
4. 让 fcitx5 的 `xim` 插件彻底消失（否则抢 `@im=`），`XMODIFIERS=@im=serenity`。

---

## 9. `serenity-ime`：输入法后台服务（Serenity 原生进程）

用户定的形态：**写一个 Serenity 后台程序来监听**，而不是让桥或 fcitx5 各自一半。

### 9.1 职责（一个进程，三件事）

```
       原生 Serenity 应用 ─┐
       桥（Linux 客户端）─┤→ /tmp/serenity-ime.sock ─→ serenity-ime ─→ 引擎 socket ─→ fcitx5(rime)
       桥（X11 客户端）  ─┘                         │
                                                    └→ 候选窗（LibGUI，一个实现）
```

| 职责 | 为什么归它 |
|---|---|
| **socket 服务端**（现有行协议不变） | 客户端（LibGUI、桥）只认这一条 socket；多客户端由服务自己管，不依赖 addon 的多客户端改造 |
| **引擎代理** | 服务连 fcitx5 addon 的**另一条** socket（如 `/tmp/serenity-ime-engine.sock`），按 client 建/切 InputContext，做请求路由 |
| **候选窗** | 现在候选窗分散在两处：原生应用在进程内画、桥在桥里画。收进服务后**只画一次**，跟随主题，位置用客户端报的光标矩形 |

### 9.2 为什么不是「桥内建」（现状）

- 桥内建只能服务 Linux 客户端；原生应用的候选窗还在各自进程里，两套代码两套外观；
- 桥是合成器的命脉，把 IME 状态（socket、候选窗、超时）塞进去，**引擎或 UI 出问题会连累桌面**；
- 后台服务崩了只影响输入法，serenitid 拉回来即可（和现在的守护脚本同一个思路，但换成真正的进程）。

### 9.3 进程与文件

```
Ports/LinuxGUI/Apps/serenity-ime/     新程序（LibCore 事件循环 + LibGUI 候选窗）
  main.cpp
scripts/port/serenity-ime-daemon.sh   退化为「拉起 + 看护 serenity-ime + fcitx5 引擎」
SessionManager: child "ime" → /serenity/bin/serenity-ime
```

### 9.4 落地顺序（每步都可单独验证）

1. **骨架**：LibCore 建 listening socket，accept 多连接，逐行解析，`create/cap/focus/cursor/surround`
   先只记录并回 `ok`——用 `titest` / 原生 Notepad 验证连接与生命周期；
2. **引擎代理**：内部为每个 client 连一条到 addon，转发 `key`/`select`，把 `panel/commit/delete`
   按 client 回投；
3. **候选窗**：把桥里的 `ImeService` UI 部分搬过来（`InputMethodPanelWidget` 原样复用）；
4. **切流**：`LibGUI::InputMethod` 与桥都只连服务；addon 的 socket 改名 `…-engine.sock`，
   服务成为唯一对外入口；
5. 最后才删 `ImeService` 里的引擎/UI 代码，桥只留「把键和光标矩形送进去，把上屏文字拿出来」。

> 第 1 步不碰任何现有行为（服务空转），可以随时合入；第 4 步才需要一次性切换。

---

## 10. 阶段性验收（2026-11-07 21:40）

Wayland 通路**完整跑通**，VM 实测：

| 客户端 | 结果 |
|---|---|
| titest（极简 text_input_v3） | `TI commit '你'` |
| gedit（GTK3） | 文档出现「你」 |
| WordCraft（winit/egui，原生 Wayland） | 文档出现「你」+ Serenity 候选窗 |

链路：按键 → `SurfaceWidget` → `ImeService` → `/tmp/serenity-ime.sock`
→ `serenity-ime`（Serenity 后台服务）→ `/tmp/serenity-ime-engine.sock`
→ 裁剪版 fcitx5(rime) → `commit` 回投 → `text_input_v3.commit_string`。
候选窗由 `ImeService` 用 `GUI::InputMethodPanelWidget` 画（`WindowType::Autocomplete`）。

**尚未完成**：X11 客户端的 XIM 握手（`XimServer` 目前只注册了
`XIM_SERVERS` selection，没有回 `XIM_CONNECT_REPLY` / `OPEN` / `CREATE_IC` /
`FORWARD_EVENT` / `COMMIT`）。X11 客户端当前 `XMODIFIERS=@im=serenity` 会连不上。
