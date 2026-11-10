# 部署测试报告 — serenity-wlroots-host P1

- 时间：2026-11-10 20:52 – 21:07
- 范围：P1（WS 原生菜单桥侧闭环）部署 + 系统级回归
- 结果：**部署成功，回归 11/11 PASS**；发现并修掉一个日志洪水缺陷；记录一次孤发 SIGABRT

## 1. 部署

| 项 | 值 |
|---|---|
| 部署脚本 | `Ports/LinuxGUI/WlrootsHost/deploy.sh`（本轮新增，可复现） |
| 二进制 | `Build/lagom/bin/serenity-wlroots-host` |
| 最终 md5 | `8c6b00a2040e3bbdff7020dc22e1b7b9`（461 KB）；宿主 / VM 一致 |
| 安装位置 | `/serenity/bin/serenity-wlroots-host` |
| 托管 | `serenitid`（serenity-desktop.service）自动重启 |

脚本固化了三个踩过的坑：scp 必须**串行**（并发会被 sshd 拒 `Connection closed`）；运行中的二进制不能直接覆盖（Text file busy），要 stage → 按 `/proc/*/exe` 杀 → `sudo mv` 原子替换；替换后轮询等待 serenitid 拉起并校验 socket + md5。

**部署副作用（必须知悉）**：重启桥会中断所有现有 Wayland 客户端连接，Xwayland 会重连，但已打开的 GTK 应用会退出。应在会话空闲时部署。

## 2. 回归矩阵

脚本：`Ports/LinuxGUI/WlrootsHost/tests/regression.sh`；原始结果：`deploy-test/results.tsv`。

| # | 测试项 | 期望依据 | 实测 | 判定 |
|---|---|---|---|---|
| T1 | 桥进程 | serenitid 托管，恰好 1 实例 | 1 实例，PID 4754 | ✅ |
| T2 | Wayland + X11 socket | `/run/user/1000/wayland-serenity-0` 与 `/tmp/.X11-unix/X100` 都在 | 两者均存在 | ✅ |
| T3 | Wayland 事件循环 | 最小客户端 `ROUNDTRIP_OK globals>=10`（冷启动死锁会挂住） | `globals=14` | ✅ |
| T4 | `serenity_menu_v1` global | 探针 `BOUND name=14 version=1` | `BOUND name=14 version=1` | ✅ |
| T5 | 菜单由 `Menu.cpp` 弹出 | 桥日志 `popup -> Menu.cpp` | `popup -> Menu.cpp at 300,200` | ✅ |
| T6 | 激活回传 | 键盘 Down+Return → `ACTIVATED id=1` + 桥 `activated` 日志 | 两侧均命中 | ✅ |
| T7 | dismiss / 资源清理 | 探针 `DISMISSED` + `DONE` | 均收到 | ✅ |
| T8 | X11/Xwayland 应用 | `xwininfo` 有 xterm 窗口 + XWM 配对日志 | 窗口 1 个 / 配对 3 条 | ✅ |
| T9 | GTK3 gedit | 进程存活且窗口显示 | pid=5516 | ✅ |
| T10 | GTK4 gnome-text-editor | 进程存活且窗口显示 | pid=5117 | ✅ |
| T11 | 桌面整体 | 截图无黑屏/残影，三个窗口与任务栏正常 | `deploy-test/desktop.png` | ✅ |

原生菜单截图 `deploy-test/menu-native.png`；与原生 Notepad「编辑」菜单 y=250 行逐像素一致：左边框 `rgb(0,0,0)` 1px、stripe 单条 24px `rgb(219,216,209)`、分隔线不切 stripe、快捷键右对齐、禁用项灰字。

## 3. 测试中发现并修掉的问题

### 3.1 `text_input commit` 日志洪水（已修）

`text_input_handle_commit()` 每次 commit 无条件 `fprintf`。GTK4 应用高频 commit，导致
`/tmp/serenity-session.log` 涨到 **100 MB / 122 万行**（后续累计 258 万行），其中
`text_input commit` 一条占 89 万+ 行。

