# 截图 / 录屏 / 桌面通知（Linux GUI 移植）

> ## ⛔ 硬性约束：合成器只能是 WindowServer，坚决不能用 wlroots
>
> **这个移植的桌面合成器只有一个：`serenity-window-server`（KMS presenter）。**
> 绝对不允许切到 wlroots presenter，也不允许创建/保留下面这类 systemd drop-in：
>
> ```ini
> # /etc/systemd/system/serenity-desktop.service.d/wlroots.conf  —— 禁止
> [Service]
> Environment=SERENITY_PRESENTER=wlroots
> Environment=SERENITY_NO_CURSOR=1
> ```
>
> 原因（有实测依据，不是偏好）：
>
> 1. **wlroots 路径必然在鼠标移动/点击时冒黑块**。根因与修复过程记录在
>    `HANDOFF_WSWLR_COMPOSITOR.md` §0.10 / §0.11：wlroots 侧的
>    `damage_ring` + swapchain buffer-age 与 WindowServer 自己的
>    `m_flush_rects` 是两套互相矛盾的脏区判断，新 swapchain buffer 的非脏区
>    未初始化 → 黑块；同时 `BGRx8888` 的 x=0 被 GLES 当成 alpha=0 → 透明混合
>    露出 output 黑底。**KMS 路径直接扫描 XRGB、不读这个字节，所以从来没有这个症状。**
> 2. **wayland-host 会无限重启**。wlroots presenter 让 WindowServer 自己占住
>    `/run/user/1000/wayland-serenity-0`，外部 `serenity-wayland-host` 抢不到
>    lock，日志被 `unable to lock lockfile` 刷屏，最终表现为整个桌面窗口消失。
> 3. 这台 VM 的 EGL 只有 `kms_swrast`（`libEGL warning: NEEDS EXTENSION`），
>    wlroots 拿不到真 GL，只会更慢更不稳。
>
> **自检**（每次部署后都该跑）：
>
> ```bash
> grep -i "KMS presenter\|input via libinput" /tmp/serenity-session.log | tail -2
> ls /etc/systemd/system/serenity-desktop.service.d/   # 不应出现 wlroots.conf
> ```
>
> 正确输出应包含 `LinuxScreenBackend: KMS presenter on <W>x<H>` 和
> `WindowServer: input via libinput`。如果看到
> `input via embedded wlroots compositor`，说明已经被切到 wlroots，必须立刻回滚。

> 目标：把 gpurec（gpu-screen-recorder-ui）那套「录制 + 录制指示 + 截屏通知 + 全局快捷键」
> 搬到 Serenity 桌面里。gpurec 的做法是让 `gpu-screen-recorder` **活在合成器里**直接抓帧；
> 这里同理 —— 抓帧在 WindowServer 内部完成，UI 只是一个遥控器。

---

## 1. 三条链路一览

```
┌──────────────────────────── WindowServer（Serenity 桌面合成器进程）────────────────────────────┐
│                                                                                              │
│  Compositor 前缓冲 ──► Recorder ──(TCP 裸帧)──► 宿主 serenity-record-host.py ──► ffmpeg ──► mp4 │
│        │                 │                                                    │              │
│        │                 ├──► PNGWriter ──► ~/Pictures/screenshot-*.png        │              │
│        │                 └──► 录制胶囊 Overlay（顶部居中 REC 00:12）            │              │
│        │                                                                      │              │
│        └──► NotificationCenter ──► 通知卡片 Overlay（右上角）◄── D-Bus org.freedesktop.Notifications
│                                                                                              │
│  WindowManager::process_key_event ──► 全局快捷键（PrintScreen / Super+Shift+S/V/P）            │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
                                        ▲
                                        │ /tmp/serenity-recorder.sock（文本行协议）
                                        │
                             serenity-screenshot（Screenshot & Recorder 面板）
```

为什么录制要拆到宿主编码：**guest 镜像里没有 ffmpeg**，而用户要求不往 VM 里装。
QEMU user-mode 网络把 `10.0.2.2` 映射到宿主回环，所以裸帧直接走 TCP 出去。

---

## 2. 文件清单

