# WlrootsHost × winit 原生 Wayland 崩溃修复 + 输入法（text-input）通路

> 日期：2026-11-07  
> 状态：**🟡 第一阶段已完成并部署验证（winit 崩溃已修）；第二阶段（输入法 text-input 通路）根因已定位、方案已定、代码未完成**  
> 涉及文件：`Ports/LinuxGUI/WlrootsHost/wlroots_proto.c`（唯一改动文件）  
> 相关文档：`STATUS.md`、`WAYLAND_ROADMAP.md`、`HANDOFF_WSWLR_COMPOSITOR.md`、`.workbuddy/memory/2026-11-07.md`

---

## 一页速览

| 项目 | 结论 |
|---|---|
| winit 0.30 原生 Wayland 启动崩溃 | ✅ **已修 + 已部署 + 已验证**（断言消失，窗口正常出现） |
| 崩溃根因 | `wlr_xdg_surface_schedule_configure` 断言 `surface->initialized` |
| 真正触发点 | **不是** toplevel 首次 commit 路径，而是 **xdg-decoration 的 `set_mode`**（客户端在首 commit 前就请求 CLIENT_SIDE） |
| 第一版修复为何无效 | 修的是 `on_surface_commit` 的首次 configure（idle 推迟），而断言来自 `on_new_decoration` / `on_decoration_request_mode` 的同步 `set_mode` |
| 修复手法 | `set_mode` 推迟到 idle，`!initialized` 就再排一轮；deco 销毁时置 dead 防悬垂 |
| 部署后桥进程 | PID 随 serenitid 重启而变；**不能靠 `pkill -x`**（见 §6 坑位） |
| **「不能输入」的根因** | 🔴 **桥完全没有实现 `zwp_text_input_v3` / `zwp_input_method_manager_v2`** |
| gedit 为何也不行 | `GDK_BACKEND=wayland,x11` → GTK 走 Wayland；`GTK_IM_MODULE=fcitx` → 装的是 fcitx5 原生 GTK 模块，Wayland 下需要 `zwp_input_method_v2`，桥没有 → 键被吞 |
| WordCraft 为何也不行 | egui/winit **不做 raw key→字符翻译**，文本只从 `zwp_text_input_v3.commit_string` 来 → 桥无该协议 → 一个字符都进不去 |
| 为什么「输入法明明打通了」 | 打通的是**原生 Serenity 应用**那条路（`/tmp/serenity-ime.sock` + `serenityime` addon），**Linux 客户端走的是另一条路**，两者互不相通 |
| 下一步 | 在桥里实现 `text-input-v3` + `input-method-v2`，让 fcitx5 当 IM（方案与理由见 §5） |

---

## 1. 背景：这次要做的事

把 `https://github.com/storytold/wordcraft`（纯 Rust 干净室重写的 MS Word）接进 Serenity Linux GUI。

**范围约定（早前已确定，勿变）**：

- 只有**窗口协议**走 WindowServer；**不重写** WordCraft 的 UI 到 LibGUI。
- 外部 Linux 应用通过 `serenity-wlroots-host`（`Ports/LinuxGUI/WlrootsHost/`）接入：
  - 原生 Wayland：socket `wayland-serenity-0`（`XDG_RUNTIME_DIR=/run/user/1000`）
  - X11：Xwayland `:100`
- 62 个 app 装在 `/serenity`；WS 核心尽量不动。

---

## 2. 第一阶段：winit 原生 Wayland 崩溃（✅ 已修 + 已部署 + 已验证）

### 2.1 症状

`artcraft-launcher`（winit 0.30 + egui）在原生 Wayland 下启动即崩：

```
serenity-wlroots-host: types/xdg_shell/wlr_xdg_surface.c:168:
    wlr_xdg_surface_schedule_configure: Assertion `surface->initialized' failed.
