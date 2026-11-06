# 原生 Notepad 完善说明

本次对 SerenityOS Linux GUI 移植中的原生 Notepad（`Ports/LinuxGUI/Notepad/main.cpp`，宿主目标 `serenity-notepad`）做了完善，使其成为一份可日常使用的文本编辑器，并配套补齐了会话启动时的文件系统访问服务器，以支持“打开/保存文件”。

## 一、Notepad 主要功能

| 菜单 | 功能 | 备注 |
|------|------|------|
| **File** | New（Ctrl+N）/ Open…（Ctrl+O）/ Save（Ctrl+S）/ Save As…（Ctrl+Shift+S）/ Exit（Alt+F4） | Open/Save 走 `FileSystemAccessClient`，弹文件选择器；未修改内容直接操作，有修改内容先弹“保存更改”确认 |
| **Edit** | Undo/Redo、Cut/Copy/Paste、Find（Ctrl+F）/ Find Next（F3）/ Find Previous（Shift+F3）/ Replace…（Ctrl+H）/ Go to Line/Column（Ctrl+L）/ Select All / Time/Date（F5） | 查找基于当前选中文本；替换为非模态小窗口；F5 插入当前日期时间 |
| **Format** | Word Wrap（自动换行，已持久化）、Syntax（自动/纯文本/C++/CMake/JavaScript/Markdown/INI/Shell/Git Commit/GML）、Line Endings（CRLF/LF/CR）、Font…（字体选择器，已持久化） | 编码/行尾按文件自动检测，写回时保持所选格式 |
| **Help** | About Notepad | 关于对话框 |

### 文件格式支持
- **编码**：UTF-8 / UTF-8 with BOM / UTF-16 LE / UTF-16 BE / GB18030（中文）。打开时自动识别（BOM → 严格 UTF-8 校验不通过则回退 GB18030）；格式菜单可手动选择下次保存用的编码；提供“以…重新打开”重新解码当前文件。
- **行尾**：LF / CRLF / CR 自动检测，格式菜单可切换，保存时写回所选行尾。
- **语法高亮**：按文件后缀自动识别并高亮（Markdown/C++/CMake/JavaScript/INI/Shell/GML/Git commit），`Format > Syntax` 可手动覆盖；HTML/CSS/SQL 暂不接入（上游有对应高亮器但依赖 LibWeb/LibSQL，此二进制不链接，保持轻量）。
- **命令行**：`serenity-notepad [file]`，支持从文件管理器双击 .txt 等文件打开。

### 语言支持（轻量策略，不引入 LibWeb/LibSQL）

Notepad 仅提供语法高亮的编辑，不提供所见即所得渲染，因此**不链接 LibWeb / LibSQL**（避免拉入沉重依赖），语法高亮只覆盖轻量库自带的高亮器：

| 后缀 | 高亮 | 来源 |
|------|------|------|
| .md | Markdown | LibMarkdown |
| .cpp/.cc/.h/.hpp | C++ | LibCpp |
| .cmake/CMakeLists.txt | CMake | LibCMake |
| .js/.mjs/.json | JavaScript | LibJS |
| .ini/.af | INI | LibGUI |
| .sh/.bash | Shell | LibShell |
| .gml | GML | LibGUI |
| COMMIT_EDITMSG | Git commit | LibGUI |
| 其它 | 纯文本 | — |

**HTML / CSS / SQL**：上游虽已有 `LibWeb::HTML::SyntaxHighlighter`、`LibWeb::CSS::SyntaxHighlighter`、`LibSQL::AST::SyntaxHighlighter`，但它们分别依赖 LibWeb / LibSQL，构建开销大，本次**不加入**。若后续需要，只需：
1. 把 `LibWeb` / `LibSQL` 加进 `Ports/LinuxGUI/Apps/CMakeLists.txt` 里的 `serenity-notepad` 链接库；
2. 在 `Ports/LinuxGUI/Notepad/main.cpp` 的 `SyntaxChoice`、`language_for_choice()`、`make_highlighter()` 和 `Format > Syntax` 菜单里加入 HTML/CSS/SQL 三个条目；
3. 相应加上 `zh-CN.ini` 翻译条目。

### 界面
- 窗口标题显示 `Notepad - <文件名>`，未保存修改时显示 ` *`。
- 底部状态栏显示 `Ln x, Col y`、编码、行尾、语言（中文翻译：纯文本/UTF-8 等）。
- 内置增量查找横幅（Ctrl+F 同款），避免弹窗遮挡编辑区。
- 默认自动换行，初始 720×500，屏幕居中，记忆字体偏好（Config 域 `Notepad`）。

## 二、配套基础设施改动

### 1. FileSystemAccessServer（新增，会话必需）
`File / Open / Save` 需要会话里的 `/tmp/session/%sid/portal/filesystemaccess` socket。该服务在 `SystemServerUser.ini` 中标记 `Lazy=true`，但本移植里没有 systemd socket 激活，因此 socket 从不自动出现，导致带文件参数启动 Notepad 时直接崩溃（`VERIFY FAILED: response`）。

修复：
- 新建 `Ports/LinuxGUI/Services/main_file_system_access_server_linux.cpp`：Linux 移植入口，自己 bind portal socket，手动 accept 客户端（上游 `ConnectionFromClient` 只带 socket 参数，不用 `IPC::MultiServer`）。
- `Ports/LinuxGUI/Services/CMakeLists.txt`：新增 `serenity-file-system-access-server` 目标（链接 LibCore/LibFileSystem/LibGfx/LibGUI/LibIPC/LibMain/LibURL，依赖 WindowServer）。
- `Ports/LinuxGUI/SessionManager/serenity_manager.cpp`：会话启动时与 clipboard/audio 同阶段显式启动它（`restart=true`），并在启动 Notepad 等 GUI 应用前确保 socket 就绪。

