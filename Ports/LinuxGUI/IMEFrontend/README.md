# 输入法：fcitx5 引擎 + LibGUI 客户端

SerenityOS 的 `TextEditor` 原本只认 `KeyEvent::code_point()`，没有任何输入法
概念。本移植把输入法拆成两半，中间用一条 unix socket 上的行协议连接：

```
   LibGUI 进程（任何一个 app）                  fcitx5 进程
   ┌───────────────────────────┐               ┌──────────────────────────────┐
   │ TextEditor::keydown_event │               │ serenityime 插件（本目录）    │
   │   → GUI::InputMethod      │  unix socket  │   → fcitx 引擎（pinyin 等）   │
   │      key/commit/panel     │◄─────────────►│   → InputPanel（预编辑/候选） │
   │   → InputMethodCandidate  │  /tmp/        │                              │
   │      Window（候选窗）      │  serenity-    │                              │
   └───────────────────────────┘  ime.sock     └──────────────────────────────┘
```

- **引擎侧**：`serenityime.cpp`，一个 fcitx5 addon。它直接链接 `libFcitx5Core`，
  所以能拿到**已经组好的** `InputPanel`（预编辑、候选列表、光标），而不是像
  D-Bus 客户端那样去解序列化的 UI blob。
- **客户端侧**：`Userland/Libraries/LibGUI/InputMethod.{h,cpp}`。它只依赖
  POSIX socket，不链接任何 fcitx 代码；候选窗是普通的 `GUI::Window`。

## 为什么不是 D-Bus

fcitx5 的 D-Bus 客户端协议只传"客户端侧 UI"的序列化数据，前端要自己解包；而且
Serenity 这边就得引入 libdbus。addon 方案让 Serenity 端只需要一条 socket，逻辑
小得多。

## 协议

UTF-8 字符串一律十六进制编码，所以协议保持按行、无转义。

| 方向 | 命令 | 说明 |
|---|---|---|
| 客户端 → 引擎 | `create` | 建输入上下文，回 `ok` |
| | `cap <flags>` | 能力位（addon 目前固定启用 ClientSideUI） |
| | `focus <0\|1>` | 焦点进出 |
| | `cursor <x> <y> <w> <h>` | 光标矩形（屏幕像素） |
| | `surround <cursor> <anchor> <hex>` | 周边文本全文 + 光标/锚点（**字符**偏移，非字节） |
| | `key <keysym> <states> <release>` | 一个按键，回 `handled <0\|1>` |
| | `select <index>` | 鼠标点选第 `index` 个候选（0 基），回 `handled 0` |
| | `reset` / `destroy` | 放弃组合 / 销毁上下文 |
| 引擎 → 客户端 | `commit <hex>` | 上屏文本 |
| | `delete <offset> <size>` | 删除光标周围文本（`offset`/`size` 为 **UCS4 字符**，`offset` 可为负=光标前） |
| | `forward <keysym> <states> <release>` | 引擎不处理、交还应用的键 |
| | `panel <cursor> <aux> <preedit> <sel> <n> [<label> <text>]*` | 预编辑与候选 |
| | `active <0\|1>` | 输入法开关状态 |

**关键细节**：一次 `key` 的回复可能是若干行 `commit` / `panel`，最后才是
`handled`。客户端必须一直读到 `handled` 为止（`InputMethod::read_until_handled`），
不能假设"一次 read 一个回复"——本地 socket 上也经常只读到 `panel` 那一行。

**周边文本与删除**：fcitx5 的 `deleteSurroundingText()` 会先拿"周边文本"做范围
校验，周边文本为空时这次删除会被直接吞掉、根本不会回调前端。所以客户端在
`focus_in` 之后、以及每次按键往返结束后，都用 `surround` 把整段文本和光标/锚点
（字符偏移）推给引擎（`InputMethod::send_surrounding_text`）。引擎侧的 `delete`
再把 `offset`/`size`（UCS4 字符，`offset` 负值表示光标前）交回客户端，客户端按
真实 buffer 换算成 `TextRange` 调 `delete_text_range()`，删除会进正常的 undo 栈。
注意：预编辑**不进** buffer，所以拼音这类"在预编辑里删字符"的引擎本来就不需要它；
`delete` 主要是给会改动已上屏文本的引擎兜底。