[2026-11-07 17:46:22] wayland-host exited (status 0, signal 6)   # SIGABRT
# 客户端侧：
Io error: Broken pipe (os error 32)
```

### 2.2 根因链

1. `wlr_xdg_surface_schedule_configure()` 里 `assert(surface->initialized)`（`wlr_xdg_surface.c:168`）。
2. `initialized` 只在 **`xdg_surface_role_commit`** 里被置真（`wlr_xdg_surface.c:327`），而角色 commit 在 `wlr_compositor.c:563` 执行、**早于** `:566` 的 `events.commit` 派发。
3. winit 0.30 的首帧请求顺序：
   ```
   get_xdg_surface → get_toplevel → get_toplevel_decoration
   → set_mode(CLIENT=1) → set_opaque_region → set_app_id → set_title
   → set_min_size(720,500) → set_max_size(0,0) → wl_surface.commit → wl_display.sync
   ```
   注意 **`set_mode` 发生在首次 `wl_surface.commit` 之前**。
4. 桥在 `on_new_decoration` / `on_decoration_request_mode` 里**同步**调
   `wlr_xdg_toplevel_decoration_v1_set_mode(deco, SERVER_SIDE)`，
   而它内部（`wlr_xdg_decoration_v1.c:57`）就是
   `wlr_xdg_surface_schedule_configure(decoration->toplevel->base)` → **此时 `initialized` 还是假 → 断言 abort**。

> ⚠️ **教训**：第一版修复改错了位置。当时把 `on_surface_commit` 里的首次 configure 改成 idle 推迟
> （新增 `schedule_toplevel_configure_idle`），重新编译部署后**断言一字不变**——
> 因为断言根本不走那条路径。**下次遇到 wlroots 断言，先用 backtrace/调用点确认是哪条路径，别急着改。**

### 2.3 真正的修复（已在 `wlroots_proto.c`）

把 decoration 的 `set_mode` 推迟到 idle，并处理两种触发源：

```c
struct deco_defer {
    struct wlr_xdg_toplevel_decoration_v1* deco;
    struct wlroots_proto* proto;
    struct wl_listener deco_destroy;
    bool dead;
};

static void schedule_decoration_mode_idle(void* data)
{
    struct deco_defer* d = data;
    if (d->dead) { free(d); return; }                 /* deco 已销毁 */
    struct wlr_xdg_surface* xdg = d->deco->toplevel->base;
    if (!xdg->initialized) {
        /* 首 commit 还没到，再排一轮 */
        wl_event_loop_add_idle(wl_display_get_event_loop(d->proto->display),
            schedule_decoration_mode_idle, d);
        return;
    }
    wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_list_remove(&d->deco_destroy.link);
    free(d);
}
```

`on_new_decoration` 与 `on_decoration_request_mode` 都改走
`request_server_side_decoration(p, deco)`（挂 destroy listener + 排 idle）。
`decoration_state` 增加了 `proto` 字段（`on_decoration_request_mode` 需要它拿 `p->display`）。

### 2.4 验证证据（部署后）

会话日志 `/tmp/serenity-session.log`：

```
10155:ws-proto: new wl_surface #14 pid=30827
10156:ws-proto: new wl_surface #16 pid=30827
10157:ws-proto: new xdg_surface role=0 surf=#16
10158:ws-proto: on_new_toplevel entered toplevel=0x55888895bd70 base=0x5588888b1910 role=1
10159:ws-proto: xdg_surface configure serial=41
10160:ws-proto: xdg_surface configure serial=43
10213:ws-proto: xdg_surface configure serial=67
10214:ws-proto: xdg_surface configure serial=74
```

- 客户端跑在 **Wayland** 后端：`winit::platform_impl::linux::wayland::window` 警告行；
- **无断言、无 Broken pipe、无 `wayland-host exited`**；
- 桥进程 **PID 29829 一直存活**，未重启；
- configure 发出并被 ack（serial 41→43→67→74），客户端进入主循环（ureq 网络请求持续输出）。

对照旧行为（同一份日志里 17:46:22 那次）：

```
serenity-wlroots-host: types/xdg_shell/wlr_xdg_surface.c:168: ... Assertion `surface->initialized' failed.
[2026-11-07 17:46:22] wayland-host exited (status 0, signal 6)
```

