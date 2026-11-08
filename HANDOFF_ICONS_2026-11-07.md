# 图标解析与快捷方式统一管理 —— 交接（2026-11-07）

> 状态：**代码完成 + 宿主编译通过 + VM 已部署并截图验证**（11-07 11:35–11:58）。
> 本轮把"只补几个扩展名"扩成了完整的类型图标体系，并修掉 wine 快捷方式误判红叉。

## 0. 一句话结论

桌面 / 文件管理器 / 开始菜单 / 快速启动以前各自写了一套图标解析，都不认识
freedesktop 图标主题名、`.ico` 和 exe 内嵌图标，所以 Linux 应用和 `.exe` 全是灰方块；
而且 **`/etc/FileIconProvider.ini` 在 VM 上压根不存在**，所以 `.zip`/`.tar`/`.md` 一律白纸。
现在统一到一个**系统层解析器**（`PortIcons`）+ 一个**统一快捷方式模型**（`PortShortcuts`），
`.desktop` 只转换一次、结果存成 `.af` 复用，并有一套 50 个图标族的扩展名类型表。

## 1. 设计决定（已和用户确认）

| 问题 | 决定 | 理由 |
|---|---|---|
| 系统层还是桌面层？ | **系统层** | 文件管理器、桌面、开始菜单、属性窗、预览窗格都要同一套规则。放进 `GUI::FileIconProvider` 的宿主分支，所有 GUI 客户端自动生效。 |
| `.af` 里内嵌图标（base64）？ | **不内嵌** | 改为导入时把图标**固化成缓存 PNG 绝对路径**写进 `Icon=`；`IconData=` 仍支持**读取**（base64），不写入。 |
| Linux `.desktop` 直接读还是转 `.af`？ | **内存转换 → 落盘保存 → 之后只读 `.af`** | 源 `.desktop` **不改不删**，结果写 `~/.local/share/serenity/shortcuts/<hash>.af`，用 `SourceStamp=mtime:size` 判断过期。 |
| 缺的图标怎么补？ | **按"图标族"补，不按扩展名单个补** | 50 个族覆盖 ~600 个模式；长尾（`.swift`/`.tf`/`.dart`…）落到 `text`，不再出白纸。 |

## 2. 解析器与模型（`Ports/LinuxGUI/Common/`，header-only）

### `AppIcons.h` —— `namespace PortIcons`

- `resolve(spec, size)`：绝对路径 / 主题名 / `.exe` / `base64:` 统一入口
- `for_shortcut(icon_spec, executable, size)`：**三级回退**
  1. 快捷方式 `Icon=`（主题名或绝对路径）
  2. `.exe` → PE 内嵌图标（`from_executable`，`.rsrc` 正解析 + PNG/ICONDIR 签名扫描，含失败缓存）
  3. **本轮新增**：`.af` 忘写 `Icon=` 时按可执行文件 basename 反查
     `<prefix>/res/icons/<size>x<size>/app-<kebab>.png`，失败再试 `app-<lowercase>`
     （`/bin/ImageViewer` → `app-image-viewer.png`；`/bin/TTPlayer` → `app-ttplayer.png`）
- `resolved_path(spec, size)`：供转换时把图标固化成绝对路径
- `invalidate_cache()`：桌面「刷新」用
- SVG 栅格化：`rsvg-convert` → `convert` → `magick` → `python3 + gi(GdkPixbuf/cairosvg)`，
  结果缓存在 `~/.cache/serenity/icons/<hash>-<size>.png`

### `AppShortcut.h` —— `namespace PortShortcuts`

- `Record`：`.af` 与 `.desktop` 归一后的唯一形状（含 `program` / `arguments` /
  `working_directory` / `icon_spec` / `source_file` / `categories` / `cache_file` / `source_stamp`）