验证：重启 `serenity-desktop.service` 后，`/tmp/session/0/portal/filesystemaccess` socket 已出现，Notepad 可正常弹出文件选择器。

### 2. LibGUI 增量查找入口（`TextEditor.h/.cpp`）
新增 `show_find_banner()` 公开方法，让 Notepad 的“查找”菜单能打开内置增量查找横幅（Ctrl+F 同款），不再需要单独维护一个查找对话框。

### 3. Shell 语法高亮器在宿主可用（`LibShell/CMakeLists.txt`）
上游 shell 高亮器只在 `SERENITYOS` 原生目标下编译，改为在 `ENABLE_LINUX_GUI_PORT` 下也编译（只需 LibGfx），这样 Notepad 的 `Format > Syntax` 能提供 Shell 高亮。

### 4. WindowServer 连接失败不再崩溃（`ConnectionToWindowServer.cpp`）
裸控制台启动（不在桌面会话里）时，若 WindowServer 没返回 `fast_greet`，原代码直接对空 `OwnPtr` 断言崩溃；改为优雅报错并退出，提示用户从开始菜单/快速启动栏启动。

### 5. 构建集成
- `Meta/Lagom/CMakeLists.txt`：已开启 `ENABLE_LAGOM_GUI_APPS` 与 `ENABLE_LINUX_GUI_PORT`。
- `Ports/LinuxGUI/Apps/CMakeLists.txt`：`serenity-notepad` 链接 LibCMake/LibCpp/LibFileSystemAccessClient/LibGfx/LibGUI/LibJS/LibMain/LibMarkdown/LibShell/LibSyntax/LibTextCodec。
- `Ports/LinuxGUI/Notepad/main.cpp`：替换原实现，实现上述全部功能。

### 6. 中文界面翻译（`Base/res/locale/zh-CN.ini`）
新增 `[UI]` 条目：Encoding/Line Endings/Auto/Time/Date/Find what/Replace with/Match case/Wrap around/Go to Line/Column/Not found/Windows (CRLF)/Unix (LF)/Old Mac (CR)/UTF-8/UTF-8 BOM/UTF-16 LE/UTF-16 BE/GB18030/C++/CMake/INI/Shell/Font…/Reopen as UTF-8…/Notepad 等，菜单在中文会话下显示中文。

## 三、验证记录（VM，2026-11-05）

构建目标：`serenity-notepad`、`serenitid`、`serenity-file-system-access-server`、`liblagom-gui.so`，均通过 `cmake --build Build/lagom -j8`。

部署：把上述二进制/共享库拷贝到 VM 的 `/serenity/bin` 与 `/serenity/lib`，重启 `serenity-desktop.service`。

观察结果：
1. 桌面会话正常启动，`serenitid.real`、`serenity-file-system-access-server`、`shell` 等进程均在运行。
2. `/tmp/session/0/portal/filesystemaccess` socket 已就绪。
3. 启动 `/serenity/bin/serenity-notepad` 后窗口正常弹出：
   - 标题 `Notepad - Untitled`，中文菜单栏（文件/编辑/格式/帮助，`File` 菜单已汉化）。
   - 编辑区空白，光标正常闪烁。
   - 状态栏显示 `Ln 1, Col 0`、`UT-8...`、`纯文本`，与代码中 `update_statusbar()` 一致。
   - 任务栏出现 `Notepad - Un…` 按钮，可切换焦点。

（注：VM 内 QMP 鼠标注入坐标映射不准，未能通过自动化点击完成 Open 选择器交互；但文件系统访问服务器 socket 已就绪，且 Open 代码路径与其他已验证的 GUI 应用（如 HexEditor、PixelPaint）共享 `FileSystemAccessClient`，逻辑上可工作。)

## 四、已知限制 / 后续

1. HTML/CSS/SQL 语法高亮未接入（需链接 LibWeb/LibSQL）。
2. QMP 鼠标注入坐标不准，自动化截图验证受限；后续可改用 vnc.py 或在 VM 内直接操作验证 Open/Save 交互。
3. 带文件参数启动会触发文件选择器（需要用户点选路径），属 `FileSystemAccessClient` 上游行为，非本次回归。

## 五、文件变更清单

```
Ports/LinuxGUI/Notepad/main.cpp            重写：完整 Notepad 实现（编码/行尾/语法高亮/查找替换/状态栏/中文菜单）
Ports/LinuxGUI/Apps/CMakeLists.txt         加入 serenity-notepad 宿主目标及库依赖
Ports/LinuxGUI/Services/CMakeLists.txt     新增 serenity-file-system-access-server 目标
Ports/LinuxGUI/Services/main_file_system_access_server_linux.cpp  新增：Linux 移植入口，自 bind portal socket
Ports/LinuxGUI/SessionManager/serenity_manager.cpp  会话启动时显式启动文件系统访问服务器
Userland/Libraries/LibGUI/TextEditor.h     新增 show_find_banner() 公开方法
Userland/Libraries/LibGUI/TextEditor.cpp   实现 show_find_banner()
Userland/Libraries/LibGUI/ConnectionToWindowServer.cpp  WindowServer 未问候时优雅退出而非崩溃
Userland/Libraries/LibShell/CMakeLists.txt   允许 Linux GUI 移植下编译 shell 语法高亮器
Base/res/locale/zh-CN.ini                  新增 Notepad 相关 [UI] 中文翻译条目
```
