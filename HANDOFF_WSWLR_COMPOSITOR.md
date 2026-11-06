# HANDOFF_WSWLR_COMPOSITOR.md — wlroots 内嵌合成器开发记录

> ## ⛔ 已被否决：不要启用 wlroots presenter
>
> **2026-11-06 用户明确要求：合成器只能是 WindowServer（KMS presenter），坚决不能用 wlroots。**
>
> 本文档保留为**历史开发记录**，用于解释 wlroots 路径为什么会被放弃，不再作为可用方案。
> 禁止创建 `/etc/systemd/system/serenity-desktop.service.d/wlroots.conf`
> （`SERENITY_PRESENTER=wlroots` / `SERENITY_NO_CURSOR=1`）。原因见本文 §0.10：
> KMS 完全正常，只有 wlroots 路径会在鼠标移动/点击时冒黑块；另外它会让
> `wayland-host` 抢不到 `wayland-serenity-0` 的 lock 而无限重启，最终整个窗口消失。
> 部署后自检与回滚步骤见 `Ports/LinuxGUI/SCREENSHOT_RECORDER.md` 顶部。

> 项目：把 `serenity-window-server` 改造成真正的 compositor（单进程内嵌 wlroots 0.20）。
> 整理时间：2026-11-04（第四轮，**真机路径已跑通**）。
> 关联索引：`STATUS.md`、`HANDOFF.md`、`LINUX_GUI_PORT.md`、`WAYLAND_ROADMAP.md`。

---

## 0. 一句话目标（既定架构，不可退回）

`WindowServer` 是 compositor，**所有窗口（原生桌面、X11、Wayland）平等、自由堆叠于同一 scene graph**：

- 原生桌面：CPU 帧缓冲包装成 `DRM_FORMAT_ARGB8888` 的 `wlr_buffer`，作为服务端 surface 垫在 scene 底层；
- 外来 Wayland 客户端：经 `xdg_shell` 作为平等 scene 节点；
- 外来 X11 客户端：经 Xwayland 作为平等 scene 节点（可选，需 `WSWLR_XWAYLAND=1`）。

---

## 0.11 xdg_popup 首帧 configure 打通（2026-11-05，VM 实测通过）

`Ports/LinuxGUI/WlrootsHost/wlroots_proto.c` 上 popup 首帧 configure 的两个时机错误已闭环，
完整根因见下面几段（原 `WAYLAND_POPUP_CONFIGURE_SCHEME.md` 已删，内容并入本节）。

要点（都属「provider 在错误时机调 wlroots」这一类）：

1. `wlr_xdg_surface_schedule_configure` 断言 `surface->initialized`，该标志只在 popup 的
   wl_surface 首次 commit 时置真 → 不能在 `new_popup` 里调，改挂 `surface->events.commit`。
2. **`wlr_xdg_popup_unconstrain_from_box` 内部末尾就调 `schedule_configure`**（反汇编确认），
   所以在 `new_popup` 里做约束避让同样必崩，且现场毫无线索（`on_new_popup entered` 后直接断言）。
   → 约束框只能先存进 `popup_node`，首次 commit 后再生效；两者二选一，避免重复 configure。
3. `wlroots_proto_popup_unconstrain()` 对未配置的 popup 只更新存框。

VM 实测：`/tmp/wh2` + `tests/popuptest_client.c`（1 主 popup + 3 轮开/关），
四个 popup 各收到**恰好一次** configure，坐标与 positioner 语义一致，`Assertion` 计数 0，
`HOST_ALIVE=1`，rc=0。测试产物已归档到 `Ports/LinuxGUI/WlrootsHost/tests/`。

**真实部署测试（gedit，GTK3 原生 Wayland）**：二进制装到 `/serenity/bin/serenity-wlroots-host`，
独立 socket `wayland-serenity-9` 起，真实客户端实测 —— Shift+F10 上下文菜单走 xdg_popup、
F10 应用菜单走 wl_subsurface，两条都正常弹出/关闭，连开关 5 轮 configure 与创建严格 1:1，
无断言、RSS 稳定 40 MB。

顺带修掉一个只有真实测试才暴露的**窗口类型时机 bug**：`ensure_hosted_window()` 内部就 `show()`，
而调用方是在它返回后才 `set_window_type(Popup)`；但 `LibGUI::Window::set_window_type()` 只改本地
字段、不通知 WS（类型只在 `async_create_window` 随 create 发出），于是菜单按 `Normal` 建窗，
任务栏给每个 popup 加了一个「popup」任务按钮。已把 `GUI::WindowType` 提为
`ensure_hosted_window()` 参数并在 `show()` 之前设置（xdg_popup / subsurface / X11
override-redirect 三处），复测任务栏干净。

### 真实点击测试暴露的输入链路问题（同一轮修复）

弹菜单之外，真实鼠标操作还连着踩出三个问题，成因都在「我们这边的协议/坐标语义」而不是客户端：

1. **`enter` 写死 (0,0)** —— `SurfaceWidget::enter_event` 没有坐标可用，老代码直接发
   `wl_pointer.enter(…, 0, 0)`。窗口映射时若指针已在窗口内，后续没有 `motion` 来纠正，
   客户端认为指针停在左上角，**所有点击都打到 (0,0)**。改成只在有真实坐标时发
   enter+motion（与旧桥 `wayland_host.cpp` 的 on_mouse_move 同款）。
2. **输入坐标没反算 `window_geometry`** —— toplevel 的 buffer 是按 `window_geometry`
   裁掉 CSD 阴影后才进窗口的（见 `wlroots_proto.c` 的 `toplevel_committed`），所以
   widget 局部坐标要**加回**阴影偏移（GTK 是 26×23）才是客户端 surface 坐标。不加的话
   点击整体偏上 23px，正好落在 GTK 自己的标题栏上 —— 实测客户端直接发
   `xdg_toplevel.resize`，「点按钮变成拖窗口」。修法：`to_surface_position()`。
3. **缺 `wl_pointer.frame`** —— `wlr_seat` 的 `notify_motion/button/axis` **不会**自动发
   frame（头文件原文："Motion, button and axis events should all be followed by a frame
   event"）。少了它，GTK 把事件攒在批次里：**按钮按下不弹起、第一次点击只按下、第二次
   才响应**（用户观察到的「第一下只按下，第二下才弹菜单」）。已在 motion/button/axis
   后各补 `wlr_seat_pointer_notify_frame()`（enter 自带一次，不重复补）。
4. 附带：`pointer_enter` 对同一 surface 幂等（协议上 enter/leave 必须成对），以及
   映射时补发一次 `ACTIVATED`（窗口创建时已激活，那次变化早于回调注册，客户端否则
   认为窗口是非激活的）。

### 切换到 wlroots 新路线 + Firefox 实测（2026-11-05 下午）

