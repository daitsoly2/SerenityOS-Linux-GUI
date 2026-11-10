# GTK Serenity — 控件级接入 Serenity UI 层

这不是主题。**GTK 应用链接到改造后的 GTK 库，按钮、输入框、滚动条、复选框、菜单、
标题栏就由 Serenity 的绘制代码画出来**，应用侧零配置：不需要 `GTK_THEME`、不需要
`~/.config/gtk-4.0/gtk.css`、不需要任何环境变量、不需要重编译应用。

定制版系统，不随上游。三套上游源码直接 vendor 进本目录并做侵入式改造。

## 硬约束

| 约束 | 做法 |
|---|---|
| 公开 API 不改 | 不动任何公开头（唯一改的 `gtksettingsprivate.h` 是私有头且只有 `#define`），不动类结构体、不动导出函数签名 |
| ABI 不改 | SONAME 保持 `libgtk-3.so.0` / `libgtk-4.so.1` / `libadwaita-1.so.0`；新增绘制库**编译进** GTK，不新增 `.so`、不新增 `DT_NEEDED`；所有新符号 `visibility("hidden")` |
| 现有应用可用 | 只换库，应用无需重编；`check-abi.sh` 对比导出符号表，删除=0、内部符号泄漏=0 |
| 风格统一 | GTK3 / GTK4 / libadwaita 共用**同一个** `libserenity-ui`，配色/字体/度量全部从活动的 Serenity `SystemTheme` 实时读取 |

## 架构

```
Base/res/themes/<Name>.ini  ([Colors]/[Fonts]/[Metrics])
        │  /serenity/etc/WindowServer.ini -> [Theme] Name
        ▼
libserenity-ui         主题解析 · 颜色角色映射 · 度量 · 热重载
        │
        ├── libserenitypaint.so   C ABI 桥：直接调用**真的** Gfx::ClassicStylePainter
        │     （LibGfx 主机版），画进 Gfx::Bitmap 后把 ARGB32 像素交回 GTK。
        │     调色板来自 libserenity-ui，即桌面自己的 WindowServer.ini，无硬编码。
        │     仓库里**没有**第二份控件绘制实现（旧的 serenity-widgets.c 已删除）。
        │
        ├── GTK3   gtkrender.c 的 gtk_render_background/frame/check/option/
        │          slider/handle/arrow  → dlopen 桥，按元素名分派 kind
        │          GtkStyleContext 的 widget path 元素名用来识别控件类型
        │          另钩 gtkcssgadget.c 的 gadget 背景（menuitem/modelbutton
        │          等 gadget 控件不走 gtk_render_*）：仅当控件位于
        │          GtkMenu/GtkPopover 内时接管——idle 条目抑制 stock 背景，
        │          让面板自己的 base+stripe 透出；hover/选中画
        │          SERENITY_WIDGET_MENU_SELECTION（stripe 起点右移的高亮，
        │          同 WindowServer::Menu）；菜单内的 button 一律降级为
        │          menu item（gedit 图标行不再画成凸起按钮）
        │
        ├── GTK4   gtk_widget_create_render_node()（每个控件必经）里调用
        │          _gtk_serenity_paint()：按 CSS 节点名（trough/slider/tab/…）→
        │          GType → 继承 三级识别，用真实画笔出位图贴进 render node；
        │          gtk_css_style_snapshot_background()/border() 在原绘制前 return
        │
        └── libadwaita  全部是 GTK4 控件，自动走同一条路径；
                        adw-style-manager 固定亮色、禁动画、不探 portal

三套库各自再注入一份由 SystemTheme 生成的 CSS provider，
优先级 = APPLICATION+1（压过应用自带 CSS），统一字体/方角/无阴影/named colors。
主题切换（serenity-theme.sh vista|classic）由 revision 轮询在 1 秒内同步到所有 GTK 应用。
```

## 上游版本（`versions.env`）

| 组件 | 版本 | 来源 |
|---|---|---|
| GTK3 | **3.24.53**（`gtk-3-24` 分支 tip，2026-10-29） | GNOME/gtk GitHub（官方 tarball 停在 3.24.43） |
| GTK4 | **4.24.1** | download.gnome.org 最新稳定 |
| libadwaita | **1.10.0** | download.gnome.org 最新稳定 |

每个包都有 sha256 校验，重新同步用 `./sync.sh`。

## 目录

```
versions.env            锁定的上游版本 / URL / sha256
sync.sh                 下载+校验+解压+打补丁（可重复）
apply-serenity.py       幂等的源码改造（上游模式不匹配会直接报错，不静默丢功能）
serenity-ui/            统一 UI 桥 + 控件绘制层 + headless 预览 / 主题 dump
overlay/                三套 shim（内部符号，hidden 可见性）
vendor/                 同步来的上游源码（gitignore，可由 sync.sh 重建）
build.sh                meson 构建三套 + staging
deploy.sh               部署（私有 prefix / 替换系统库 / 推到 VM）
check-abi.sh            导出符号表 / SONAME 对比
serenity-widgets.png    控件预览（改主题后重新生成，可作视觉回归基线）
```

## 构建