`keysym` 用 X11 的键值：可打印字符直接用码点（fcitx 会归一化大小写），
功能键用 `0xffxx`。`states` 与 X11 修饰位一致（Shift=1、Lock=2、Ctrl=4、
Mod1/Alt=8、Mod4/Super=64），正好也是 fcitx5 的 `KeyState` 布局。

## 构建与安装

```bash
# 依赖（Debian）
sudo apt install fcitx5 fcitx5-chinese-addons libfcitx5core-dev cmake ninja-build

# 编译并安装 addon（装进系统的 fcitx5 addon 目录）
sudo scripts/port/build-ime.sh

# 让 fcitx5 重新加载
fcitx5 -r
```

LibGUI 客户端随主构建一起编译（`ENABLE_LAGOM_GUI_APPS=ON`），不需要单独处理。
`serenitid` 会在 Wayland host 之后、shell 之前拉起 `fcitx5`（见
`SessionManager/serenity_manager.cpp`）；没装 fcitx5 时自动跳过。

## 客户端挂钩点

| 位置 | 作用 |
|---|---|
| `TextEditor::keydown_event` 开头 | 可编辑时先把键交给 `InputMethod::handle_key_event` |
| `TextEditor::focusin_event` / `focusout_event` | 连接 / 断开引擎，发 `focus 1/0`，并首推 `surround` |
| `TextEditor::cursor_did_change` | 重发光标矩形，候选窗跟着走 |
| `TextEditor::cursor_screen_rect()` | 新增：把内容坐标换算成屏幕坐标 |
| `InputMethod::handle_key_event` 末尾 | 每次按键往返后重发 `surround`，保持周边文本同步 |
| `TaskbarWindow::populate_taskbar` | 任务栏加入 `GUI::IMEIndicator` 按钮（读 status 文件、经控制 socket 切输入法） |

候选窗是 `InputMethodCandidateWindow`（`WindowType::Autocomplete`，无边框、
不进任务栏），由 `InputMethodPanelWidget` 画在 `palette()` 配色上（跟随主题）：

- 顶部淡色 `aux` 行（引擎给的音节/声调提示，可为空）；
- 预编辑行：在编拼写，行内画一个文本光标标出下一个字符落点；
- 候选区：**竖排**，每行 `序号 词条`，`cursor_index` 指向的当前项用
  `selection()` 深蓝高亮 + `selection_text()` 白字；
- 翻页指示行：有上一页/下一页时分别在左下、右下画 `<` / `>`。

定位在光标正下方 2px；若会超出所在窗口底边则翻到光标上方；
水平方向夹在窗口内，不会跑出屏幕。

候选窗是可交互的：点候选行 = 选该候选（`InputMethod::select_candidate` → `select <index>`
→ addon 调 fcitx5 `InputContext::selectCandidate` 后重推 `panel`）；点 `<` / `>` 翻页 = 给引擎
发一次 `PageUp` / `PageDown` 键（`InputMethod::request_page`，复用 `key` 命令，addon 原样转发给
fcitx5）。两条路径都挂在 `InputMethodCandidateWindow` 的回调上，由 `InputMethod::update_panel_window`
在每次建/刷新候选窗时接好。

## 输入法指示器

任务栏托盘里有一个 `中/EN` 按钮（`GUI::IMEIndicator`，见
`Ports/LinuxGUI/IMEFrontend/IMEIndicator.{h,cpp}`）。它本身**不链接任何 fcitx 代码**——
它只是个普通的 `GUI::Button`，通过两条 auxiliary 通道和引擎侧的 `serenityime` addon 通信：

