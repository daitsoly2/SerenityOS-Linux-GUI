# 主题引擎现状 & CSS 化可行性（2026-11-09）

> 结论先说：**能做 CSS 方言的“令牌层”，不建议做完整 CSS 渲染引擎。**
> 现有 24 个主题、3 套窗口框 painter 和整套复古皮肤都能保留；
> 真正要改的是「主题怎么写」，而不是「控件怎么画」。

---

## 1. 现在的主题引擎是什么机制

### 1.1 一条链路

```
Base/res/themes/<Name>.ini          ← 作者写的 7 段配置
        │  Gfx::load_system_theme()  (LibGfx/SystemTheme.cpp)
        ▼
struct SystemTheme {                ← 定长 POD，放进匿名缓冲（共享内存）
    ARGB32 color[ColorRole::__Count];       // ~50 个颜色角色
    TextAlignment alignment[...];           // 标题对齐
    WindowThemeProvider window_theme[...];  // 3 选 1 的枚举
    bool flag[...];                         // BoldTextAsBright / IsDark / TitleButtonsIconOnly
    int metric[...];                        // BorderThickness / BorderRadius / TitleHeight / ...
    char path[...][256];                    // 阴影图、按钮图标目录、配色方案
};
        │  AnonymousBuffer（memfd）
        ▼
WindowServer 持有 → 每个客户端 async_update_system_theme(buffer)
        ▼
Gfx::PaletteImpl::theme()  ← 零拷贝，直接按偏移读同一块共享内存
        ▼
控件 paint_event() 里 palette.color(ColorRole::Button)、palette.metric(...)
```

关键点：

- **不是样式表，是一块定长结构体**。所有“主题”最终都塌缩成
  `color[50] / metric[6] / flag[3] / path[8] / alignment[1] / window_theme[1]`。
- **分发是整块广播**：`ConnectionFromClient::notify_about_theme_change()` →
  `async_update_system_theme(Gfx::current_system_theme_buffer())`。
- **取色是数组下标**：`Palette::color(role)` 就是 `Color::from_argb(theme().color[(int)role])`。
- **窗口外观不在数据里**：`WindowThemeProvider` 只有
  `Classic / RedmondPlastic / RedmondGlass` 三个枚举，对应
  `ClassicWindowTheme.cpp` / `PlasticWindowTheme.cpp` / `GlassWindowTheme.cpp`
  三个手写 painter（各 ~220–260 行）。INI 只能选其中一个，不能描述“长什么样”。
- **控件也不查样式表**：每个 `Widget::paint_event()` 自己决定画什么，
  只是颜色/度量从 `Palette` 里取。没有 selector、没有继承、没有状态伪类。

### 1.2 INI 的 7 段

| 段 | 内容 |
|---|---|
| `[Menu]` | 显示名 |
| `[Window]` | `WindowTheme=Classic/RedmondPlastic/RedmondGlass` |
| `[Colors]` | ~50 个颜色角色（`Window`、`Button`、`MenuSelection`、`SyntaxKeyword`…） |
| `[Alignments]` | `TitleAlignment` |
| `[Flags]` | `BoldTextAsBright`、`IsDark`、`TitleButtonsIconOnly` |
| `[Metrics]` | 边框厚度、圆角、标题栏高度、按钮宽高、非激活透明度 |
| `[Paths]` | 阴影图、标题按钮图标目录、配色方案、overlay 阴影 |

### 1.3 为什么它“够用”但“不好写”

- 加一个新颜色角色 = 改 `SystemTheme.h` 的宏 + 重编**所有** GUI 客户端，
  因为 `SystemTheme` 是 ABI 的一部分（和 `WindowServer.ipc` 的 MessageID 同理）。
- 想做一个“Win7 半透明玻璃 + 自定义阴影 + 不同圆角”的主题，
  必须新写一个 `WindowTheme` 子类并加进枚举——主题作者无法只靠配置文件完成。
- 没有变量/复用：24 个 INI 里同一组颜色被抄了很多遍。

---

## 2. “CSS 主题引擎”到底指什么

| 方案 | 含义 | 落在本项目的改动 |
|---|---|---|
| **A. CSS 作为编写格式** | 主题作者写 `.css`（变量、注释、`calc`、颜色函数），**编译**成现有 `SystemTheme` | 只加一个编译器/加载器，ABI 不动 |
| **B. CSS 作为样式规则** | 运行时有选择器（`button:hover`、`.menubar`），控件向样式表查询颜色/内边距 | 需要给 `Widget` 加 `style_class` + 一个 `ThemeSheet`，控件仍自绘 |
| **C. 完整 CSS 渲染** | box model、`display`、`flex`、`box-shadow`、`border-image`、过渡动画都由 CSS 驱动 | 等于在 LibGUI 里重写一套样式/布局/绘制引擎 |

### 结论

