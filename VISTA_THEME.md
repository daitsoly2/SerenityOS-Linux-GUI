# Vista 复刻与「关闭主题」开关（系统级需求）

> 记录时间：2026-11-05
> 状态：方向已定，实现进行中
> 适用范围：**整个桌面**，不是单个应用

## 1. 目标

用户明确要求：**复刻 Vista 风格的系统**，且**能关闭主题回到经典模式**。
XP/Vista 的复古感本身就来自「主题服务关闭后回落到经典外观」，所以这里不重做一套
主题引擎，而是复用 SerenityOS 已有的系统主题管线，把 Vista 做成**一份可切换的主题**。

- 「开启主题」= Vista/Aero：浅色玻璃 chrome、蓝色高亮、Aero 窗口装饰。
- 「关闭主题」= 经典：`Redmond 2000` / `Default`，Win2000 灰面板 + 经典边框。
- **不允许**每个应用各自硬编码一套 Aero 颜色。所有应用（原生 LibGUI、
  GTK3/GTK4/libadwaita、Wine 窗口装饰）都从同一份系统主题取色。

## 2. 架构决策（与之前的做法对比）

| | 之前（已否决） | 现在（本文件定稿） |
|---|---|---|
| Aero 配色来源 | `FileManager::Aero` 硬编码在应用内 | 系统主题 ini 的 `[Colors]`，经 `Gfx::Palette` / `ColorRole` 下发 |
| 影响面 | 只有文件管理器像 Vista | 所有 LibGUI 应用 + 窗口装饰 + GTK 应用 |
| 关闭主题 | 无效果，应用仍自绘 Aero | 自动回落到经典配色（chrome 也跟着变灰） |
| 文件管理器 chrome | 自绘 + 硬编码色 | 仍自绘**结构**（命令栏/地址栏/导航窗格/预览窗格），但**颜色全部读 `GUI::Application::the()->palette()`** |

关键结论：**结构可以应用内自绘，颜色必须来自系统主题**。这样 Vista 化是系统级的，
而「关闭主题」这一个动作就能整体回到经典，符合 XP/Vista 的语义。

## 3. 系统主题管线的现有落点（无需另造）

| 层 | 文件 | 作用 |
|---|---|---|
| 主题数据 | `Base/res/themes/*.ini` | `[Colors]` 对应 `Gfx::ColorRole`；`[Window] WindowTheme` 选窗口装饰实现 |
| 控件配色 | `Userland/Libraries/LibGfx/Palette.cpp/.h`、`SystemTheme.h` | 把 ini 读进 `SystemTheme`，通过 IPC 下发给客户端 |
| Aero 窗口装饰 | `Userland/Libraries/LibGfx/GlassWindowTheme.cpp`（`WindowThemeProvider::RedmondGlass`） | 已实现玻璃标题栏，直接复用 |
| 主题切换 | `WindowServer/WindowManager.cpp::update_theme()` | 写 `[Theme] Name` + 应用 + 通知全部客户端重绘 |
| GTK/GTK4/Qt/libadwaita | `scripts/port/serenity-theme-gen.py --theme <name> --out /usr` | 从同一份 ini 生成，天然同步 |
| 当前配置 | `/etc/WindowServer.ini` `[Theme] Name=` | 指向主题 ini 的文件名（不含扩展名） |

因此「全局 Vista」= 新增一份 `Redmond Vista.ini` + 把开关接通，不需要改
`Palette.cpp` / `WindowFrame.cpp` 的实现。

## 4. 主题文件

`Base/res/themes/Redmond Vista.ini`，基于 `Redmond Glass.ini` 的窗口装饰
（`WindowTheme=RedmondGlass`、`BorderRadius=5`、`TitleHeight=23`），配色换成 Aero 蓝：

| ColorRole | Vista 值 | 经典值（关闭主题） |
|---|---|---|
| `Accent` / `Selection` | `#2f6fd0` | `#4a6eab` / `#1a3584` |
| `Window` / `Button` | `#f0f4fa` | `#d4d0c8` |
| `Base` / `MenuBase` | `#ffffff` | `#ffffff` |
| `WindowText` / `BaseText` | `#1f2a37` | `#000000` |
| `DisabledTextFront` / `PlaceholderText` | `#607086` | `#808080` |
| `HoverHighlight` | `#dcebfc` | `#e3dfdb` |
| `MenuSelection` | `#c6dcf8` | `#b6c0d2` |
| `ThreedShadow1` / `GutterBorder` | `#b6c6dc` | `#808080` / `#404040` |
| `Gutter` / `Ruler` | `#eef3fa` | `#d4d0c8` |
| `ActiveWindowBorder1/2` | `#123f8c` / `#a9cdf5` | `#09226e` / `#9ecaf4` |

文件管理器 `ExplorerChrome.cpp` 的颜色映射：

```
chrome_top/bottom ← Window / Button
chrome_border     ← ThreedShadow1
nav_top/bottom    ← MenuBase / Window
nav_hover         ← HoverHighlight
nav_selected      ← MenuSelection
text/text_dim     ← WindowText / DisabledTextFront
glass/white/base  ← HoverHighlight / Window / Base
```

关掉主题（切回 `Redmond 2000`）时这些角色自动变回 Win2000 灰，chrome 无需任何分支判断。

## 5. 开关使用

```bash
# 开启 Vista（Aero）
sudo scripts/port/serenity-theme.sh vista

# 关闭主题 → 回到经典 Win2000
sudo scripts/port/serenity-theme.sh classic

# 查看当前
scripts/port/serenity-theme.sh status
```