生产会话的 provider 从旧桥 `serenity-wayland-host` 换成 `serenity-wlroots-host`
（`SessionManager/serenity_manager.cpp` 的 `make_child("wayland-host", ...)`）。
旧桥已弃用：它缺 `wl_subcompositor` 等 global，Firefox 的 Wayland 后端启动 0.2s 内就
`MOZ_RELEASE_ASSERT(GetSubcompositor())` 退出（minidump 里 `MozCrashReason` 写明
"We're missing subcompositor interface!"）。新路线的 globals 里有 `wl_subcompositor v1`。

切换后 Firefox 才真正跑起来，又踩出四个问题：

1. **内容型 wl_subsurface 不能独立成窗**。Firefox 把整个窗口内容放在一个偏移
   =(父内容区原点)、尺寸≈窗口的子表面里（实测偏移是 `(26,23)`，即父 buffer 的
   CSD 阴影偏移，减掉 `window_geometry` 才是 (0,0)）。旧代码给每个 subsurface 都建
   独立窗口，于是这个"内容层窗口"盖在主窗口上，**输入被它截走**（点地址栏、点按钮
   全没反应）。现在偏移落在内容区原点的子表面判定为 `ChildKind::ContentLayer`：
   不建窗，作为叠加层画进父窗口画布（`SurfaceWidget::set_overlay`），输入仍只发给父
   toplevel —— 符合 Wayland 语义。
2. **窗口尺寸跟随内容层**。不要用 `wlroots_proto_toplevel_set_size()` 把 host 的初始
   640x480 发下去：客户端会照它布局，而内容层仍按 wl_output 尺寸（1280x885）渲染，
   结果只有左上角一块有内容。尺寸由客户端定，WS 窗口在 content-layer 帧到达时
   `set_rect` 跟随（带 `set_suppress_resize`，不回灌 configure）。
3. **popup 位置要用 `popup->current.geometry`**。`wlr_xdg_popup_get_toplevel_coords()`
   沿 `base->current.geometry` 累加，而那是 xdg_surface 的 window geometry（popup 上
   恒为 0,0），实测只返回父窗口的 CSD 偏移 `(26,23)` —— 菜单被甩到窗口左上角。
   头文件写明 `popup->current.geometry` 是"相对父 surface window geometry 左上角"，
   正好等于我们要的父内容区坐标。改用后：`anchor_rect={1236,52 40x24}`、
   gravity=BOTTOM_LEFT 的菜单落在 `(936,76)`，即 ☰ 按钮正下方。
4. **paint 无 base bitmap 时仍要画叠加层**：Firefox 的父 surface 一帧 buffer 都没有，
   早退会让整个窗口空白。

另外 `position_popup()` 要跳过没有 `window` 的 entry（ContentLayer），否则 pump 的
"跟随父窗口"循环会解引用空 RefPtr 把 host 打死（`VERIFICATION FAILED: m_ptr`），
客户端看到的是 `Error reading events from display: Broken pipe`。

### 菜单的两个时序 bug（2026-11-05 晚，用户实测反馈）

5. **菜单窗口先在左上角闪一下**。`on_popup_created` 里 `ensure_hosted_window()` 尾部
   就 `show()`，而此刻 popup 的 geometry 往往还是 `(0,0)`（要等首个 configure 才对），
   于是窗口先出现在「父窗口内容区左上角 / 级联默认点」，随后才被 `position_popup()`
   挪到正确位置——视觉上就是「点汉堡菜单，左边闪一下窗口边框」。
   修法：菜单类窗口 `ensure_hosted_window(..., show_now=false)` 创建时不显示，
   等首帧到达、`position_popup()` 算完位置后再 show（`on_toplevel_committed` 的
   child 分支）。**注意 `on_popup_created` 末尾不要再补一句 show()**，那会把延迟
   显示整个抵消掉。
6. **连点菜单会把 host 打崩、并带崩桌面**。Firefox 的菜单结构是「popup →
   subsurface → 又一层 content-layer subsurface」，连点时子层的帧会赶在父窗口
   销毁之后到达；此时 `parent->widget` 已是野指针，`set_overlay()` 写坏堆，进程
   随后 `free(): invalid pointer`，Xwayland 报 `Broken pipe`，桌面 `serenity-shell`
   在 `remove_child` 里 `Vector.h:181 i < m_size` 越界 → 整个会话重启。
   修法：① `ContentLayer` 帧到达时校验 `parent->window/widget`，父已消失就自我清理；
   ② 删除任一 surface 时先用 `drop_content_layers_of()` 把它名下的叠加层一起删掉。
   验证：连续点击菜单 8 次 + 15 次，无新 `VERIFICATION`/`invalid free`，Firefox 与
   host 均存活。gedit（xdg_popup 路径）本来就稳定，问题只在 Firefox 的嵌套结构。
7. **菜单项点不动（"设置界面打不开"）**：`SurfaceWidget::mousedown_event` 里为修
   「第一次点击被吞」而加的 `wlroots_proto_keyboard_enter(m_surface)` 对所有 surface
   都生效，包括菜单 popup。点菜单项时它把 wl_keyboard 焦点从主窗口转到菜单表面；
   Firefox 的 XUL 菜单是**非 grabbing** 的，键盘焦点一离开就当作菜单失焦 → 关闭菜单，
   菜单项动作永远不执行（用户看到的就是「点 Settings 没反应、菜单消失」）。
   排查证据：press 定位日志显示客户端收到的坐标是对的（`local=(26,554)`，菜单
   340x734，正落在 Settings 行），但 host 侧没有任何后续 surface 请求；同时
   `firefox-esr --no-remote about:preferences` 直接打开设置页**完全正常**，
   证明页面本身没问题，问题在点击链路。
   修法：`SurfaceWidget::set_popup_surface()` 标记菜单类窗口，`mousedown` 时只对
   普通窗口发 `keyboard_enter`。
   验证：点菜单 Settings → 设置页正常打开；连点菜单 6 次无断言；gedit 菜单无回归。

8. **非激活 toplevel 永远拿不到首个 configure**（`wl-copy` / `wl-paste` 必踩）：
   旧实现只在 `set_activated` 时才间接触发 configure，于是**不上屏、不被激活的
   toplevel** 一直收不到 configure；客户端带 buffer 提交时被 wlroots 以
   `xdg_surface has never been configured`（error 3）拒绝并断开。实测 `wl-paste`
   直接报这个错。
   修法：`on_surface_commit` 里首次 commit（此时 `initialized` 已真）主动
   `schedule_configure()` 补一次，用 `toplevel_node.configure_sent` 保证只补一次。
   ⚠️ 这里**不能**再置 `configure_drain_pending`：那个抑制窗口是给「新建窗口首次
   configure」用的，在 commit 回调里重入会让 flush 的 drain 与 wlroots 的配置
   定时器互相等待，实测把进程带进堆破坏（`free(): invalid size`，signal 6）。
   验证：`wl-paste` 不再报协议错误，host 不再崩；gedit / Firefox 无回归。