```bash
cd Ports/GTKSerenity
./sync.sh --offline        # 已下载过源码时；联网同步用 ./sync.sh
./build.sh ui              # 先单独验证绘制层（不需要 GTK dev 包）
./build.sh                 # GTK4 -> GTK3 -> libadwaita + ABI 校验
```

`build.sh ui` 产出的 `serenity-widget-preview` 是无显示服务器依赖的预览（控件由
`libserenitypaint.so` 画，和 GTK 里完全同一条路径），
直接画出所有 Serenity 控件到 PNG，可在改动主题后做视觉对照。
`serenity-theme-dump --css 3|4` 打印注入 GTK 的实际 CSS。

GTK4 用 clang 编译：GTK 4.24 的 meson 会探测 clang 专有的 `-Wno-*` 告警选项，
GCC 16 把这些当硬错误。`apply-serenity.py` 同时跳过了这批仅影响告警的编译器探测，
在有批量删除保护/受限的构建环境里也能完成配置。

## 部署 / 回滚

```bash
./deploy.sh --private     # 默认：装到 /serenity/gtk + ld.so.conf.d，系统库不动
./deploy.sh --replace     # 覆盖 /usr/lib/<multiarch> 三个库（先备份到 /root）
./deploy.sh --vm          # 推 staging 到 VM 并在 VM 内执行 --private
./deploy.sh --status      # 查看应用实际解析到的库
SERENITY_GTK_LEGACY=1     # 运行时一键回到上游 Adwaita 绘制（A/B 对照用）
```

部署后 `serenitid` 需要给会话带上 `SERENITY_PREFIX=/serenity`；
`deploy.sh --private` 会写 `/etc/environment.d/50-serenity-gtk.conf`，
非桌面后代走 `dbus-update-activation-environment` 同步。

## 验证清单

1. `./build.sh ui` → `serenity-widgets.png` 显示 3D 按钮/凹陷输入框/方角/渐变标题栏。
2. `./check-abi.sh build/stage` → SONAME 不变、删除符号 0、内部符号泄漏 0。
3. VM 内 `gtk3-demo`（GTK3）与 `gnome-text-editor`（GTK4/libadwaita）截图：
   控件是 Serenity 形态，窗口套 WindowServer 复古边框，无双标题栏、无阴影圆角。
4. `serenity-theme.sh vista` / `classic` 各截一张，GTK 应用应在 1 秒内跟着换配色。
5. A/B：`SERENITY_GTK_LEGACY=1` 启动同一应用，应恢复上游外观。

## 全局主题（`theme/`）

`theme/` 是一套**系统级**主题，由 `theme/install.sh` 装到
`/usr/share/themes/Serenity/{index.theme,gtk-3.0/gtk.css,gtk-4.0/gtk.css}`，
`vm-deploy.sh` 会自动部署。它和控件级 fork 分工明确：

- **真 painter 负责外观**：按钮斜面、输入框凹陷、滚动条、菜单面板（白底+黑框+
  24px stripe）全部由 `libserenitypaint.so` 的 `LibGfx::ClassicStylePainter` 画。
- **主题 CSS 只负责 painter 表达不了的东西**：字体/字号（对齐 `WindowServer.ini`
  的 `[Fonts]`）、菜单行 min-height/padding、label 与快捷键的间距、libadwaita 的
  `@define-color` token。主题里**不写任何** bevel / gradient / box-shadow。

GTK4 的 shim 通过 `install_global_theme()` 把这份系统主题以
`APPLICATION + 2` 优先级再注入一次：libadwaita 自带 stylesheet 是 `APPLICATION`、
会盖掉 `THEME` 优先级里的排版规则，所以必须提权，才能让所有 GTK4/libadwaita
进程共享同一份全局来源。`install.sh` 会移除旧的
`~/.config/gtk-4.0/gtk.css` 局部覆盖（USER=800，会盖过一切）并备份。

GTK4 菜单面板关键约定：`popover > contents` 的 css name `contents` 属于真实 widget
`GtkPopoverContent`（不是纯 CSS 节点），因此 `create_render_node()` 钩子会执行它。
shim 把它映射为 `SERENITY_WIDGET_MENU` 由真 painter 画整个面板；CSS 侧保持
`transparent` 避免重复绘制；`panel_is_a_child_contents()` 抑制 `GtkPopover` 外壳，
防止画两遍黑框和 stripe。早期注释误以为 `contents` 不是 widget 而把它清空，
结果就是 GTK4 菜单"只剩文字浮在窗口上"。

## 与旧方案的区别

旧方案（`scripts/port/serenity-theme-gen.py` + `Ports/gtk-serenity/` 补丁系列）是
「三份手写 CSS 模板 + 覆盖 libadwaita 的 .so」，只换配色：CSS 自己画 Win2000 斜面
和灰底 popover，与真 painter 打架；且 GTK4/libadwaita 依赖用户级
`~/.config/gtk-4.0/gtk.css`（容易丢、优先级会被应用覆盖）。本方案把控件绘制本身换成
Serenity 实现，主题只留排版与 token，并统一装到系统目录，不再有用户级局部覆盖。

旧资产保留在 `Ports/gtk-serenity/`（另一工作区），不参与本构建。