- `split_command()`：引号、`env` 包装、`FOO=bar` 前缀、去掉 `%U/%f` 字段码
- `load_or_convert()`：`SourceStamp=mtime:size` 一致就读缓存 `.af`，否则解析并落盘
- `scan(dirs)`：后者覆盖前者；去重键统一成 `<stem>.af`
- `belongs_in_start_menu()`：过滤 `hidden/no_display/exclude_from_menu/terminal`、
  `Type!=Application`、`ConsoleOnly/TerminalEmulator/Building/Debugger`、少量内置黑名单 +
  用户可编辑 `~/.config/serenity/app-filter.ini` 的 `[Blocked]`

## 3. 文件类型图标（本轮重点）

### 3.1 类型表 `Base/etc/FileIconProvider.ini`

从 38 行扩到 **50 个图标族 / ~600 个模式**，分 7 段：源码 / 标记与文档 / 数据与配置 /
字体 / 图像 / 音视频 / 归档·包·磁盘 / 二进制。要点：

- **最具体的模式必须写在前面**：`*.tar.gz` 在 `*.tar`、`*.gz` 之前
- `.exe/.af/.desktop` **不进这张表**，它们走 `PortShortcuts`（PE 资源 / `Icon=`）
- 冲突取舍：`.obj` 仍归 `object`（不归 3D）、`.ts` 归 TypeScript、`.iso` 归 `diskimage` 不归归档
- 长尾源码（`.kt` `.swift` `.dart` `.tf` `.nix` 之外的）落到 `text`；`Makefile`/`Dockerfile`/
  `LICENSE`/`README` 也挂 `text`（用 `*Makefile` 这类带前导 `*` 的模式，Serenity 的 glob 是整串匹配）

### 3.2 新增 12 个图标

`scripts/gen-filetype-icons.py` —— 纯 Python（zlib + struct 手写 PNG 读写 + 5×7 像素字体），
白纸页 + 色标 + 三个字母，与仓库原有的 `filetype-asm.png` 完全同风格；每族出 16×16 与 32×32。

| 图标 | 标签/配色 | 覆盖 |
|---|---|---|
| filetype-zig | ZIG 橙 | `.zig .zon` |
| filetype-rust | RS 铁锈红 | `.rs` |
| filetype-go | GO 青 | `.go` |
| filetype-lua | LUA 深蓝 | `.lua` |
| filetype-config | CFG 石板灰 | `.toml .yaml .yml .conf .cfg .env .service .properties` |
| filetype-diskimage | ISO 藏蓝 | `.iso .img .qcow2 .vmdk .vdi .vhd .dmg` |
| filetype-package | PKG 红 | `.deb .rpm .apk .appimage .flatpak .snap .msi` |
| filetype-document | DOC 蓝 | `.doc .docx .odt .rtf .epub .mobi .chm` |
| filetype-log | LOG 灰 | `.log .journal .err` |
| filetype-certificate | KEY 土黄 | `.pem .crt .cer .der .key .p12 .pfx .gpg` |
| filetype-nix | NIX 蓝 | `.nix` |
| filetype-model3d | 3D 紫 | `.stl .glb .gltf .fbx .blend .dae .ply` |

（`filetype-archive.png` / `filetype-asm.png` 仓库里本来就有，但 **VM 上从未部署**，本轮一并装上了。）

### 3.3 `LibGUI/FileIconProvider.cpp` 的宿主化改造

- 配置路径：优先 `$SERENITY_PREFIX/etc/FileIconProvider.ini`，回退 `/etc/FileIconProvider.ini`
- 文件缺失**不再** `release_value_but_fixme_should_propagate_errors()` 崩掉，只退化为 `filetype-unknown`
- 匹配顺序：原来遍历 `HashMap::keys()`（跨进程不确定，`x.tar.gz` 可能命中 `*.gz`），
  改为按 ini 声明顺序的 `Vector<ByteString> s_filetype_order`

## 4. 修改的文件清单