9. **剪贴板：Wayland 内部已打通，Xwayland 侧隔离中**。
   根因是 wlroots 把客户端的 `set_selection` 当「请求」，compositor 必须调
   `wlr_seat_set_selection()` 才算接受；旧实现完全没监听 `request_set_selection`，
   seat 的 selection **恒为空**（应用内复制粘贴、wl-copy/wl-paste、Xwayland 全废）。
   现在 `on_request_set_selection` 已接上，实测 `echo WL-OK | wl-copy` 后
   `wl-paste` 能读到 `WL-OK`，Wayland 应用之间复制粘贴可用。
   **Xwayland 转发的 source 用 X 原子名（STRING/UTF8_STRING/TEXT），直接塞进
   Wayland seat 会让 host 堆破坏（`free(): invalid pointer` / `invalid size`，signal 6）**，
   所以先用 `source_is_x11()`（有 X 原子名且没有任何 `text/plain*`）把它们挡在外面，
   X 内部剪贴板仍由 Xwayland 自己维护。待办：拿到 Xwayland 的 seat-client 后按
   serial 语义正确接受，再补 Serenity `GUI::Clipboard` 的双向桥（新桥这块整体缺失）。

**剪贴板不通的旧根因记录（已修复部分）**：
   wlroots 的 `request_set_selection` 事件语义是「应用**想**复制，compositor 必须调
   `wlr_seat_set_selection()` 才算接受」；我们既没监听也没接受，于是 seat 的
   selection **恒为空** —— 这解释了全部症状：应用内复制粘贴、`wl-copy`→`wl-paste`、
   Xwayland 的 X↔W 转发，全都拿不到数据（`target STRING not available` /
   `Nothing is copied`）。
   已尝试直接转发（`on_request_set_selection` → `wlr_seat_set_selection(seat,
   event->source, event->serial)`），日志能看到 `selection accepted`，但随后进程
   `free(): invalid pointer`（signal 6）崩溃——source 生命周期 / serial 校验没处理对，
   已回滚，`wlroots_proto.c` 里留了 `TODO(clipboard)`。回滚后 host/shell 稳定。
   下一步：照 wlroots 的 seat-client 语义（serial 必须是发给该 client 的、source
   引用要持有到下一个 selection 或 destroy）实现，再接 Xwayland 的 X↔W 转发。

**剪贴板双向仍不通（旧记录，根因见上）**：`wl-copy` 写 Wayland selection 后
   `xclip -o -selection clipboard` 报 `target STRING not available`；`xclip` 写
   X selection 后 `wl-paste` 得到 `Nothing is copied`。`data_device_manager` 由
   `wlr_data_device_manager_create()` 提供（协议层 OK，客户端不再报错），但
   Xwayland 的 X↔Wayland 转发没有生效 —— 这是独立缺口，需要照 mutter/xwayland
   的 selection 桥接单独实现，不在本次范围。

### 输入法候选窗 UI 统一（2026-11-05 12:40）

按用户要求把 LibGUI 的候选窗（`Userland/Libraries/LibGUI/InputMethodCandidateWindow.cpp`）
从「Win2000 竖排 + 3D 边框」改成 gedit 里 fcitx5 候选窗的观感：

- 布局：候选**横向排成一行**（原来是竖排），aux 压缩到最左侧（`preedit` 已移出候选窗、
  改为在编辑器输入区光标处**内联**绘制，见下）；
- 配色（2026-11-05 对比度修正后）：冷灰面板底 `#e8edf3` + 边框 `#b6c2d4`，正文 `#1f2a37`、
  序号/aux 浅灰 `#6b7785`，选中项**蓝底 `#2f6fd0` + 白字**（原来白底比底色还亮、完全看不出选中，
  被用户判"不对吧"）；候选之间加细分隔线 `#d5dde6`；面板带 2px 下拉阴影（用
  `with_alpha` 合成，浮在白纸上有层次）。
- 翻页：右侧 `<>` 两个箭头，不可翻页时置灰。

⚠️ **部署库的坑**：只替换 `/serenity/lib/liblagom-gui.so` 会让桌面起不来
（`symbol lookup error: undefined symbol IPC::encode<float>`）——宿主 Build/lagom 的
LibGUI 依赖新版 LibIPC，必须同步部署 `liblagom-ipc.so`，否则 shell/notepad 全部加载失败。
以后改 LibGUI 一律「gui + ipc 一起部署」。

### 候选窗对比度修正 + 内联预编辑（2026-11-05）

用户看截图判候选窗"不对吧"：旧配色 `#f4f4f4` 底几乎与白纸同色、且选中项用白底（比底色还亮）
等于看不见选中态。已重做配色（冷灰面板 + 蓝底白字选中 + 2px 阴影 + 分隔线），见上。

同时按用户要求实现**内联预编辑**（README 的"内联预编辑" TODO 划掉）：

- `TextEditor` 新增 `m_preedit` / `m_preedit_cursor` / `m_preedit_caret_rect` 与
  `set_preedit(String, int)`；`InputMethod::handle_panel` / `commit` / `focus_out` / `reset`
  都会把拼音推给 `editor->set_preedit(...)`。
- `TextEditor::paint_event` 在**包含光标的那个可视行**做 splice：在缓冲区光标处画
  `[pre_part][拼音(下划线)][post_part]`，并把光标移到拼音内部的 `preedit_cursor` 处
  （与缓冲区光标同闪烁）；其余可视行照常绘制。pinyin 因此显示在**输入区光标处**，而不是
  只画在浮动候选窗里。
- 候选窗不再画 `preedit`（避免与内联重复），只保留 `aux` 提示 + 候选列表 + 翻页。
- 已知小缺陷：长拼音超出可视宽度时，横向滚动不会跟随拼音末尾（待改进）。

**VM 实测（2026-11-05 13:31，notepad 原生 LibGUI 应用）**：打出 `nihao` 后，文本区光标处
显示带实心下划线的 `ni hao`（含拼音内部光标），下方同时弹出横向候选窗「1 你好 2 姝好 3 逆號
4 擬好 5 你 < >」，首个「你好」蓝底白字选中；按空格后「你好」上屏、候选窗与下划线消失。
截图存档 `Build/vm/ime-inline-{compose,detail,commit}-2026-11-05.png`。
部署注意：运行时库在 `/serenity/lib`（非 `/usr/local/lib`），须 `systemctl stop
serenity-desktop.service` 后再覆盖两个 `.so` 并重启服务。IME 需用 `fcitx5-remote -s rime`
激活（rime 为默认时 Ctrl+Space 会反切回 keyboard-us），见 `Build/vm/ime_show.py`。

