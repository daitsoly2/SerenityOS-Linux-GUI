# 打印服务与共享打印模块 —— 交接（2026-11-07）

> 状态：**已完成并在 VM 实测**（12:36–13:55）。打印走独立守护进程，应用不再自己 fork `lp`；
> **所有打印选项（方向/纸张/缩放/份数）由守护进程落实**，客户端只负责预览 + 交出原始文件。

## 0. 一句话结论

CUPS 仍然是系统打印队列；端口另加一个**独立守护进程 `serenity-print-service`**，把
"有哪些打印机 / 提交作业 / 全局默认设置（纸张·方向·缩放·份数）"封装成一个 Unix socket 服务。
**页面的排版与合成（fit/shrink/fill/original/custom、图像与 PDF）全部在服务里完成**，
应用通过 `Ports/LinuxGUI/Common/PortPrint.h` 只做「选参数 + 预览 + 交文件」，
因此打印对话框、预览、实际纸张、通知、设置只有一份实现。

## 1. 架构

```
app(ImageViewer/…)  --PortPrint::show_dialog-->  打印对话框（含预览，纯客户端渲染）
        |                                              |
        |  LIST / SETTINGS / SET / PRINT(原始路径)      | 同一份 PortPrint.h + PortPrintGeometry.h
        v                                              v
  serenity-print-service  （session 常驻，serenitid 拉起）
        |  1) 合成页面（图像直接合成；PDF 先 pdftoppm 栅格化再逐页合成）
        |  2) lp(1) 提交合成后的整页 PNG（fit-to-page 1:1 映射到纸张）
        v
  cupsd  ->  打印机
        |
        `-- org.freedesktop.Notifications（D-Bus，serenitid 起的合成器提供）
              正在打印 / 打印完成 / 打印失败