| 文件 | 改动 |
|---|---|
| `Ports/LinuxGUI/Common/AppIcons.h` | 新增 `serenity_app_icon_path()`；`for_shortcut()` 加第三级 app 图标回退 |
| `Ports/LinuxGUI/Common/ExplorerMenus.h` | 新增 `is_windows_launcher()` / `wine_prefix_root()` / `windows_argument_to_host()` / `shortcut_status_for_record()`；`shortcut_status()` 改为薄包装 |
| `Ports/LinuxGUI/Shell/DesktopIconsWidget.cpp` | 删掉自己那份 broken 判定，改调 `shortcut_status_for_record()` |
| `Userland/Libraries/LibGUI/FileIconProvider.cpp` | 配置路径宿主化 + 缺失容错 + 匹配顺序确定化 |
| `Base/etc/FileIconProvider.ini` | 38 → 50 族 / ~600 模式 |
| `Base/res/icons/{16x16,32x32}/filetype-*.png` | 新增 12 族 × 2 尺寸 |
| `scripts/gen-filetype-icons.py` | 新增（图标生成器，可重跑） |
| `Ports/LinuxGUI/Shell/SystemMenu.cpp`、`StartMenuShortcuts.cpp`、`AppCatalog.cpp`、`Userland/Libraries/LibDesktop/AppFile.cpp`、`Common/ExplorerMenus.h`、`Userland/Applications/FileManager/DirectoryView.cpp` | 上一轮已改，本轮未动 |

## 5. VM 验证结果（11-07 11:35–11:58）

部署：`liblagom-gui.so.0.0.0`、`liblagom-desktop.so.0.0.0`、`serenity-shell`、
`serenity-file-manager` → `/serenity/{lib,bin}`；`filetype-*.png` → `/serenity/res/icons/{16,32}x{16,32}`；
`FileIconProvider.ini` → `/serenity/etc/`。

| 验证点 | 结果 |
|---|---|
| 桌面 `.exe` | `Launcher.exe` 显示真实 PE 图标（黑底白箭头） ✅ |
| 桌面 `.desktop`（主题名） | `火狐浏览器 ESR` 显示 Firefox logo ✅ |
| 桌面 `.af` 无 `Icon=` | `TTPlayer 音乐播放器`/`图像查看器` 显示 app 图标 ✅ |
| 桌面 `.zip` | `111.zip` 显示归档（拉链文件夹）✅ |
| 二次刷新不重转 | 重启前后 `shortcuts/*.af` 的 `stat` 全量 md5 **一致** ✅ |
| 坏快捷方式 | 人为 `Exec=/usr/bin/does-not-exist-xyz` → 红叉 + 灰标签 ✅ |
| wine 快捷方式 | `明日方舟：终末地`（`Exec=wine C:\...\*.lnk`）**红叉消失** ✅ |
| 文件管理器 | 42 + 21 个样本（`.zig .zon .rs .go .tar .tar.gz .gz .7z .zip .iso .qcow2 .deb .rpm .appimage .toml .yaml .env .log .docx .xlsx .pptx .pdf .flac .mkv .png .svg .glb .pem .patch .psd .woff2 .ics .ipynb .mk .kts .lnk .bat`）逐格核对，**全部命中预期图标** ✅ |
| 开始菜单 → Linux 应用 | 12 项，图标全在；htop/xterm/vim/gtk-demo 等已被过滤 ✅ |

截图：`Build/vm/` 下未存档（本轮截图走 `/tmp`）。如需留档，重跑下面的步骤即可。

## 6. 还没做

1. **桌面「刷新」时批量提示坏快捷方式**（现在只在双击时提示；红叉已能看见）
2. **图标大小联动**：`PortIcons` 缓存按 size 分键，改图标大小会重新解码；如需省内存可加 48/64 预取
3. **`.af` 的 `IconData=` 写入**：目前只读不写（按设计）
4. `systemd` 下 `SERENITY_PREFIX/etc` 与 `/etc` 双写：目前靠代码优先前缀，安装脚本尚未统一
5. 生成器目前只有 A–Z 0–9 `*` `.`，要写中文/符号标签得扩字体表

## 7. 构建 / 部署 / 验证命令