| 文件 | 作用 |
| --- | --- |
| `Ports/LinuxGUI/Recorder.h` / `Recorder.cpp` | 录制引擎：抓帧、缩放、TCP 推流、控制 socket、REC 胶囊 Overlay |
| `Ports/LinuxGUI/Notifications.h` / `Notifications.cpp` | 合成器级通知中心：卡片 Overlay + D-Bus `org.freedesktop.Notifications` 服务端 |
| `Ports/LinuxGUI/ScreenshotApp/main.cpp` | `serenity-screenshot`（Screenshot & Recorder 面板），只做 UI + 控制协议客户端 |
| `Ports/LinuxGUI/main_linux.cpp` | 启动时调用 `NotificationCenter::initialize()` / `Recorder::initialize()` |
| `Ports/LinuxGUI/CMakeLists.txt` | 加入上述源文件，链接 `dbus-1`，定义 `SERENITY_LINUX_GUI_PORT=1` |
| `Ports/LinuxGUI/Apps/CMakeLists.txt` | `serenity-screenshot` 目标（替换上游 Screenshot app） |
| `Ports/LinuxGUI/Apps/generated-apps.cmake` | 移除自动生成的上游 Screenshot 目标 |
| `scripts/port/gen-app-targets.py` | 生成器里把 `Screenshot` 列入「由 Apps/CMakeLists.txt 显式构建」 |
| `scripts/port/serenity-record-host.py` | **宿主**编码守护进程（ffmpeg 封装 + 回传 guest） |
| `Userland/Services/WindowServer/Compositor.h/.cpp` | 新增 `front_bitmap_for_recorder()`，给录制器读前缓冲 |
| `Userland/Services/WindowServer/WindowManager.cpp` | 全局快捷键（`#if SERENITY_LINUX_GUI_PORT` 保护，不影响原生构建） |

上游 `Userland/Applications/Screenshot/` 一行未改，仍可用于原生 Serenity 构建。

---

## 3. 快捷键

在 `WindowManager::process_key_event()` 里，**早于**窗口分发处理，所以任何窗口有焦点都生效：

| 快捷键 | 动作 |
| --- | --- |
| `PrintScreen` | 全屏截图 |
| `Super+Shift+S` | 全屏截图 |
| `Super+Shift+V` | 开始 / 停止录制 |
| `Super+Shift+P` | 暂停 / 继续录制 |

> `PrintScreen` 依赖输入层能把它映射出来；`Super+Shift+*` 是保底组合，已验证可用。

---

## 4. 控制协议（`/tmp/serenity-recorder.sock`）

Unix stream socket，**一行一条命令**，回一行结果。权限 0600（属主 serenity）。

| 命令 | 返回 |
| --- | --- |
| `status` | `status <idle\|recording\|paused> <已录秒数> <fps> <最后输出路径>` |
| `start` / `stop` / `toggle` | 同上（执行后的状态） |
| `pause` / `resume` | 同上 |
| `screenshot` | 同上（`screenshot` 后路径字段是 PNG 路径） |
| `set fps <1-120>` | 同上 |
| `set dir <路径>` | 同上 |
| `toast <文本>` | `ok toast`（发一条桌面通知，调试用） |
| `ping` | `ok pong` |

手动测试：

```bash
echo status | socat - UNIX-CONNECT:/tmp/serenity-recorder.sock
```

**注意**：这个 socket 的读回调跑在 WindowServer 事件循环上，客户端 socket 必须设为非阻塞，
且关闭时用 `Core::deferred_invoke` 延迟移除 —— 否则会在回调内部销毁正在执行的
`AK::Function`，触发 `may_defer || !called_from_inside_function` 断言，**整个合成器崩溃重启**。
这两个坑都踩过了，改代码时别退回去。

---

## 5. 宿主编码守护进程

```bash
./scripts/port/serenity-record-host.py            # 前台
./scripts/port/serenity-record-host.py --verbose  # 打印帧数
```

启动后监听 `127.0.0.1:9099`。guest 连上后先发一行头：

```
SERENITYREC 1 <宽> <高> <fps> <文件名>\n
```

随后是每帧 `宽*高*4` 字节（`bgr0`，自上而下，无行填充）。断开即结束录制。

处理流程：读裸帧 → 管道喂 `ffmpeg`（libx264 / veryfast / crf 20 / yuv420p / +faststart）
→ 编码完用 `scp` 把 mp4 推到 guest 的 `~/Videos/`，桌面上就能看到文件。