修法：只在 `enabled / focused_surface / keyboard_focus_surface / input_method`
四个状态变化时打印（桥单实例，状态放文件级 static）。

实测效果（同一套回归脚本）：

| | 修复前一轮 | 修复后一轮 |
|---|---|---|
| 新增日志行数 | 359,054+ | **121** |
| 其中 `text_input commit` | 359,054 | **11** |

### 3.2 一次桥 SIGABRT（孤发，未复现，持续观察）

`21:00:45 wayland-host exited (status 0, signal 6)`，崩溃前最后一行是
`free(): invalid pointer`，紧随 X11 窗口 `plain surface #17 first frame`。
级联后果：Xwayland 断连重启 → gedit 报 `cannot open display: :100`（该轮 T9 失败）。

定性依据：
- 本会话**唯一一次** SIGABRT；部署时的 4 次 signal 9 是我主动 kill，性质不同。
- 源码里已有同类堆破坏的历史注释：`wlroots_proto.c:476`（Xwayland selection source
  塞进 Wayland seat 会 `free(): invalid`）、`wlroots_proto.c:567`（configure drain
  导致 `free(): invalid size`）、`main.cpp:993`（ContentLayer 父窗口销毁写坏堆表现为
  `free(): invalid pointer`）。都是**既有 X11/客户端生命周期路径**。
- P1 新增代码里唯一的 `free()` 是 `serenity_menu_proto.c` 的 `node_destroy_notify`
  中与 `calloc` 配对的释放；`s_native_menus` 走 RAII，无裸 free。崩溃时探针已
  `destroy` 完毕并走完 X11 surface 创建/首帧，P1 代码路径已退出。
- 修复日志洪水后重跑完整回归：**桥 PID 4754 全程存活 1:20，无新 restart、无新崩溃，
  11/11 PASS**。

**诚实结论**：证据指向既有 X11 生命周期问题、在"反复 SIGKILL 应用 + 重启 Xwayland"的
压力时序下偶发触发，但**不能 100% 排除与 P1 的间接关联**（堆破坏可能延迟显现）。
未做二进制 A/B（需再中断会话且崩溃不易稳定复现）。建议后续按
`WAYLAND_ROADMAP.md` 的 X11 生命周期项单独排查，并在长时间会话中观察。

### 3.3 测试脚本自身的两处假阴性（已修）

- T8 初版按 `-title REGRESS-X11` 去 `xwininfo` 里 grep，但 xwininfo 显示 WM_NAME，
  标题只出现在 XWM/wl_surface 侧 → 改为按窗口 class `xterm` 匹配。
- 探针初版用裸 `wl_display_dispatch()` 会吞掉先前 roundtrip 缓冲的事件 → 改为标准
  `prepare_read → poll → read_events → dispatch_pending`。

## 4. 完成度总结

| 目标 | 状态 |
|---|---|
| 部署可复现（脚本 + md5 校验 + 健康检查） | ✅ 完成 |
| 回归覆盖协议闭环 + 事件循环 + X11/GTK3/GTK4 + 桌面 | ✅ 11/11 |
| 日志洪水 | ✅ 已修并量化验证 |
| 一次孤发 SIGABRT | ⚠️ 已定位方向、复测未复现，**未根治** |
| GTK4 shim 拦截 `GtkPopoverMenu`（P1b） | ❌ 未做，GTK4 应用菜单仍走旧 xdg_popup 自绘 |
| VM 冷启动死锁 / QMP socket 失效 | ❌ 已知遗留，未修 |

## 5. 复现

```bash
cd Ports/LinuxGUI/WlrootsHost
# 1) 构建（宿主，注意 safe-bin / LIBRARY_PATH 坑）
PATH="$(printf '%s' "$PATH"|tr ':' '\n'|grep -v safe-bin|tr '\n' ':'|sed 's/:$//')" \
LIBRARY_PATH=<repo>/Build/vm-libs \
ninja -C <repo>/Build/lagom serenity-wlroots-host
# 2) 部署
./deploy.sh
# 3) 回归
bash tests/regression.sh
```

产物：`deploy-test/results.tsv`、`deploy-test/menu-native.png`、`deploy-test/desktop.png`。
