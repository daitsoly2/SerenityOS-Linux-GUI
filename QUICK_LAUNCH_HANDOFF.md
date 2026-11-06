# 快速启动栏 / 任务栏 —— 交接说明

> 状态：**已修好。图标、条目数、拖放、.desktop 条目都验证通过。**
> 最后更新：2026-11-01 19:30

## 1. 现在的样子

任务栏从左到右：`开始` → 快速启动（几个空按钮）→ 窗口按钮区 → 托盘（CPU/MEM/NET 图 + 音量 + 时钟）。

- ✅ 鼠标移上去**不再崩**（之前是整个 shell SIGILL）
- ✅ 条目确实加载了（日志 `quick-launch: 5 entries, width 120`）
- ❌ 按钮里**没有图标**（空按钮 / 一个深色占位方块）
- ❌ 5 个条目只画出 2 个
- ❌ 拖放（拖 .af 进快速启动栏）没验证过

## 2. 已定位并修掉的根因

### 2.1 崩溃：HiDPI 位图 blit

崩溃栈（`/var/log/serenity-session.log`）：

```
VERIFICATION FAILED: scale() >= source.scale() && "painter doesn't support downsampling scale factors"
    at LibGfx/Painter.cpp:1052
Gfx::Painter::blit(...)
serenity-shell(+0x38719)   ← QuickLaunchWidget::paint_event
```

`entry->icon().bitmap_for_size(16)` 返回的是 **2x 位图**（`scale()==2`），
`painter.blit()` 不支持缩小采样，直接 VERIFY。

修复（`Userland/Services/Taskbar/QuickLaunchWidget.cpp::paint_event`）：

```cpp
auto scale = icon->scale();
auto logical_size = Gfx::IntSize { icon->width() / scale, icon->height() / scale };
painter.draw_scaled_bitmap(Gfx::IntRect { icon_location, logical_size }, *icon, icon->rect());
```

顺带：`AppFile::icon()` 在宿主上会走 `FileIconProvider::icon_for_path("/bin/FileManager")`，
这个路径不存在 → 打 `Default icon not found` 然后 VERIFY。现在改成先按 `app-<kebab>`
去 `/res/icons` 找（`try_create_default_icon`，找不到就返回空 Icon），见
`Userland/Libraries/LibDesktop/AppFile.cpp`。

### 2.2 之前完全看不见：paint 从来没跑过

`QuickLaunchWidget::create()` → `load_entries()` → `repaint()` → `update()` 时 widget 还是
0 尺寸、未显示，update 被丢弃；之后布局给了 120x21 也没有新的重绘。补了
`resize_event` / `show_event` 里 `update()`（`QuickLaunchWidget.h/.cpp`）。

## 3. 第二轮修的问题（已验证）

| 症状 | 根因 | 修法 |
|---|---|---|
| 按钮全空 / 只有占位方块 | `paint_event` 里 `entry->icon().bitmap_for_size(16)`：返回的指针指向**临时 Icon 释放后**的位图 | 局部 `auto icon = entry->icon();`，并给条目加 `mutable Optional<GUI::Icon>` 缓存 |
| 6 个条目只画 5 个，重启后越来越少 | `create()` 用 `load_entries(true)` 写回整份配置 → Config 监听回调再 `load_entries()` → 读到写了一半的配置 | 启动只读不写（`load_entries(false)`） |
| 图标被压成一条 | `rect.shrunken(8, 2)` 把 24 宽缩成 8 | `shrunken(2, 2)` + 按逻辑尺寸居中 |
| 按钮底部被切 | `for_each_entry` 固定 24 高，任务栏内容区只有 21 | 用 `height()` |
| WebBrowser 没有图标 | 图标名不匹配（`app-web-browser` 不存在） | `AppFile::icon()` 枚举候选：kebab / 去连字符 / 单词回退 → `app-browser` |
| 拖进来的 .desktop 是空白图标 + 一长串路径名 | 落到 `QuickLaunchEntryFile` | 新增 `QuickLaunchEntryDesktopFile`，解析 Name/Exec/Icon |
| 悬停崩溃 | 见 §2.1 | 已修 |

拖放已验证：桌面图标拖到快速启动栏 → 写入 `Taskbar.ini`；快速启动栏内部拖动可以换位。