- **A 值得做，成本低、收益立刻可见**（变量、复用、热重载、主题作者不用碰 C++）。
- **B 可以做增量**，但只建议覆盖“颜色/内边距/字体”这类标量，
  不要碰布局。
- **C 不建议**：
  1. `SystemTheme` 是共享内存里的定长 POD，任意 CSS 值放不进去；
     要么每个客户端重编（ABI 爆炸），要么另开一套版本化 IPC。
  2. LibGUI 是 immediate-mode 自绘，每个控件的 `paint_event()` 都是一段 C++。
     CSS 要生效必须让这些绘制代码改成“查询计算样式再画”，
     等于把 60+ 个控件重写一遍。
  3. 这套界面的“复古感”来自像素级位图/九宫格/3D 边框（Vista 皮肤、TTPlayer 皮肤），
     纯 CSS 表达不了，最后还是要 `border-image: url(...) 4 fill / ...` 这类扩展。
  4. LibWeb 里虽然有完整 CSS parser，但它和 DOM/Style/Layout 强绑定，
     链进 WindowServer/LibGUI 的体积和启动成本都不可接受。

---

## 3. 推荐方案：CSS 方言 → 现有 token

### 3.1 语法（只做子集，够用即可）

```css
@theme {
  name: "Redmond Vista";
  menu-name: "Redmond &Vista";
  window-theme: redmond-glass;   /* classic | redmond-plastic | redmond-glass */
  dark: false;
  bold-text-as-bright: true;
}

:root {
  /* 变量可以被后面的规则引用，也可以被其他主题 @import */
  --accent: #ab6e4a;
  --window: #d4d0c8;
  --title-height: 23px;
  --border-radius: 5px;
  --button-icon-dir: url("/res/icons/themes/RedmondGlass/16x16/");
  --active-window-shadow: url("/res/icons/themes/Default/shadow-active.png");
}

colors {
  Window: var(--window);
  Button: var(--window);
  Accent: var(--accent);
  ActiveWindowTitle: white;
  /* … 其余角色同名同 INI … */
}

metrics {
  TitleHeight: var(--title-height);
  BorderRadius: var(--border-radius);
  BorderThickness: 4px;
}
```

- 解析结果仍然是 `SystemTheme`，所以**零 ABI 改动**。
- `var()`、`@import`、十六进制/`rgb()`/`hsl()` 颜色、`px`/裸数字，
  这四样就能覆盖现在 24 个主题的全部写法。
- 可选加一个 `@window-frame { ... }` 段，为将来的 Phase 3 留位置。

### 3.2 落地步骤

| 阶段 | 内容 | 预估 | 风险 |
|---|---|---|---|
| P0 | 写 `theme-compile`（Python 或 LibGfx 内的小工具）：CSS 子集 → `.ini` / `SystemTheme` buffer；把现有 24 个 INI 反向生成一份 `.css` 做对照 | 1–2 天 | 低，纯离线工具 |
| P1 | `Gfx::load_system_theme()` 支持 `.css`（按扩展名分派）；`list_installed_system_themes()` 同时列 `.ini`/`.css`；主题切换逻辑不变 | 3–5 天 | 低，失败可回退 INI |
| P2 | `Widget::set_style_class()` + `ThemeSheet`：只有标量属性（颜色、字体、内边距），控件仍自绘 | 2–3 周 | 中，要定“哪些控件认哪些属性” |
| P3 | 数据驱动窗口框：把 3 个 painter 的硬编码换成 `border-image` + `box-shadow` + `border-radius` + 线性渐变描述 | 1–2 月 | 高，视觉回归要靠截图对比 |

P0/P1 做完，用户就能写：

```css
@import "vista-base.css";
:root { --accent: #2f6fd0; }
colors { MenuSelection: var(--accent); }
```

——这已经解决“主题难写、颜色抄来抄去”的 80% 痛点，而且不碰任何渲染代码。

### 3.3 不建议做的事

- 不要把 CSS 解析放进 WindowServer 的每帧路径（每帧重新计算样式是灾难）。
  解析一次、编译成 token、变更时广播，仍然是现在的模型。
- 不要让 `SystemTheme` 变成变长结构。宁可加第二个 IPC
  （`async_update_theme_extras(ByteString css_blob)`）+ 版本号，
  也不要动那块被所有客户端 mmap 的 POD。
- 不要在 Phase 1 就支持 `flex`/`grid`/`transition`。这套 UI 没有文档流，
  只有绝对布局的窗口和控件。

---

## 4. 一句话回答

**现在的“主题引擎”= 一份定长颜色/度量表 + 3 个编译期窗口绘制器，
不是样式表。**
改成 CSS 完全可行，但正确姿势是「CSS 写、编译成现有 token」，
而不是「运行时解释 CSS 重画整个 UI」。前者一两天就能出原型，
后者是几个月的重写，而且会把复古皮肤体系一起赔进去。