---

## 3. 部署流程（可复用，已验证）

```bash
# 1) 编译（lagom/ninja，约 5s）
cd /mnt/user/serenity-linuxgui && ninja -C Build/lagom serenity-wlroots-host

# 2) 传到 VM 的 /tmp（/serenity/bin 是 root 的，serenity 用户写不进去）
sshpass -p serenity rsync -az \
  -e "ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  Build/lagom/bin/serenity-wlroots-host serenity@127.0.0.1:/tmp/serenity-wlroots-host.new

# 3) 落位 + 重启（serenitid 会自动拉起）
sshpass -p serenity ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  serenity@127.0.0.1 '
    sudo -n mv -f /tmp/serenity-wlroots-host.new /serenity/bin/serenity-wlroots-host
    for p in /proc/[0-9]*; do
      x=$(readlink "$p/exe" 2>/dev/null)
      case "$x" in *serenity-wlroots-host*) kill "$(basename "$p")";; esac
    done
  '
```

> 🚨 **必须用 `case` 通配而不是 `readlink` 全等比较**。
> `mv -f` 覆盖后，老进程持有的是**已删除的 inode**，`readlink` 返回
> `/serenity/bin/serenity-wlroots-host (deleted)` —— **带 ` (deleted)` 后缀**，
> 全等比较匹配不到，桥不会被重启（我第一次就踩了，日志显示 "no running bridge found"）。
> 用 `case "$x" in *serenity-wlroots-host*)` 才能同时命中「活文件」与「已删除」两种。

---

## 4. 第二阶段：输入法打不通（🔴 根因已定位，代码未写）

### 4.1 用户现象

> 「我现在打开了 wordcraft，但是为什么不能输入？明明输入法是打通了的啊，gedit 也不行了」

### 4.2 关键事实（全部来自 VM 实测）

| 事实 | 出处 |
|---|---|
| 桥对外只暴露 `wl_compositor(v6)` / `wl_shm` / `xdg_wm_base(v6)` / `wl_seat` / `wl_data_device_manager` / `zxdg_decoration_manager_v1` / `wl_subcompositor` | `wlroots_proto.c` 的 `wlroots_proto_create()` |
| **全仓库没有任何 `zwp_text_input_manager_v3` / `zwp_input_method_manager_v2` 实现** | `grep -rlE "text_input_manager_v3\|input_method_manager_v2" Ports/` → 空 |
| `GDK_BACKEND=wayland,x11` | 桥进程环境 + 全部 app 环境 |
| `GTK_IM_MODULE=fcitx`（**不是 `serenity`**） | `serenity_manager.cpp:650` `setenv("GTK_IM_MODULE", "fcitx", 0)` |
| `im-serenity.so`（GTK IM 模块）**根本没编译、没部署** | VM 全盘 find 只有源码 |
| `serenityime.so`（fcitx5 addon）**没装到 fcitx5 插件目录**（只有 `/usr/share/fcitx5/addon/serenityime.conf`） | VM find |
| fcitx5 进程活着，且已有 `program:serenity frontend:serenity` 的 input context | `fcitx5-diagnose` |
| `/tmp/serenity-ime.sock` 存在 | VM |
| fcitx5 的 Wayland IM 前端是 `libwaylandim.so`，**同时**使用 `zwp_input_method_v2` 和 `zwp_input_method_keyboard_grab_v2`（含 "Failed to grab keyboard" 字符串） | `strings`/`nm` |
| WordCraft 是 **egui/winit** AppImage；gedit 是 **GTK3** | 进程 cmdline |

### 4.3 根因

系统里有**两条互不相通**的输入法路径：