## 4. 仍待处理

- **系统级剪贴板**：`Userland/Services/Clipboard` 只在 `SERENITYOS` 下构建，移植里没跑
  ClipboardServer，所有程序的复制/粘贴都不可用（日志：`Clipboard: no clipboard server available`）。
  桌面右键的复制/剪切目前只是桌面内部剪贴板。
- **托盘 CPU/MEM/NET 图**：`serenity-resource-graph` 在托盘里画成黑块（背景没按主题填）。
- **TextEditor 未移植**：依赖 LibWebView/LibWeb，`gen-app-targets.py` 直接跳过，
  快速启动栏里那个按钮点了会失败。

## 5. 怎么复现 / 调试

```bash
# 1) 编译 + 安装 + 重启会话（在 VM 内，仓库根）
cd ~/serenity && cmake --build Build/lagom --target serenity-shell -j6
sudo bash scripts/port/05-install-prefix.sh        # 注意用 bash，脚本不是 POSIX sh
sudo systemctl restart serenity-desktop.service

# 2) 看崩溃
sudo grep -n "VERIFICATION FAILED" /var/log/serenity-session.log | tail -3
L=$(sudo grep -n "shell exited" /var/log/serenity-session.log | tail -1 | cut -d: -f1)
sudo sed -n "$((L-22)),$((L-1))p" /var/log/serenity-session.log

# 3) 截图 / 模拟鼠标（宿主侧，VNC 通道 5901）
python3 Build/vm/vnc.py   # 见 Build/vm/vnc.py 里的 VNC 类
# 用法示例：
#   v = VNC(); v.pointer(100, 946, 0)   # 悬停快速启动栏
#   v.shot("Build/vm/shot.ppm")         # 存 PPM，再 convert 成 PNG
```

快速启动栏的条目来自 **目录**（不是写死的）：

```
~/.local/share/quick-launch/          # 首次运行自动创建，放 .af 链接
  Calculator.af -> /res/apps/Calculator.af
  FileManager.af -> /res/apps/FileManager.af
  Terminal.af -> /res/apps/Terminal.af
  WebBrowser.af -> /res/apps/WebBrowser.af
```

## 6. 相关文件

| 文件 | 作用 |
|---|---|
| `Userland/Services/Taskbar/QuickLaunchWidget.{h,cpp}` | 快速启动栏本体（自绘 + 拖放骨架） |
| `Userland/Services/Taskbar/TaskbarWindow.cpp` | 任务栏布局；`populate_taskbar()` 里按顺序加 开始/快速启动/窗口/托盘 |
| `Userland/Libraries/LibDesktop/AppFile.cpp` | `icon()` 的宿主分支（app-<kebab> → /res/icons） |
| `Ports/LinuxGUI/Shell/DesktopIconsWidget.cpp` | 桌面图标（拖拽源要加在这里） |
| `scripts/port/05-install-prefix.sh` | 安装到 `/serenity`（用 bash 跑） |

## 7. 建议的下一步顺序

1. 修图标（3.1）——用 `Bitmap::load_from_file` 兜底，最快见效；
2. 修条目数（3.2）——先加 dbgln 确认 `m_entries.size()` 和 `rect()`；
3. 桌面拖拽源 → 快速启动 drop 目标（3.3）；
4. 都通了之后，把「快速启动栏」的默认内容改成从 `Base/res/apps/*.af` 里挑几个
   `Category=&Utilities` 的写进目录，而不是硬编码。

## 8. 别踩的坑

- **`sh script.sh` 会失败**：`05-install-prefix.sh` 用了 `[[`、`${BASH_SOURCE}`，必须 `bash`。
- **`pkill -x serenity-shell` 这类 >15 字符的进程名匹配不到**（comm 截断到 15 字符），
  用 `pkill -x serenity-shel` 或 `pgrep -x` 确认。
- **别再用 `insert_child_before` 重排任务栏子控件**：在当前 LibGUI 里会把「开始」按钮
  弄丢或把快速启动挤到屏幕外。顺序靠 `populate_taskbar()` 里的 `add_child` 顺序保证。
- **`GUI::Icon::default_icon()` 找不到图标会 VERIFY**，一律用 `try_create_default_icon()`。