⚠️ 内联预编辑只对"包含光标的那一个可视行"生效；合成中途若编辑器有选区/语法高亮，
该行的选区与 trailing-whitespace 网格在合成期间会被跳过（合成结束即恢复）。

**阶段性收尾（2026-11-05 11:55，用户确认）**：Firefox 页面内容实际渲染正常
（百度首页完整显示：地址栏、搜索框、热搜列表、底部图标全部正确），此前记录的
"页面内容区空白"是**我的截图通道抓帧太早**（VNC 取到的还是未刷新的帧），不是渲染问题。

> 📌 Wayland 协议层到此**暂时完成**。已知未覆盖项：偶发 SIGSEGV（minidump 指向
> libxul 的 GLib log writer）、Firefox 菜单的 grab 路径、HiDPI。后续发现 bug 再单独查，
> 不再占用主线。

**菜单四周的灰边（不是"合成器不支持阴影/透明"）**：GTK popover 在内容外围画半透明
CSD 阴影，而我们的子窗口是**不透明窗口 + 控件先铺了主题灰底**，半透明像素被混到灰底上
就成了一圈灰边。修法：popup / subsurface / X11 override-redirect 一律
`set_has_alpha_channel(true)` + 不再 `set_fill_with_background_color(true)`，
阴影这才真正与下层混合（放大截图可见柔和渐变）。

> 内嵌路线（`WSWlrBridge/host`）走 `wlr_scene`，configure 由 scene 自己发，无同类问题。

---

## 0.9 拆层实施方案（labwc 结构，下次一次做完）

目标：每个原生窗口一块 layer，窗口内容与 WindowServer 的 framebuffer 半区解耦，
不再依赖 `flush_framebuffer_rects` 的半区契约 —— 这是黑块的根，也是 labwc 的结构。

### 需要的三层改动

1. **桥（已完成）**
   `wswlr_compositor_create_layer(c, x, y, w, h)` / `wswlr_layer_present` /
   `wswlr_layer_set_position` / `wswlr_layer_raise` / `wswlr_layer_destroy`。
   `cpu_buffer` 已实现 `get_shm`（GLES 必需）与 `release` 驱动的复用。

2. **Backend 转发**（`Ports/LinuxGUI/LinuxScreenBackend.{h,cpp}`）
   ```cpp
   void* create_window_layer(int x, int y, int w, int h);
   void present_window_layer(void* layer, Gfx::Bitmap const& bitmap, int x, int y);
   void destroy_window_layer(void* layer);
   ```
   内部直接调上面 3 个桥函数（`present` 用 `bitmap.scanline(0)` + `pitch()`）。

3. **Compositor 拆层**（`Userland/Services/WindowServer/Compositor.cpp`）
   - 现成的 `compose_window_rect(Screen&, Gfx::Painter&, Gfx::IntRect const&)`（第 376 行）
     **已经支持任意 painter**，直接复用它往 layer 的 bitmap 上画：
     ```cpp
     auto frame = window.frame().render_rect();
     auto bitmap = Gfx::Bitmap::create(BGRA8888, frame.size());   // 忽略 alpha 也行
     Gfx::Painter p(*bitmap);
     p.translate(-frame.location());
     compose_window_rect(screen, p, frame);
     backend.present_window_layer(layer, *bitmap, frame.x(), frame.y());
     ```
   - 在 `Compositor.cpp:536` 的 `for_each_visible_window_from_back_to_front` 里分叉：
     wlroots 单层模式下**不再** `compose_window(window)`（不画进整屏），只走 layer。
   - 桌面/任务栏（`WindowType::Desktop/Taskbar`）暂时保留在整屏 layer（它们全屏、更新少）。
   - 每窗口维护 `HashMap<Window*, void*> layer`，窗口关闭/尺寸变化时重建。

### 验证顺序（必须逐条过）

1. 只开一个窗口（Notepad/计算器）：layer 位置/内容/复古框正确；
2. 拖动窗口：跟手、无黑块；
3. 快速连续点击桌面图标：任务栏/图标区域无黑块（这是之前复现的用例）；
4. 同时开 weston-terminal（外来 xdg）+ 原生窗口 + 任务栏：三者叠放正确；
5. 空闲 CPU < 3%。

### 已知风险

- `window.frame().paint(screen, painter, rect)` 需要 `Screen&`（DPI/主题），但目标是 layer 的
  painter —— 已确认签名接受 painter，只是内部可能用 screen 的 scale，需要实测。
- 窗口 layer 每次重建 bitmap 有开销，按窗口缓存 layer + 复用 bitmap。
- 桌面 layer 仍需接受「半区可能陈旧」，所以对桌面区域要**强制全幅重绘**
  （或在 wlroots 模式下让 `flush_framebuffer_rects` 的 rects 始终为整屏）。

## 0.10 冲突核对（2026-11-04 22:10）：黑块是**渲染 bug**，不是 WS 写黑

用户判据：`SERENITY_PRESENTER=kms` 完全正常，只有 wlroots 路径在鼠标移动/点击时冒黑块，
且 WS framebuffer dump 里没有未绘制区域 → 黑块只能产生在 wlroots 的合成/上传环节。

### 根因：像素格式契约冲突（BGRx8888 ↔ DRM_FORMAT_ARGB8888）

- 桌面层来自 WS 的 `Gfx::BitmapFormat::BGRx8888`，最高字节是**无意义的 x**；
  `Bitmap::allocate_backing_store()` 用 `kcalloc` 清零，所以 x 通常是 0。
- 窗口层是 WS 主动画成 `BGRA8888`（`render_window_to_layer()` 先
  `clear_rect(Transparent)`），alpha **有意义**，圆角/阴影靠它混合。
- 两种像素共用一个 `cpu_buffer`，而 `wlr_buffer` 必须声明 `DRM_FORMAT_ARGB8888`
  （窗口层要真 alpha）。于是**桌面层的 x=0 被 GLES 当成 alpha=0**：整块像素做
  透明混合，桌面底下没有任何节点 → 露出 output 黑底。鼠标一动，WS 走
  `restore_cursor_back` / `m_temp_bitmap` 回拷等路径带出更多 x=0 像素，黑块就跟着长。
- **KMS 路径直接扫描 XRGB、压根不读这个字节**，所以从来没有这个症状；
  pixman 渲染器不做 GL 透明混合也比 GLES 干净 —— 与实测完全吻合。

修法（`layer_present_rects`）：把 WS 的「XRGB、x 无意义」语义**映射**到 wlroots 的
ARGB 契约，桌面层拷贝时逐像素 `|= 0xff000000`。窗口层不走这条路径，alpha 保持不变。

### WS 与 wlroots 功能/协议重复清单（WS 是唯一决策者）