```bash
# 宿主
cd /mnt/user/serenity-linuxgui
cmake --build Build/lagom --target LibGUI LibDesktop serenity-shell serenity-file-manager -j8

# 重生成图标（改了 ICONS 表之后）
python3 scripts/gen-filetype-icons.py

# 打包图标 + 上传
tar -czf /tmp/serenity-icons.tgz -C Base/res/icons 32x32 16x16 --wildcards '*filetype-*.png'
scp -F /dev/null -i /mnt/user/serenity/Build/vm/id_ed25519 -P 2222 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    Base/etc/FileIconProvider.ini /tmp/serenity-icons.tgz \
    Build/lagom/lib/liblagom-gui.so.0.0.0 Build/lagom/lib/liblagom-desktop.so.0.0.0 \
    Build/lagom/bin/serenity-shell Build/lagom/bin/serenity-file-manager serenity@127.0.0.1:/tmp/

# VM 内安装（必须 stop → rm/install → start，否则 Text file busy）
sudo systemctl stop serenity-desktop.service
sudo install -m 644 /tmp/liblagom-gui.so.0.0.0     /serenity/lib/liblagom-gui.so.0.0.0
sudo install -m 644 /tmp/liblagom-desktop.so.0.0.0 /serenity/lib/liblagom-desktop.so.0.0.0
sudo install -m 755 /tmp/serenity-shell           /serenity/bin/serenity-shell
sudo install -m 755 /tmp/serenity-file-manager    /serenity/bin/serenity-file-manager
sudo install -m 644 /tmp/FileIconProvider.ini     /serenity/etc/FileIconProvider.ini
tar -xzf /tmp/serenity-icons.tgz -C /tmp/iconx
sudo cp -a /tmp/iconx/32x32/. /serenity/res/icons/32x32/
sudo cp -a /tmp/iconx/16x16/. /serenity/res/icons/16x16/
sudo systemctl start serenity-desktop.service

# 截图 / 点击（click 走 QMP 绝对坐标，比 poke 稳）
python3 /mnt/user/serenity/Build/vm/vncshot.py "shot:/tmp/x.ppm"
python3 /mnt/user/serenity/Build/vm/vncshot.py "click:32,968" "sleep:1" "shot:/tmp/menu.ppm"
convert /tmp/x.ppm /tmp/x.png
```

## 8. 踩过的坑

- **`/etc/FileIconProvider.ini` 在 VM 上不存在**，而代码里是
  `release_value_but_fixme_should_propagate_errors()` → 真实原因不是"表太短"而是"表根本没装"。
  任何"扩展名没图标"的排查都要先 `ls /serenity/etc/FileIconProvider.ini`。
- **`filetype-archive.png` / `filetype-asm.png` 仓库有、VM 没有** —— `Base/res` 的部署是手工 cp，
  新增图标不会自动进 VM。
- **`String::matches` 是整串 glob 匹配**：写 `Makefile` 匹配不到 `/home/x/Makefile`，必须 `*Makefile`。
- **`HashMap::keys()` 的遍历顺序**决定了哪条模式先命中；同时改 ini 顺序和遍历来源才有意义。
- **`ByteString` 没有 `.size()`**（它是 `.length()`），`-Werror` 直接编译失败。
- **`pkill -x serenity-print-settings` 永远匹配不到**：kernel `comm` 截断到 15 字符，用 `pkill -f`。
- **`systemctl set-environment` 对 unit 无效**（unit 自带 `Environment=` 不继承 manager 环境），
  指针 1:1 要么改 unit，要么加 drop-in。本轮加了
  `/etc/systemd/system/serenity-desktop.service.d/pointer.conf` 的 `SERENITY_POINTER_FLAT_ACCEL=1`。
- **`Core::command()` 在程序不存在时会 `VERIFY` abort**，SVG 栅格化必须先 `tool_on_path()` 拿绝对路径。
- `-Wwrite-strings` 是 `-Werror`：`char* p = "literal"` 必须写 `char const*`。
- header-only 头文件被 LibGUI / LibDesktop / shell / FileManager 各编译一遍；函数内 `static` 缓存
  在同一 `.so` 内共享、跨二进制各自一份（可接受）。
- 部署前必须 `stop` 服务：直接 `install` 运行中的二进制会 `Text file busy`。