```
【路径 A：原生 Serenity 应用】——「输入法打通了」指的就是这条
  LibGUI TextEditor → GUI::InputMethod → /tmp/serenity-ime.sock
                   → serenityime addon（连 libFcitx5Core）→ fcitx5 引擎(rime)
  （文档：Ports/LinuxGUI/IMEFrontend/README.md）

【路径 B：Linux 客户端（本问题所在）】
  GTK/egui 应用 → 走 Wayland 协议 → 桥 → ？？？
  桥没有 text_input_v3 / input_method_v2 ⇒ 文本永远送不到应用
```

- **gedit**：GTK3 + `GDK_BACKEND=wayland,x11` ⇒ 走 Wayland；`GTK_IM_MODULE=fcitx` ⇒ 装的是
  fcitx5 自带 GTK 模块，它在 Wayland 下要靠合成器的 `zwp_input_method_v2` 才能拿到按键。
  桥没有 ⇒ IM context 建不起来 ⇒ **且 fcitx 模块会吞掉按键** ⇒ 表现为「完全打不出字」。
- **WordCraft（egui）**：egui **不做 raw key→字符翻译**，文本只从
  `winit::Event::Ime(Ime::Commit)` 来，即 `zwp_text_input_v3.commit_string`。
  桥没有该协议 ⇒ **一个字符都进不去**，与 fcitx5 无关。

> 💡 旁证：`WL_SEAT_CAPABILITY_KEYBOARD` 是有的、keymap 也建了、evdev 转换表
> (`serenity_key_to_evdev`) 完整 ⇒ **不是「键没送到」的问题，是「送到了但没人把字符翻译出来」**。

### 4.4 顺带发现

`ports/LinuxGUI/IMEFrontend/gtk3/im-serenity.c` 里的 GTK IM 模块设计得很好
（让 GTK 应用也走路径 A、候选窗复刻 Serenity 视觉），但**从未编译部署**，而且它的注释写着
「The addon accepts exactly one client」——**serenityime socket 同时只接受一个客户端**，
这决定了下面的方案取舍。

---

## 5. 方案决策（已定，别推翻）

### ✅ 选定：桥实现 `zwp_text_input_v3` + `zwp_input_method_manager_v2`，fcitx5 当 IM

**理由**：

1. wlroots 0.20 **已经内置**这两个协议的实现，不用自己写协议代码：
   - `wlroots/types/wlr_text_input_v3.c`、`wlroots/types/wlr_input_method_v2.c`
   - `wlroots/protocol/input-method-unstable-v2.xml`（在仓库里）
   - 头文件需 `-DWLR_USE_UNSTABLE` —— **`wlroots_proto.c` 已经有了**
     （`Ports/LinuxGUI/WlrootsHost/CMakeLists.txt:84`）
   - vendored `libwlroots-0.20.so` **已导出**全部所需符号（`nm -D` 确认）
2. **不与路径 A 冲突**：fcitx5 作为 IM 直连桥；`/tmp/serenity-ime.sock` 仍只服务原生 Serenity 应用，
   没有「一个 socket 只能一个客户端」的问题。
3. **一次治好两类应用**：GTK（fcitx5 原生 GTK 模块走 input-method-v2）和 egui（走 text-input-v3）都通。
4. 符合项目「协议走 WindowServer」的定位。

### ❌ 备选一：GTK 应用改用 `im-serenity.so`（`GTK_IM_MODULE=serenity`）
只治 gedit，治不了 egui/WordCraft；且与桥方案抢 serenityime socket 的唯一客户端位。

### ❌ 备选二：桥自己当 serenityime socket 的客户端
要重写一套 socket 行协议 + 候选窗；且和 `im-serenity.so` 抢唯一客户端位。

### ⚠️ 已知难点：键盘 grab 的独占性

`input-method-unstable-v2.xml` 原文：

> The compositor should send all keyboard events on the seat to the grab holder via the returned
> wl_keyboard object. Nevertheless, the compositor may decide not to forward any particular event.
> **The compositor must not further process any event after it has been forwarded to the grab holder.**