可用环境变量：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `SERENITY_REC_PORT` / `--port` | `9099` | 监听端口（要和 guest 端一致） |
| `SERENITY_REC_BIND` | `127.0.0.1` | 监听地址 |
| `SERENITY_REC_DIR` | `~/Videos` | 宿主暂存目录 |
| `SERENITY_REC_SSH_KEY` | `<repo>/Build/vm/id_ed25519` | 回传用的私钥 |
| `SERENITY_REC_SSH_PORT` | `2222` | guest ssh 端口 |
| `SERENITY_REC_SSH_TARGET` | `serenity@127.0.0.1` | guest ssh 目标 |
| `SERENITY_REC_VM_DIR` | `/home/serenity/Videos` | 回传目录 |
| `SERENITY_REC_CRF` / `SERENITY_REC_PRESET` | `20` / `veryfast` | x264 参数 |

guest 侧（WindowServer 进程环境）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `SERENITY_RECORDER_HOST` | `10.0.2.2` | 宿主地址 |
| `SERENITY_RECORDER_PORT` | `9099` | 宿主端口 |
| `SERENITY_RECORDER_FPS` | `30` | 目标帧率 |
| `SERENITY_RECORDER_WIDTH` | `1152` | **采集宽度上限**（见下节） |
| `SERENITY_RECORDER_DIR` | `~/Videos` | 录制文件落点（guest 视角） |

---

## 6. 性能：为什么默认 1152 宽

QEMU user-mode 网络（SLIRP）实测 **VM→宿主约 129 MB/s**（`dd`/`sendall` 实测）。
1080p 裸帧 7.3 MB/帧，30fps 需要 220 MB/s —— **物理上不可能**。

所以 `Recorder::on_frame_tick()` 在合成器里先做一次 bilinear 缩放到
`SERENITY_RECORDER_WIDTH`（默认 1152，高度按比例并向下取偶），再推流：

| 采集宽度 | 单帧 | 30fps 需求 | 实测 |
| --- | --- | --- | --- |
| 1866（原生） | 7.3 MB | 220 MB/s | ~10 fps（不可用） |
| 1280 | 3.5 MB | 103 MB/s | ~23 fps |
| **1152** | **2.8 MB** | **84 MB/s** | **~29 fps** ✅ |

另外 ffmpeg 侧加了 `-use_wallclock_as_timestamps 1` + `-fps_mode cfr`：
**即使掉帧，输出时长也和真实录制时长一致**（不会出现 8 秒录成 2 秒的「快进视频」）。

想要更高分辨率就调大 `SERENITY_RECORDER_WIDTH`，代价是帧率下降。
真正的解法是换 virtio-vsock / 共享内存通道，或把编码器放进 guest —— 见第 9 节。

---

## 7. 通知系统

### 7.1 卡片长什么样

- 位置：右上角，向下堆叠；宽 260px，圆角 8，深色不透明底
- 内容：应用名（暗灰小字）/ 标题（亮白）/ 正文（浅灰，最多 4 行，自动换行）
- 左侧 3px 强调条：普通蓝色，`urgency=2`（critical）红色
- 默认 4 秒自动消失（调用方可指定 `expire_timeout`，上限 30 秒；`0` 表示常驻）

录制状态胶囊是**另一个** Overlay：顶部居中、`REC 00:12` + 红点（暂停时变琥珀色 + `PAUSE`）。
两者分开是为了不和通知卡片抢位置。

### 7.2 D-Bus 接口

WindowServer 在 session bus 上申请 `org.freedesktop.Notifications`，对象路径
`/org/freedesktop/Notifications`，实现：

- `Notify(app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout) -> id`
  - `replaces_id != 0` 时原地替换
  - `hints.urgency`（byte）会映射成强调条颜色
  - `actions` 目前忽略（返回的 id 仍可被 `CloseNotification` 关掉）
- `CloseNotification(id)`
- `GetCapabilities() -> ["body", "persistence"]`
- `GetServerInformation() -> ("Serenity Notification Server", "SerenityOS Linux GUI port", "1.0", "1.2")`
- `org.freedesktop.DBus.Introspectable.Introspect`（返回最小可用 XML）
- 信号：`NotificationClosed(id, reason)`、`ActionInvoked(id, action_key)`（后者目前不会发）

libdbus 没有自带事件循环，所以用一个 50ms 的 `Core::Timer` 调
`dbus_connection_read_write(conn, 0)` + `dbus_connection_dispatch()` 泵消息 —— 两个系统调用，开销可忽略。

**冷启动竞态**：session bus 由桌面会话拉起，WindowServer 可能先起来。
`try_connect()` 失败会每秒重试，直到 `/run/user/<uid>/bus` 出现。