| 功能 | WS 侧（保留） | wlroots 侧 | 处理 |
| --- | --- | --- | --- |
| 脏区 / 局部重绘 | `Compositor::flush()` 的 `m_flush_rects` | `wlr_scene_output.damage_ring` + swapchain buffer-age | **删**：`on_output_frame()` 每次 `wlr_damage_ring_add_whole()`，整幅重绘。两套 damage 判断不一致时，新 swapchain buffer 的非脏区未初始化 → 黑块/残影 |
| 像素格式语义 | `BGRx8888`（XRGB） | `DRM_FORMAT_ARGB8888` | **映射**：桌面层写入时把 x 规范成 opaque（见上） |
| 帧调度 | compose timer / `invalidate_*` | `output.events.frame` | **映射**：`request_frame()` 只由 WS 的 present 驱动，不在 frame 回调里自排帧（§3.10/3.11） |
| 光标 | `CompositorScreenData::draw_cursor` / `restore_cursor_back` | `wlr_cursor` + xcursor + 硬件光标 | **只保留一个**：`SERENITY_NO_CURSOR=1` 关掉 WS 的，wlroots 作为「顶层光标代理」。WS 的光标烤在桌面层会被窗口盖住，所以这一项属于「删不掉 → 映射」 |
| 指针坐标 / 加速 | `ScreenInput` / `LinuxInput` | `wlr_cursor` 自维护 x/y + libinput accel | **映射**：wlroots 只采集事件，经 `pointer_motion_absolute` 回灌 WS；`SERENITY_POINTER_FLAT_ACCEL=1` 两边对齐 |
| 焦点 | `WindowManager` | `pointer_surface` / `focused_foreign` + seat enter/clear | **映射（未完）**：外来客户端的 seat 焦点目前仍由 wlroots 命中测试决定，尚未接 WS 窗口焦点 |
| 窗口层叠 | `WindowManager` + `apply_window_layer_order` | `wlr_scene_node_raise_to_top` | **映射**：节点 raise 只由 WS 的窗口栈顺序驱动 |
| 窗口装饰 | `WindowFrame` 复古边框 | `build_frame()` / `update_frame_geometry()` | **映射（未完）**：外来窗口还没进 WS 窗口列表，暂由 wlroots 代画；接入后删除 |
| 输入设备独占 | `LinuxInput`（libinput） | `wlr_backend`（libinput） | **只保留一个**：wlroots 独占，WS 不再自己开 libinput（§3.5） |

### 本轮改动

- `Ports/LinuxGUI/WSWlrBridge/host/wlroots_backend.c`
  - `layer_present_rects()`：桌面层拷贝时规范 alpha（根因修复）；
  - `on_output_frame()`：commit 前 `wlr_damage_ring_add_whole()`，删除 wlroots 的第二套局部重绘；
  - 显式 include `wlr/types/wlr_damage_ring.h`。
- 已构建并部署到 VM（`/serenity/bin/serenity-window-server`，`serenity-desktop.service` 正常 active，
  journal 无 wlroots 报错）。

> ⚠️ **实测结论（22:12 用户反馈）：以上两条修复没有消除黑块**，鼠标移动仍出现大量黑块。
> 另外用 20 次鼠标移动压测过 WS 进程：`fd=132 / memfd=81 / VmRSS=159936 kB` 前后**完全不变**，
> 排除了「每次 present 新建 memfd/GL 纹理导致泄漏累积」的假设。
> 因此上面那个 alpha 契约分析只是「一个真实的格式语义不一致」，**不是本次黑块的根因**。
> 真正的结构性问题是 §0.11：WS 与 wlroots 各自有一套合成层。

## 0.11 架构决策（2026-11-04 22:25）：去掉 wlroots 合成层，WS 做唯一合成层

用户拍板：**Wayland/Xwayland 兼容层不再自己写，由已有的 wlroots 方案裁减而来**；
窗口绘制、按钮、边框、层叠、输出全部由 WindowServer 负责。

判断依据：同一个 wlroots 0.20 换 pixman renderer 就干净、只有 GLES 路径出黑块 →
不是 wlroots 版本 bug，而是「WS 已有一套合成层，wlroots 又叠一套」的结构性重复。
裁掉 wlroots 的合成层后，GLES 纹理缓存 / damage ring / swapchain buffer-age
整条问题链根本不存在，输出回到已验证干净的 WS/KMS 路径。

### 可行性确认（wlroots 0.20.2 头文件）
- `wlr_compositor_create(display, 6, NULL)`：renderer 允许 NULL，此时不创建任何 texture；
- `wlr_shm_create(display, 1, formats, len)`：wl_shm 不需要 renderer；
- 因此 `wlr_backend` / `wlr_renderer` / `wlr_allocator` / `wlr_scene` / `wlr_output`
  / `wlr_cursor` 全部可以不要，也不再与 WS 争 DRM。

### P0 已落地（编译通过，尚未部署）
新增 `Ports/LinuxGUI/WlrootsHost/`（独立可执行文件 `serenity-wlroots-host`）：

| 文件 | 职责 |
| --- | --- |
| `wlroots_proto.h/.c` | 裁减版协议服务端：wl_display + wlr_compositor(NULL) + wlr_shm(ARGB/XRGB) + xdg_shell + seat + data_device + xdg_decoration。**零渲染代码**。`surface.commit` 时 `wlr_buffer_begin_data_ptr_access(READ)` 把 CPU 像素交给回调 |
| `main.cpp` | LibGUI 客户端：每个 toplevel 一个普通 `GUI::Window`；shm 像素（非预乘）转预乘 BGRA8888 后 `painter.blit` 进 `SurfaceWidget`；窗口尺寸/标题/关闭/激活态与客户端互相同步；输入由 WS 单向映射进 seat（pointer enter/motion/button/axis） |
| `CMakeLists.txt` | 链 LibCore/LibGfx/LibGUI/LibMain + wlroots-0.20；挂在 `Meta/Lagom/CMakeLists.txt`（检测到 wlroots 才构建） |

构建：`cmake --build Build/lagom --target serenity-wlroots-host -j6`，产物
`Build/lagom/bin/serenity-wlroots-host`（138 KB）。

### 功能归属核对
- **wlroots 侧绘制代码：0 行**（grep `wlr_scene` / `wlr_output` / `wlr_renderer` /
  `wlr_buffer_create` / `build_frame` / `xcursor` / `draw_` 全部无命中）。
- **窗口边框、标题栏、最小化/关闭按钮**：WS 的 `WindowFrame`（`GUI::Window` 自动套复古框）。
- **按钮像素**：外来客户端自己画进 wl_shm buffer；wlroots 只转交 CPU 指针；
  WS 当作窗口 backing store 合成并显示 —— 显示权在 WS。
- **层叠 / 焦点 / 激活 / 任务栏 / 脏区 / 翻页 / KMS 输出**：全部 WS。
- **输入**：WS 独占 libinput，`WindowManager` 决定焦点，单向映射进 `wlr_seat`。