即：一旦转给 IM 的 grab，就**不能再**发给客户端 ⇒ 字母只能经 IM 组字后由
`commit_string` 回灌（不会重复），但 **Ctrl+C / 方向键 / F 键等快捷键可能丢**。
**这一段必须实测验证**，见 §6.4。

---

## 6. 待办（代码级，可直接接着写）

> 当前 `wlroots_proto.c` 的改动状态：**decoration 修复已落地并部署**；
> **IME 部分只加了 2 行 include，其余未写**。

### 6.1 已改

```c
#include <wlr/types/wlr_input_method_v2.h>   // ← 已加
#include <wlr/types/wlr_text_input_v3.h>     // ← 已加
```

### 6.2 `struct wlroots_proto` 要加的字段

```c
struct wl_listener new_text_input;        /* text_input_manager->events.new_text_input */
struct wl_list text_inputs;               /* text_input_node */
struct wl_listener new_input_method;      /* im_manager->events.new_input_method */
struct wl_listener im_commit;             /* 注意：不能用 new_input_method 做 container_of 基准 */
struct wl_listener im_grab;
struct wl_listener im_destroy;
struct wlr_input_method_v2* input_method;      /* 当前接入的 fcitx5 */
struct wlr_text_input_v3* focused_text_input;  /* 当前获得文本输入的客户端 */
```

> ⚠️ **container_of 铁律**：`im_commit` / `im_grab` / `im_destroy` 是挂在
> `im->events.*` 上的 listener，**不属于 `struct wlroots_proto`**，
> 必须各自用 `wl_container_of(listener, p, im_commit)` 等**对应字段**，
> 用 `p->new_input_method` 会算出野指针。

### 6.3 要写的函数

```c
struct text_input_node {
    struct wlroots_proto* proto;
    struct wlr_text_input_v3* ti;
    struct wl_listener commit;   /* ti->events.commit → 转发 surrounding 给 IM */
    struct wl_listener destroy;
    struct wl_list link;
};

static struct wlr_text_input_v3* text_input_for_client(struct wlroots_proto*, struct wl_client*);
static void on_new_text_input(struct wl_listener*, void*);
static void on_text_input_commit(struct wl_listener*, void*);   /* surrounding → IM */
static void on_text_input_destroy(struct wl_listener*, void*);
static void on_new_input_method(struct wl_listener*, void*);
static void on_input_method_commit(struct wl_listener*, void*);  /* IM → text_input_v3 */
static void on_input_method_grab(struct wl_listener*, void*);    /* set_keyboard(grab, p->keyboard) */
static void on_input_method_destroy(struct wl_listener*, void*);
```

**`on_input_method_commit` 转发逻辑**（IM 组好字 → 客户端）：

```c
struct wlr_input_method_v2_state* st = &p->input_method->current;
struct wlr_text_input_v3* ti = p->focused_text_input;
wlr_text_input_v3_send_preedit_string(ti, st->preedit.text ? st->preedit.text : "",
    st->preedit.cursor_begin, st->preedit.cursor_end);
if (st->commit_text)
    wlr_text_input_v3_send_commit_string(ti, st->commit_text);
if (st->delete.before_length || st->delete.after_length)
    wlr_text_input_v3_send_delete_surrounding_text(ti,
        st->delete.before_length, st->delete.after_length);
wlr_text_input_v3_send_done(ti);
```

**setup 里要加**（`wlroots_proto_create()`）：

```c
wl_list_init(&p->text_inputs);
p->text_input_manager = wlr_text_input_manager_v3_create(p->display);
p->text_input_manager->events.new_text_input.notify = on_new_text_input;
wl_signal_add(&p->text_input_manager->events.new_text_input, &p->new_text_input);

p->im_manager = wlr_input_method_manager_v2_create(p->display);
p->im_manager->events.new_input_method.notify = on_new_input_method;
wl_signal_add(&p->im_manager->events.new_input_method, &p->new_input_method);
```

**要改的既有函数**：