```

- socket：`$XDG_RUNTIME_DIR/serenity-print.sock`（回退 `/tmp/serenity-print-<uid>.sock`，权限 0600）
- 协议：每连接一个请求，Tab 分隔，末尾换行
  - `LIST` → `OK <count>\n<name>\n…`
  - `SETTINGS` → `OK <media>\t<orientation>\t<scaling>\t<percent>\t<fit>\t<copies>`
  - `SET <key> <value>` → 持久化并返回同样的 SETTINGS 行（key: media/orientation/scaling/percent/fit/copies）
  - `PRINT <printer> <media> <orientation> <scaling> <percent> <copies> <fit> <path>` → `OK <lp 输出>` / `ERR <消息>`
  - `STATUS` → `OK serenity-print-service/1 cups=<0|1>`
- 全局设置存 `~/.config/serenity/print.ini` 的 `[Defaults]`（Media/Orientation/Scaling/Percent/FitToPage/Copies），
  由服务读写，所以每个 app 的打印对话框打开时看到的是同一份默认值。

## 2. 页面合成（本版重点：选项在服务里实现）

`Ports/LinuxGUI/Common/PortPrintGeometry.h`（**新增，客户端与服务共用**）提供纯几何：

- `page_canvas_size(w_mm, h_mm)`：按 150 DPI 把纸张毫米换算成画布像素
- `compute_placement(image_w, image_h, canvas, scaling, percent)`：算出「是否转 90°、缩放比、目标矩形」
- `rotate_quarter_turn(bitmap)`：顺时针 90° 拷贝

服务端 `compose_one_page()` 用同一套几何把图画到**纵向纸张比例**的画布上（白底 + 4% 不可打印边距），
写临时 PNG，再交给 `lp`。方向由 `-o landscape` 让 CUPS 把整张纵向画布旋转成横向页，
所以「预览里看到的」与「实际纸张」在相对尺寸上完全一致。

缩放语义（预览与实际打印一致，**全部在服务端实现**）：

| 模式 | 行为 | 实现 |
|---|---|---|
| `fit` 适应页面 | 整张可见，可留白；自动转 90° 以放得更大 | 合成画布 + `fit-to-page` |
| `shrink` 缩小到可打印区域 | 同 fit，但**只缩不放**（小图保持小） | 同上，`scale=min(scale,1)` |
| `fill` 填充页面 | 铺满纸张、裁掉溢出边缘 | 合成画布时按 cover 缩放并裁切（CUPS 无 cover 语义） |
| `original` 原始大小 100% | 1 图像像素 = 150 DPI 下的 1 像素，超出被纸裁 | **不合成**，直接 `-o scaling=100` |
| `custom` 自定义百分比 | 10%–400% 居中，超出被纸裁 | 合成画布 + 指定比例 |

- **图像**：`Gfx::Bitmap::load_from_file` → 合成 → 临时 PNG → `lp ... -o fit-to-page`
- **PDF**：`pdftoppm -png -r 150` 逐页栅格化（poppler-utils）→ 每页 `compose_one_page` → 多文件一次性 `lp`
  （CUPS 把每个文件当一页）。无 pdftoppm 时退回直接把 PDF 交给 CUPS。
- **文本/其他不支持格式**：不做合成，按原样交 `lp`（`-o fit-to-page` / `scaling=100` 走驱动）。
- 合成/栅格化临时文件在 `lp` 返回后删除；设 `SERENITY_PRINT_KEEP_TEMP=1` 可保留到
  `$XDG_RUNTIME_DIR/serenity-print-page-*.png` 便于对照预览。

## 3. 通知（用户要求"用系统通知显示打印状态"）

- 提交成功 → `正在打印：<文件名>` / `打印机 <p> · 作业 <id>`
- `lp` 失败 → `打印失败：<文件名>` + lp 的 stderr
- 作业从 CUPS 未完成队列中消失 → `打印完成：<文件名>`（服务每 2s 轮询 `lpstat -W not-completed -o`）
- 用的就是 `org.freedesktop.Notifications`（compositor 已在 session bus 上提供），
  所以**应用不再弹自己的失败对话框**。
- 采用 libdbus 一次性连接，发完即断，不占常驻连接。

## 4. 打印对话框（`PortPrint::show_dialog`）

| 控件 | 说明 |
|---|---|
| 预览 | 白纸按纸张真实比例；4% 不可打印边距；用 `PortPrintGeometry` 与服务的合成**同一套几何** |
| 打印机 | 来自服务 `LIST` |
| 方向 | 纵向 / 横向 —— 预览纸张真的会转（210×297 → 297×210），图像按"哪种放法更满"自动转 90° |
| 纸张 | A4 / US Letter / 照片 10×15 cm / A5，映射到 `-o media=` |
| 份数 | 1–99，`-n` |
| 缩放 | 适应页面 / 缩小到可打印区域 / 填充页面 / 原始大小 100% / 自定义百分比（配 10–400% 数字框） |
| 保存为所有应用的默认设置 | 勾上则回写服务的全局默认值 |

客户端**不再合成页面**：`show_dialog` 只把原始 `path` + 选项通过 `PRINT` 发给服务。

## 5. 新增 / 修改的文件

| 文件 | 说明 |
|---|---|
| `Ports/LinuxGUI/Services/main_print_service.cpp` | 守护进程（socket + **页面合成 / PDF 栅格化** + lp/lpstat 封装 + D-Bus 通知 + 作业轮询 + settings） |
| `Ports/LinuxGUI/Services/CMakeLists.txt` | 新增 `serenity-print-service` 目标；链接 `LibCore LibGfx LibCompress AK ${DBUS_LIBRARIES}`，加 `${SERENITY_PROJECT_ROOT}` 以引用共享头 |
| `Ports/LinuxGUI/Common/PortPrintGeometry.h` | **新增**：纸张画布尺寸 / 布局计算 / 90° 旋转（客户端与服务共用） |
| `Ports/LinuxGUI/Common/PortPrint.h` | 客户端协议、Settings、print_file、预览控件、打印对话框；移除客户端合成，改发原始文件 |
| `Ports/LinuxGUI/SessionManager/serenity_manager.cpp` | 子进程表加 `print-service`，D-Bus 就绪后、fcitx5 之前 spawn |
| `Userland/Applications/ImageViewer/main.cpp` | 工具栏 + 文件菜单「打印(&P)…」（Ctrl+P），调 `PortPrint::show_dialog`；pledge 加 `proc exec` |

## 6. VM 实测（2026-11-07）

- `STATUS → OK serenity-print-service/1 cups=1`；`LIST → opttest / serenitytest`
- 服务端合成逐项核对（`SERENITY_PRINT_KEEP_TEMP=1` 取回 `serenity-print-page-*.png` 目视）：
  - **fill + portrait**：横屏截图旋转 90° 铺满 A4 纵向并裁边 ✅
  - **fit**：整张可见、自动旋转放到最大 ✅
  - **custom 45%**：按 45% 居中、不旋转 ✅
  - **original**：不合成，`-o scaling=100` 直接提交 ✅
  - **PDF（2 页）**：`pdftoppm` 栅格化后逐页合成，`lp` 收到 `2 file(s)`，第 1 页文字正常 ✅
- 全部作业提交到 `opttest` 均返回 `OK request id is opttest-*` ✅
- 截图：`Build/vm/print-dialog-*.png`、`print-notifications-2026-11-07.png`

## 7. 已知问题 / 待办

1. **ImageViewer 目前编不过**：树里 `LibGfx/Bitmap.h`、`Color.h` 的 API 重构（`NamedColor` 枚举化、
   `PNGWriter/JPEGWriter` 的 Options 新增 `icc_data`、`ErrorOr::release_value` 去 const）导致
   `ViewWidget.cpp` / `main.cpp`（早前写的那批）报错，与本次打印改动无关。要先修好这批才能跑 UI 回归。
2. 作业**失败**只在提交阶段能捕获；CUPS 侧中途失败（缺纸/卡纸）目前只表现为"作业离开队列"，
   会被当成"打印完成"。要区分就得解析 `lpstat -W completed -o` 或订阅 cupsd 事件。
3. 只支持单文件打印；多选批量（FileManager 右键）还没接。
4. 打印机属性、双面、色彩模式等 CUPS 选项未暴露（可用 `lpoptions -l` 扩展成动态选项页）。
5. 应用侧只有 ImageViewer 接了「打印」；FileManager / PDF 阅读器 / 文本编辑器还没加入口。
6. PDF 栅格化固定 150 DPI；文档类打印已够用，如需更高清晰度可让 `PRINT` 带 DPI 参数。