### 仍缺（P1/P2）
1. **键盘**：seat 目前只声明 POINTER；没有 `wlr_keyboard` 就没有 keymap，
   `keyboard_enter/leave` 是空实现 → 客户端收不到按键（按钮能点、字打不了）。
2. **Xwayland**：`xwayland` 参数目前忽略，X11 应用未接（要接 `wlr_xwayland_surface` 回调）。
3. popup / subsurface / CSD 阴影裁剪 / 剪贴板 / text-input / pointer-constraints：P2。
4. **清理旧路径**：`WSWlrBridge/host/wlroots_backend.c` 里 6 处 `wlr_scene_rect_create`
   / `wlr_cursor_set_xcursor`（装饰与光标）以及 `Compositor.cpp` 的拆层代码仍在仓库里，
   新 host 不链接它们；要彻底「只保留一个」需把内嵌 presenter 从 WS 摘掉并下线
   `SERENITY_PRESENTER=wlroots`。

## 0.8 现状（2026-11-04 21:05）：wlroots 路径**暂停**，生产保持 KMS

用户实测基线：**KMS 路径无黑块、鼠标正常**；wlroots 路径在快速交互时仍有黑块。
本轮三种 buffer 模型（每帧新 buffer+release 池 / 单块增量+pending / 双块各自演进+pending）
都未达标，且「增量」版本会写不完整（黑屏/残影）。

**必须先搞清 WindowServer 的契约，再改缓冲模型**：
`flush_framebuffer_rects(buffer_index, rects)` 里 `rects` 是相对哪个半区、是否保证
「该半区在 rects 之外的内容与上一帧一致」。在这个契约明确之前，任何 wlroots 侧模型都是猜。

已知可靠结论（保留）：
1. `cpu_buffer` 必须实现 `get_shm`（memfd+mmap），否则 GLES 整层不画（黑屏）；
2. `wlr_buffer.events.release` 确实会触发（实测 60 次 present / 59 次 release）；
3. 一层合成的 layer API 已就位（`wswlr_compositor_create_layer` 等）。

## 0.7 现状（2026-11-04 20:55）：一层合成**真机跑通**

`SERENITY_PRESENTER=wlroots` 下真机验证通过（`SERENITY_NO_CURSOR=1`）：

- 桌面（壁纸/图标/任务栏）由「最底下的全屏 layer」提供；
- weston-terminal 作为 scene 节点合成，套复古框；
- 键盘输入 `hi` 正常上屏（命中测试 → seat → 客户端）；
- **连续 6 次快速点击无黑块/无重影**，画面不冻结；
- 空闲 CPU 1% 量级。

两个决定性修复（此前所有「黑屏/黑块/重影」都是这两条）：

1. **`cpu_buffer` 必须实现 `get_shm`**（memfd + mmap 提供 fd/format/stride/offset）。
   只实现 `begin_data_ptr_access` 时 pixman 能读、**GLES 直接跳过该节点** →
   headless 正常、真机黑屏。
2. **buffer 复用必须由 `wlr_buffer.events.release` 驱动**（4 块池，只挑 idle 的重写；
   池满就丢这一帧）。按「两帧交替」或「延迟一次释放」猜都会在快速点击时黑块 ——
   GLES 上传是异步的，会读到已被回收/正在改写的内存。

证据：`Build/vm/wswlr-layer-desktop.png`、`Build/vm/wswlr-layer-terminal.png`。

## 0.6 现状（2026-11-04 20:50）：bridge 已按「一层合成」重建

`wlroots_backend.c` 已重写为 1176 行、零警告，结构就是一层合成：

- `wswlr_layer`：每个原生窗口一块（`wlr_scene_buffer` + 两块自建 `cpu_buffer` 交替）；
- 桌面用同一个 layer 机制（`wswlr_compositor_create_desktop` 只是「最底下的全屏 layer」）；
- 外来 xdg / xwayland 窗口仍是 scene 节点；输入路由、装饰、popup、decoration 都在。

**已验证（headless + pixman）**：桌面层 + weston-terminal 正常合成，`foreign xdg window` 回调正常。

**未通过（真机 + GLES）**：黑屏。同一个二进制在 `WLR_RENDERER=pixman` 下正常，说明是
**GLES 渲染器对我们自建 wlr_buffer 的上传路径**没走通（`begin_data_ptr_access` 之外，
GLES 还需要 `get_shm`/dma-buf 或 shm 纹理路径）。这是下一步唯一的阻塞点。

VM 当前已回滚 KMS（删除 drop-in + `.bak-kms` 二进制），桌面可用。

## 0.5 ⚠️ 状态：wlroots presenter 暂时回滚（2026-11-04 20:45）

`Ports/LinuxGUI/WSWlrBridge/host/wlroots_backend.c` 在一次脚本化区间替换中被截断（1300→220 行），
核心逻辑（窗口/输入/装饰/合成器创建）丢失。VM 已切回 KMS 路径（删除 drop-in + `.bak-kms` 二进制），
桌面可用。重建基底：`/tmp/wswlr-salvage/wlroots_backend_1744.c`（VM 上 17:44 的 424 行旧版）。

## 1. 现状（截至本轮结束）

| 能力 | 状态 | 落点 |
|---|---|---|
| headless 冒烟（scene + 截图） | ✅ | `WSWLR_HEADLESS=1` + `WSWLR_SHOT` |
| 外来 Wayland toplevel 挂载 | ✅ | `on_new_toplevel` |
| 三类窗口同屏（原生/Wayland/GTK4/X11） | ✅ | `Build/vm/wswlr-02/03-*.png` |
| **真机桌面（KMS + seatd）** | ✅ **本轮打通** | `SERENITY_PRESENTER=wlroots` |
| 输入（wlroots 独占 libinput → 原生 + 客户端） | ✅ 基本可用 | `on_cursor_*` / `on_keyboard_key` → `LinuxInput::feed_*` |
| 光标 | ✅ 由 wlroots 画在最上层 | `SERENITY_NO_CURSOR=1` 关掉 WS 自绘 |
| popup 约束 / grab、Xwayland 生产接线、HiDPI | ❌ 未做 | 见 §5 |

证据截图：`Build/vm/wswlr-real-desktop.png`（真机桌面）、
`Build/vm/wswlr-real-client-input.png`（真机桌面 + weston-terminal + 键盘输入 `echo hi`）。

---

## 1.5 架构澄清：这里其实是「双层合成」，不是两个合成器打架

WindowServer 自带 Compositor + WindowManager，它仍然把原生窗口、装饰、壁纸合成到自己的
CPU framebuffer；wlroots 只是把这块 framebuffer 当成**一个** `wlr_scene_buffer`，再和外来
窗口一起合成到 DRM 输出。所以：