- `wlroots_proto_keyboard_enter(p, surface)` —— 追加文本输入焦点处理：
  用 `wl_resource_get_client(surface->resource)` 找对应 `text_input`，
  `wlr_text_input_v3_send_enter(ti, surface)`（要求 `ti->current_enabled`），
  并 `wlr_input_method_v2_send_activate(p->input_method)`；
  焦点换走时要先 `send_leave` + `send_deactivate`。
- `wlroots_proto_keyboard_leave(p)` —— 追加 `send_leave` + `send_deactivate`。
- `wlroots_proto_keyboard_key(...)` —— **键路由**：
  - 若 `p->input_method && p->input_method->keyboard_grab` ⇒
    `wlr_input_method_keyboard_grab_v2_send_key(grab, time, code, state)`（**不**再发给 seat）；
  - 否则照旧 `wlr_seat_keyboard_notify_key(...)`；
  - modifiers 始终额外发给 seat（保证客户端能判 Ctrl/Shift），grab 激活时也发一份给 grab。

### 6.4 必须实测验证的点

1. **fcitx5 会不会自动连上桥**：`WAYLAND_DISPLAY=wayland-serenity-0` 已在 fcitx5 环境里，
   但它要 `zwp_input_method_manager_v2` 才会 bind；桥加上后看 fcitx5 是否多出一个 client。
2. **字母能不能出来**：fcitx5 组好 → `commit_string` → egui 显示。
3. **快捷键还在不在**：Ctrl+C / Ctrl+V / 方向键（独占 grab 的副作用，见 §5）。
4. **中文候选窗**：fcitx5 自带候选窗（不是 Serenity 风格）能否在 Serenity 桌面上正常显示。
5. **GTK 应用**：`GTK_IM_MODULE=fcitx` 下 gedit 能否输入。

### 6.5 可选的并行小修

- `im-serenity.so` 编译 + 部署 + `GTK_IM_MODULE=serenity`：**本方案下不必要**（会抢 socket 唯一客户端位），
  但若最终要「候选窗用 Serenity 风格」，才需要另起一条路重新评估。
- `serenityime.so` 没装进 fcitx5 插件目录（只有 conf）——**原生应用路径可能也是残的**，值得单独查。

---

## 7. 踩坑记录（本轮新增）

| 坑 | 现象 | 正解 |
|---|---|---|
| `readlink /proc/PID/exe` 全等比较杀不掉旧桥 | `mv -f` 覆盖后 exe 变成 `...(deleted)`，全等不匹配 | 用 `case "$x" in *serenity-wlroots-host*)` 通配 |
| zsh 不对未加引号的变量做词分割 | `SSH_OPTS="-p 2222 -o ..."` + `ssh $SSH_OPTS` → `Bad port ' 2222 -o ...'` | zsh 里用数组，或**直接把选项内联**（本轮采用内联） |
| `ssh` 交互式密码会挂住后台任务 | rsync 卡在 `password:` 直到超时 | 用 `sshpass -p serenity`（宿主有 `/usr/bin/sshpass`） |
| `pkill -x` 对 >15 字符进程名无效 | 桥名 `serenity-wlroots-host` 截断 | 按 `/proc/*/exe` 匹配（见 §3） |
| 改错调用点 | 第一版修复改了 `on_surface_commit`，断言纹丝不动 | wlroots 断言先确认**调用链**，别猜 |

---

## 8. 本轮改动清单（工作区，未 commit）

`Ports/LinuxGUI/WlrootsHost/wlroots_proto.c`：

1. ✅ `schedule_toplevel_configure_idle` + `on_surface_commit` 首次 configure 推迟
   （**对断言无效，但本身无害**，保留）。
2. ✅ `deco_defer` / `schedule_decoration_mode_idle` / `request_server_side_decoration`；
   `on_new_decoration` / `on_decoration_request_mode` 改走推迟路径；
   `decoration_state` 增加 `proto` 字段。**← 这条是真修复**
3. ✅ 新增 2 行 include（IME 用，后续要用）。
4. ⬜ IME 子系统（§6）**未写**。