- **状态通道（只读）**：addon 把"当前输入法"写进两个小文件
  （`/tmp/serenity-ime-status`、`/tmp/serenity-ime-methods`，可用
  `SERENITY_IME_STATUS_DIR` 改目录）。指示器每 500ms 轮询一次：status 文件是
  `active\tunique_name\tdisplay_name` 一行；methods 文件是
  `unique_name\tdisplay_name` 每行一个。判断"是不是中文输入法"用和 addon 一致的规则
  （`keyboard-*` / `english` / `kbd` 视为键盘布局，其余视为 CJK）。
- **控制通道（只写）**：addon 另开一个 unix socket（`/tmp/serenity-ime-ctl.sock`）。
  指示器点击后弹菜单列所有输入法 + "Turn IME off"，选了就往 socket 发一行命令：
  `set <unique_name>` / `toggle off` / `next` / `prev`。命令火发即弃，addon 执行后重写
  status 文件，指示器下一次轮询自然刷新；看不到的 addon（没跑 fcitx5）也不报错。

绘制：CJK 激活时画一个 `selection()` 深蓝块里写白字 `中`；否则写灰字 `EN`
（`disabled_text_front()`）。整条链路都跟着 Win2000 主题走。

> 注：指示器只在 `serenity-shell` 里编译进任务栏（见
> `Ports/LinuxGUI/Shell/CMakeLists.txt` 把 `IMEIndicator.cpp` 加进 `serenity-shell`，
> 并把它的目录加进 include path）。

## 字体依赖

输入法只负责"产生字符"，不负责"画出字符"。中文要能显示，`TextEditor` 用的
字体必须含 CJK 字形——见 `LINUX_GUI_PORT.md` §4.5（Sarasa Gothic SC）。
Windows 应用那边是另一套（Wine 前缀里的字体替换），见 `scripts/port/wine-fonts.py`。

## 测试

不装 fcitx5 也能验证客户端：

```bash
# 假引擎：同样的协议，不做拼音查找
scripts/port/ime-fake-engine.py &

# 打开任意带文本框的 app，聚焦，输入 nihao，回车
# → 候选窗出现三个假候选，回车提交"你好"
```

装了 addon 时用 `ime_probe.py` 直接探协议：

```bash
python3 Ports/LinuxGUI/IMEFrontend/ime_probe.py n i h a o
```

## 已知限制

- **只覆盖 LibGUI 文本控件**。GTK / Qt / Wine 客户端走 fcitx 自己的 IM module
  （`GTK_IM_MODULE=fcitx`）或 XIM（`XMODIFIERS=@im=fcitx`），不经过这条 socket。
- **只转发按下**，不转发抬起（`TextEditor` 没有 keyup 钩子）。拼音不受影响。
- **addon 同时只接受一个客户端**，所以客户端在 focus in 时连接、focus out 时断开。
  同一时刻只有焦点应用在说话，够用。
- **`forward` 仍是空实现**：引擎不消费的键走 `handled 0` 由编辑器自己处理，普通
  路径没问题；但"引擎消费了键又想把它转给应用"这种情形不会重放。
- **`delete` 依赖 `surround` 同步**：客户端每次按键后整段重传周边文本，文本很大时
  有点浪费（尚未做光标附近的窗口裁剪）。
- 预编辑已内联到文本光标处（见 `TextEditor::set_preedit` + `paint_event` 的 splice
  分支）；候选窗不再重复画拼音，只保留 `aux` 提示 + 候选列表 + 翻页。长拼音超出可视
  宽度时目前不会被横向滚动跟随（待改进）。
- 每次按键有一次同步往返（`poll` 上限 100ms）。本地 socket 实测亚毫秒级，但如果
  引擎卡住，最坏会等 100ms 才把键交还给编辑器。

## 待办 (TODO)

按"能不能在本沙箱做"分组；带 🖥️ 的必须在装了桌面的 VM 上验证。

### 已完成（本轮）