- **不冲突**：wlroots 独占 DRM/输入，WS 的合成结果降级成一张纹理，没有第二个东西在抢屏幕；
- **有代价**：① 双份合成 + 每帧整幅 memcpy（8MB@1080p）；② wlroots 不知道 WS 内部的脏区，
  只能整幅上传，与 WS 的「只画脏区」语义冲突（这是 §3.8 闪烁的根源）；③ 光标必须二选一
  （已让给 wlroots，`SERENITY_NO_CURSOR=1`）；④ 未做 §3.10 之前会互相激出 60Hz 空转。
- **终局**（如果要把 §0 的「平等 scene 节点」做实）：把 WS 的 Compositor 降级成「每个原生窗口
  各渲染一块 surface」，由 wlroots 直接合成每个窗口 —— 只有一层合成、天然有 damage。
  工作量大，当前双层方案是可用的过渡态。

## 2. 关键实现（按文件）

### 2.1 桥 `Ports/LinuxGUI/WSWlrBridge/host/wlroots_backend.{h,c}`

**窗口**
- toplevel 用 `xdg_shell->events.new_toplevel`（role 已确定），**不能**用 `new_surface`；
- 初始 configure 必须自己发：首次 `xdg_surface->initialized` 后 `set_activated(true)` + `set_size(0,0)`；
- `map/unmap/commit` 生命周期；commit 里按 `xdg_surface->geometry` 用 `wlr_scene_rect_set_size` 重排装饰；
- popup 挂到父 toplevel 的 content 树下（`popup->current.geometry` 定位，未做 unconstrain）；
- `zxdg_decoration_manager_v1` 强制 SERVER_SIDE；
- Xwayland：`request_configure` → `wlr_xwayland_surface_configure`（不接就永远 1x1）、
  surface commit 跟尺寸/坐标、override-redirect 带 `WSWLR_FOREIGN_OVERRIDE_REDIRECT` 上报。

**回调（`wswlr_callbacks`）**
```c
on_new_foreign(surface, kind, title, app_id, flags, user);  // flags: OR / DIALOG / POPUP
on_destroy_foreign(...);
input.pointer_motion_absolute / pointer_button / pointer_axis / keyboard_key;
```

**输入路由**（`surface_at()` 命中 `wlr_scene_surface` = 外来客户端）
- motion：**始终**先回报原生（绝对坐标，保证原生光标/命中一致），再视命中发 seat enter/motion；
- button：命中外来 → 焦点转给它的 root surface + `notify_button`；否则清焦点、回报原生；
- axis：有 pointer focus 走 seat，否则回报原生；
- keyboard：`focused_foreign` 非空 → `wlr_seat_keyboard_notify_key`，否则回报原生；
  keycode 是 evdev code，直接复用 `serenity_keymap_lookup()`；
- 光标：`wlr_cursor` + `wlr_xcursor_manager`（`WSWLR_XKB_LAYOUT` 可改布局，默认 us）。

**真机后端**：`wlr_backend_autocreate` 之前必须 `unsetenv("WAYLAND_DISPLAY")` / `unsetenv("DISPLAY")`
（否则被当成嵌套客户端，见 §3.1）；`WSWLR_NESTED=1` 保留嵌套调试能力。
Socket 名默认 `wayland-serenity-0`（headless 用 `wayland-0`），`WSWLR_SOCKET` 可覆盖。

**调试**：`WSWLR_SHOT=/tmp/x.ppm` + `_DELAY_MS` / `_REPEAT_MS`（headless 唯一视觉验证通道）；
`WSWLR_FAKE_CLICK="x,y"` / `WSWLR_FAKE_KEYS="30 31"` + `_DELAY_MS`（无设备时验证路由）。

### 2.2 原生输入 `Ports/LinuxGUI/LinuxInput.{h,cpp}`

- 抽出 `apply_pointer_button()` / `apply_keyboard_key()`，libinput 直连与 wlroots 两条路径**共用**翻译；
- 新增 `feed_pointer_motion_absolute / feed_pointer_button / feed_pointer_axis / feed_keyboard_key`
  （像素 → 0..0xffff 归一化，其余原样复用）。

### 2.3 会话 / 装配

- `main_linux.cpp`：`on_mouse/on_key` 回调**无条件**注册；`SERENITY_PRESENTER=wlroots` 时不初始化
  `LinuxInput`（libinput 归合成器），headless 时也不初始化；
- `LinuxScreenBackend.cpp`：把 `wswlr_input_callbacks` 接到 `LinuxInput::feed_*`；
- `SessionManager/serenity_manager.cpp`：`SERENITY_PRESENTER=wlroots` 时**绝不 spawn `wayland-host`**
  （两者会抢 DRM/libinput 和同一个 socket 名），只等 socket；
- `Compositor.cpp`：`SERENITY_NO_CURSOR=1` 时不自绘光标；
- `CMakeLists.txt`：链接补 `PkgConfig::XKBCOMMON`、`PkgConfig::XCURSOR`（wlroots 的 `.pc` 只在
  `Requires.private` 里列，共享链接不传递，会 undefined symbol）。

---

## 3. 本轮踩的坑（都是「症状 → 真因」）

1. **`wlr_backend_autocreate` 选了 wayland 后端**：
   `[ERROR] Could not connect to remote display` —— 因为 serenitid 给子进程导出了
   `WAYLAND_DISPLAY=wayland-serenity-0`（那是给 Linux 应用用的）。真机必须先 unset。
2. **`LIBSEAT_BACKEND=builtin` 卡死**：`Could not open target tty: Permission denied`
   （`/dev/tty0` 是 `root:tty 0600`）→ `Waiting for a session to become active` 永不返回。
   **正解：用系统 seatd**（VM 里 `seatd -g video`，serenity 本来就在 `video` 组）。
3. **`set_activated`/`set_size` 早于 `initialized`** → wlroots 断言直接杀进程。
4. **不接 `request_configure`** → X11 窗口永远 1x1（`xterm` 460x264 才对）。
5. **两个 WindowServer 同时开 libinput** → 键鼠被劈成两半（用户当场察觉"和 VM 里的 GUI 冲突"）。
   现在 wlroots 独占；headless/dev 实例完全不碰输入。
6. **`SERENITY_LOG` 只被 `serenitid` 消费**（`SessionManager/serenity_manager.cpp:434`），
   直接跑 WS 时设了没用，日志一律走 stdout —— 这是上一轮"日志没生成"的真因。
7. **测试脚本里 `xmessage` 是前台阻塞程序**：`xmessage … && break` 会把整条 ssh 卡死，一律加 `&`。
8. **画面「不固定区域一直闪」**（本轮新修）：wlroots presenter 每帧整幅拷贝**当前那一块**
   framebuffer，而 WindowServer 是双缓冲 + 只画脏区 —— 另一块半屏留着旧内容，两帧一交替就闪。
   KMS 路径有 `pending_sync/accum_sync` 把累积脏区补到另一块，wlroots 路径没有这层。
   修法：`map_framebuffer()` 里 `m_back_buffer_offset = presenter_should_use_wlroots() ? 0 : size/2`，
   即 wlroots 路径退化成单缓冲（`LinuxScreenBackend.cpp`）。