### 7.3 验证

```bash
# 任何 Linux 应用（GTK/Qt/浏览器/notify-send）都能用
gdbus call --session --dest org.freedesktop.Notifications \
  --object-path /org/freedesktop/Notifications \
  --method org.freedesktop.Notifications.Notify \
  "Firefox" 0 "" "下载完成" "ubuntu-24.04.iso 已下载完成" "[]" "{}" 8000

# 桌面自己发（走控制 socket）
echo "toast hello" | socat - UNIX-CONNECT:/tmp/serenity-recorder.sock
```

截图 / 录制的提示走的是同一个系统：
`Screenshots / Screenshot saved / <路径>`、`Screen Recorder / Recording started` 等。

---

## 8. 构建与部署

```bash
# 1) 构建
cmake --build Build/lagom --target serenity-window-server serenity-screenshot -j$(nproc)

# 2) 推到 VM 并原子替换
scp -F /dev/null -i Build/vm/id_ed25519 -P 2222 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    Build/lagom/bin/serenity-window-server Build/lagom/bin/serenity-screenshot serenity@127.0.0.1:/tmp/

ssh -F /dev/null -i Build/vm/id_ed25519 -p 2222 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null serenity@127.0.0.1 \
    'sudo install -m755 /tmp/serenity-window-server /serenity/bin/.ws.new &&
     sudo mv -f /serenity/bin/.ws.new /serenity/bin/serenity-window-server &&
     sudo install -m755 /tmp/serenity-screenshot /serenity/bin/.ss.new &&
     sudo mv -f /serenity/bin/.ss.new /serenity/bin/serenity-screenshot &&
     sudo systemctl restart serenity-desktop.service'

# 3) 宿主编码守护进程（宿主机上跑，不是 VM 里）
./scripts/port/serenity-record-host.py --verbose
```

构建依赖：宿主需要 `libdbus-1-dev`（`pkg-config --exists dbus-1`）。
**VM 里不需要装任何东西**（没有 ffmpeg 也行）。

部署后自检：

```bash
ls -l /tmp/serenity-recorder.sock                       # 控制 socket 在
grep -i "Notifications\|Recorder" /tmp/serenity-session.log
gdbus call --session --dest org.freedesktop.Notifications \
  --object-path /org/freedesktop/Notifications \
  --method org.freedesktop.Notifications.GetServerInformation
```

---

## 9. 已知限制 / 后续

1. **帧率受 SLIRP 带宽限制**，默认靠降到 1152 宽换 30fps。彻底解法：
   virtio-vsock、9p/virtiofs 共享目录、或把 ffmpeg 塞进 guest 走本地管道。
2. **录制胶囊会被录进视频**（和 gpurec 一样，它也是靠延迟显示通知规避）。
   要彻底排除，得让 `front_bitmap_for_recorder()` 返回「不含 Overlay」的缓冲。
3. **区域截图没做**。上游 `shot -r` 那条路在本移植里从来没构建过（`/bin/shot` 不存在），
   现在的 Screenshot 面板是全新的，区域选择需要额外做一个「冻结帧 + 框选」的模态层。
4. **多屏**只处理 `Screen::main()`。
5. **暂停**是「不发帧」，所以暂停期间不占时长（符合预期），但也没有断点续录的 GOP 处理。
6. **通知没有点击交互 / 不响应 actions**，也没有通知中心历史。
7. 面板 UI 的 GroupBox 高度是写死的，中文长文本下会略挤（功能不受影响）。

---

## 10. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| 面板显示 “WindowServer recorder is not reachable” | 控制 socket 不存在 → WindowServer 没起或版本旧 |
| 录制开始后立刻停止，提示 backend is not running | 宿主 `serenity-record-host.py` 没跑，或端口不是 9099 |
| 录出来的文件在宿主 `~/Videos` 而不是 guest | `scp` 回传失败（密钥/端口），日志里会有说明 |
| 视频很短但录了很久 | 检查 ffmpeg 参数里 `-use_wallclock_as_timestamps` / `-fps_mode cfr` 是否还在 |
| 整个桌面卡死 / 所有 app 崩溃 | 控制 socket 客户端被设成阻塞，或回调内同步销毁了 Function（见第 4 节） |
| 通知卡片不出现，但 `gdbus` 返回了 id | 检查 session bus 是否已就绪；`/tmp/serenity-session.log` 里找 `Notifications:` |