- [x] 客户端 `delete` 处理：`offset`/`size` 按 UCS4 字符换算成 `TextRange`，
      走 `delete_text_range()`，进 undo 栈。
- [x] 周边文本上传：`surround <cursor> <anchor> <hex>`，`focus_in` 后与每次按键后各推一次。
- [x] addon 侧解析 `surround` 并 `updateSurroundingText()`。
- [x] 候选窗重设计：竖排候选（序号+词条、当前项深蓝高亮）、预编辑行内光标、
      aux 提示行、翻页 `<`/`>` 指示、屏幕边缘翻转与夹取。
- [x] 协议扩展：`panel` 行末尾追加两个可省略标志 `has_prev has_next`
      （旧 addon 不发的客户端忽略），addon 侧从 `candidateList()->hasPrev()/hasNext()` 取值。
- [x] 输入法指示器：任务栏 `中/EN` 按钮（`GUI::IMEIndicator`），只读 addon 发的
      status/methods 文件、经控制 socket 发 `set/toggle/next/prev` 切换输入法。
- [x] 候选窗鼠标交互：点候选行 → `select <index>` 提交对应候选；点 `<`/`>` → 复用 `key`
      命令发 `PageUp`/`PageDown` 翻页。addon 新增 `select` 分支（`selectCandidate` + 重推 `panel`）。
      假引擎同步支持 `select`，点选候选在 dev harness 也能验证。
- [x] addon 侧状态发布与控制 socket：`publish_status()` / `publish_methods()` 写入
      status 目录；`open_control_server()` 接收 `set/toggle/next/prev` 并调
      `FocusGroup`/`InputMethodManager` 切换。

### 引擎侧 / addon（需 `libfcitx5core-dev`，本沙箱没有）

- [ ] 🖥️ `sudo scripts/port/build-ime.sh` 能编译过（重点核对
      `surroundingText().setText()` 与 `updateSurroundingText()` 是否匹配本机 fcitx5 版本；
      以及本次新增的 `candidateList()->hasPrev()/hasNext()` 与 `InputContext::selectCandidate(int)`
      是否为该版本已有 API）。
- [ ] 🖥️ `fcitx5 -r` 后 `/tmp/serenity-ime.sock` 出现，addon 被加载。
- [ ] 🖥️ 用 pinyin 打 `nihao` → 候选窗（竖排）→ 回车提交"你好"。
- [ ] 🖥️ 任务栏出现 `中/EN` 指示器：pinyin 激活显示 `中`、否则 `EN`；点击弹输入法菜单、
      切换后状态随之更新（核对 addon `publish_status()` 与 `handle_control_command` 调用的
      `FocusGroup::setInputMethod` / `nextInputMethod` / `InputMethodManager::listInputMethods`
      是否匹配本机 fcitx5 版本）。

### 会话集成

- [ ] 🖥️ `serenitid` 拉起 fcitx5 后，任意 LibGUI app（`TextEditor`）能直接输入中文。
- [ ] 环境变量 `GTK_IM_MODULE`/`QT_IM_MODULE`/`XMODIFIERS` 对 Wine/GTK 是否真的生效（另测）。

### 客户端增强

- [ ] `keyup_event` 转发（当前只发 key down）。
- [ ] `forward` 真正重放：把引擎要求转发的键合成 `KeyEvent` 塞回编辑器。
- [ ] 周边文本窗口裁剪：只传光标前后各 N 字符，而不是整篇文档。
- [x] 内联预编辑：把预编辑画在文本光标处，而不是候选窗第一行。
- [ ] 选区 → 周边文本 `anchor`（当前 anchor 恒等于 cursor）。
- [ ] 超时重连抖动优化（当前超时即断、下次按键再连）。

### Wine 字体（另一条线）

- [ ] 🖥️ `sudo /serenity/bin/serenity-wine-fonts --prefix /root/.wine-dw`，用中文 exe 确认不再方框。
- [ ] 🖥️ 确认 `winapp-launcher.sh` 的自愈分支在真前缀上触发一次。