9. **鼠标定位漂移 / 只有四角有效**（环境侧）：VM 原来只有相对指针（`virtio-mouse`），
   VNC 的绝对坐标被 QEMU 转成增量 + 指针加速 → 落点偏几十像素；后来加 tablet 又和 PS/2 的
   `VMMouse`（也是 ABS 设备）抢同一个 cursor → 只有极值处命中。
   正式做法：`vm.sh` 只给 `-device virtio-tablet-pci`（绝对指针），并在 guest 里
   `/etc/modprobe.d/serenity-vm-input.conf` 用 `install psmouse /bin/false` 硬拦住 PS/2
   （`blacklist` 不够，它会被依赖加载）。
10. **空闲 60Hz 空转 → 高 CPU + 视觉抖动（本轮修）**：`on_output_frame` 里无条件
    `wlr_output_schedule_frame()`，导致每个 vblank 都重排一帧（实测 WS 空闲 59% CPU，
    QEMU 侧 virtio-gpu 也被打到 100%）。scene 本来就会在有 damage 时自己排帧（tinywl 同款），
    这行是多余的自激循环。去掉后 **WS 空闲 1.5%**（load 0.02）。
    另加指针坐标去重：绝对指针（tablet）静止时会持续上报同一坐标，重复投递等于每秒
    给原生桌面制造 60 次 damage。
11. **frame 调度要「present 时踢一次」，不能无条件也不能完全不做**：
    无条件 → 空闲 60Hz 空转（59% CPU）；完全不做 → 第一次绘制要等下一次输入才上屏
    （「鼠标移进画面才出来」）。现在 `request_frame_if_needed()` 只在两条 present 路径末尾调用，
    空闲 0.5%。
12. **desktop buffer 的两条路径必须写同一块**：`present_desktop`（全量）和
    `present_desktop_rects`（脏区）一个交替一个固定，会让 wlr_buffer 指向内容不一致的两块，
    实测直接全黑。统一固定 `buf_a` + 首帧 `needs_full` 全量。
13. **GTK4 双标题栏**：`zxdg_decoration_manager_v1` 已强制 SERVER_SIDE，但 GTK4 本身不支持 SSD，
   只要应用显式用了 `GtkHeaderBar` 就永远自绘 —— 与 WaylandHost 行为一致，不算回归。

---

## 4. 运维：怎么切、怎么回滚

**当前 VM 状态**：已经切到 wlroots 路径并在跑。

```bash
# drop-in（/etc/systemd/system/serenity-desktop.service.d/wlroots.conf）
[Service]
Environment=SERENITY_PRESENTER=wlroots
Environment=SERENITY_NO_CURSOR=1
```

前提：`seatd` 运行中且会话用户在 `video` 组（`systemctl status seatd`、`getent group video`）。
**不要**设 `LIBSEAT_BACKEND=builtin`（见 §3.2）。

这个 drop-in 已经**固化进 `scripts/port/05-install-prefix.sh`**：默认
`--presenter=wlroots` 会写它，`--presenter=kms` 会删掉它。所以重建 VM /
重跑安装脚本不会丢配置。VM 里另有一份手写的同内容文件（等价）。

回滚到原 KMS 路径：

```bash
sudo rm /etc/systemd/system/serenity-desktop.service.d/wlroots.conf
sudo systemctl daemon-reload
sudo install -m755 /serenity/bin/serenity-window-server.bak-kms /serenity/bin/serenity-window-server
sudo install -m755 /serenity/libexec/serenitid.real.bak-kms /serenity/libexec/serenitid.real
sudo systemctl restart serenity-desktop
```

---

## 5. 仍未解决（按优先级）

1. **Xwayland 生产接线**：`wlroots` 自己 spawn Xwayland 后 `DISPLAY` 只在 WS 进程里，
   而 serenitid 给应用导出的是 `SERENITY_APP_DISPLAY=:100`。需要把真实 display 号回传会话
   （或固定 `-displayfd` 值），否则从 shell 启动的 X11 应用连不上。
2. **popup**：`wlr_xdg_popup_unconstrain_from_box` 未接（菜单可能出屏）、`xdg_popup.grab` 的
   键盘焦点未接。
3. **输入细节**：暂时没有 pointer grab/约束、没有触摸板手势、没有多 seat；
   原生窗口的 hover/焦点高亮依赖 ScreenInput 的既有逻辑（本轮回传绝对坐标，行为一致）。
4. **窗口管理**：`on_new_foreign` 目前只在 C++ 侧 `dbgln`，外来窗口还没有进 WS 的窗口列表
   （任务栏条目、拖动、最小化仍不适用于 Linux 应用）。
5. **装饰细节**：CSD 阴影残留 1–2px 亮边；popup/子表面未按 geometry 裁剪。
6. **性能**：桌面 buffer 每帧整幅 memcpy 到 `wlr_buffer`（damage 跟踪只在旧 WaylandHost 路径有）。

---

## 6. 快速复跑命令

```bash
# 宿主编译（改桥/C++ 之后）
cd /mnt/user/serenity
PKG_CONFIG_PATH=/tmp/wlroots-prefix/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig \
  ninja -C Build/lagom serenity-window-server serenitid

# 部署（生产路径）
scp -i Build/vm/id_ed25519 -P 2222 Build/lagom/bin/serenity-window-server serenity@127.0.0.1:/tmp/ws-new
ssh … 'sudo systemctl stop serenity-desktop; sudo install -m755 /tmp/ws-new /serenity/bin/serenity-window-server;
        sudo systemctl start serenity-desktop'

# headless 冒烟（不影响线上桌面；不碰输入）
SERENITY_PRESENTER=wlroots WSWLR_HEADLESS=1 WLR_RENDERER=pixman \
  WSWLR_SHOT=/tmp/shot.ppm WSWLR_SHOT_DELAY_MS=8000 /tmp/ws-wlr-test2
# 截图：scp /tmp/shot.ppm → convert shot.ppm shot.png

# 真机桌面截图（VNC，勿用 QEMU screendump）
python3 Build/vm/vncshot.py "shot:/tmp/x.ppm" && convert /tmp/x.ppm /tmp/x.png
```

---

## 7. 当前 VM 现场

- `serenity-desktop` **以 wlroots presenter 运行中**（`wayland-serenity-0` 由 WS 自己提供）。
- `serenity-wayland-host` 不再启动（serenitid 已按 presenter 分支跳过）。
- 备份：`/serenity/bin/serenity-window-server.bak-kms`、`/serenity/libexec/serenitid.real.bak-kms`。
- 日志：`/tmp/serenity-session.log`（含 `ws-wlr:` 前缀的桥日志与 wlroots 自身 DEBUG）。