脚本做三件事：
1. 改 `/etc/WindowServer.ini` 的 `[Theme] Name`；
2. `serenity-theme-gen.py --theme "<name>" --out /usr` 重生成 GTK3/GTK4/libadwaita/Qt 主题；
3. 重启 `serenity-desktop.service`（WindowServer 重新加载主题并广播给所有客户端）。

「关闭主题」在 XP/Vista 里是停掉 Themes 服务；在这里等价于把主题指回经典 ini，
效果一致：玻璃装饰消失、面板变灰、所有应用同时回落。

## 6. 持久化

`/etc/WindowServer.ini` 是指向安装前缀的软链，而会话以非特权用户运行，
`WindowManager::sync_config_to_disk()` 写不回去，导致「显示设置」里改主题生效但
重启后丢失（旧 STATUS §1.4 已记录）。`05-install-prefix.sh` 现在把 `${PREFIX}/etc`
交给会话用户、`WindowServer.ini` 设为 664，让 UI 内的主题切换也能跨重启保留。
`06-install-host.sh` 原本已经这样处理。

## 6.1 KMS 合成：单份帧内容 + 全屏提交（2026-11-05 修复）

**症状**：Vista 主题下，鼠标**左右**扫过命令栏/任务栏时，按钮文字和图标成片消失
（按钮仍可点击），再 hover 回去又显示；**上下**扫过不出现。任务栏同样中招。

**根因**（`Ports/LinuxGUI/LinuxScreenBackend.cpp`）：

1. KMS presenter 用 `m_back_buffer_offset = m_size_in_bytes / 2`，即 `m_framebuffer`
   分成两个半区。WindowServer 只会往即将 flip 的那个半区画脏区，另一半停留在上一帧。
2. 脏区提交走 `present_buffer_kms_rects()`，用 `pending_sync[16]` / `accum_sync[16]`
   在 flip 前补齐另一个 scanout buffer。左右扫过时每个按钮的 hover-in/out 各产生一个
   水平分散的小矩形，一帧轻松超过 16 个 → 触发 `pending_full_sync` → 走全屏拷贝分支，
   但该分支只刷新当前 buffer，另一块从此落后一整帧 → 交替 flip 时内容成片缺失。
   上下扫过只在少数行内产生矩形，不触发溢出，所以看起来"只有左右扫才有问题"。

**修复**：

- `m_back_buffer_offset = 0`：两份 presenter 都只用一份帧内容（wlroots 早就是这样），
  彻底消除"两个半区不同步"的前提。
- `flush_framebuffer_rects()` 的 KMS 分支默认走 `present_buffer_kms()` 全屏拷贝；
  脏区路径保留在 `SERENITY_KMS_PARTIAL_PRESENT=1` 后面，仅供压测。
- 代价（本机实测，VM 内 1872×984）：7.03 MiB / 帧，单次 memcpy **1.31 ms**，
  60 fps 下约 **7.9% 单核 / 422 MiB/s**，占 16.67 ms 帧预算的 7.9%。
  只在有脏区的帧发生，空闲桌面不提交。正确性收益远大于这点开销。

**教训**：这套合成器的脏区优化（矩形合并 + 双半区 replay）在"大量同行小矩形"的
hover 场景下不可靠；调 UI 外观（GlassWindowTheme）也救不了它。先把呈现链路做对，
再谈省带宽。

## 7. 验收

- [x] `vista`：任务栏/窗口标题栏为玻璃蓝，原生 app 面板为浅蓝白，文件管理器
      chrome 为浅色渐变（命令栏 8 个按钮、地址栏 4 个导航按钮全部可见）。
- [x] `classic`：上述元素回落为 Win2000 灰 + 经典边框，文件管理器 chrome 同步变灰，
      没有残留硬编码 Aero 色块。
- [x] 切换后重启 `serenity-desktop.service`，主题保持（持久化生效）。
- [x] 左右扫过命令栏/任务栏后连续抓帧，命令栏按钮不再消失（帧间差异 < 1e-4）。
- [ ] GTK 应用（gedit/Firefox）跟随主题 —— 主题生成脚本已跑，未逐窗截图确认。

## 8. 已知限制 / 后续

- **圆角**：`GlassWindowTheme` 用 5×5 CharacterBitmap 把窗口四角清成 `Transparent`。
  KMS/SHM 输出是不透明帧缓冲 + 矩形脏区拷贝，没有 per-pixel alpha 混合，所以这套
  圆角遮罩只有在使用 alpha 合成的 presenter 上才正确。**不要**把它改成"填桌面背景色"，
  那会在四角留下阶梯状三角形色块（本次踩过）。要么保持透明、要么整体方角，二选一。
- **半透明装饰**：`s_title_gradient` / `s_frame_colors` 带 alpha，依赖 WindowServer
  合成；`paint_taskbar()` 开头 `clear_rect(Transparent)` 是设计的一部分，不要为了
  "不透明化"而删掉——会同时破坏任务栏玻璃感和标题栏配色。
- Wine 窗口走 frameless 自绘标题栏（强制复古框的唯一例外），不受系统主题影响。
- `GlassWindowTheme` 是 Serenity 已有的「玻璃」实现，不是 Windows DWM 的逐像素复刻。
- 快速启动栏、开始菜单、任务栏的 Vista 化布局仍按各自任务推进，本文件只统一**颜色与装饰**。

## 8. 已知限制 / 后续

- Wine 窗口走 frameless 自绘标题栏（强制复古框的唯一例外），不受系统主题影响。
- `GlassWindowTheme` 是 Serenity 已有的「玻璃」实现，不是 Windows DWM 的逐像素复刻；
  如需更接近 Aero 的模糊/透明，属于 `GlassWindowTheme.cpp` 的独立改造。
- 快速启动栏、开始菜单、任务栏的 Vista 化布局仍按各自任务推进，本文件只统一**颜色与装饰**。
