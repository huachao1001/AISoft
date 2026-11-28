# PyUI 设计

自绘跨平台 UI 库。目标：**发布体积与运行速度接近 C++，用 Python 写作业务层**。
本文只记录决策与理由；编码约束见 `AGENTS.md`。

---

## 1. 分层

```
┌──────────────────────────────────────────────────────────────┐
│ 应用层   app/*.py  —— 标准 Python 源码，构建期由 py2cpp 转译    │
├──────────────────────────────────────────────────────────────┤
│ 绑定层   pyui.*     —— C-API 绑定（spec 驱动 codegen，语言中立） │
├──────────────────────────────────────────────────────────────┤
│ 引擎层   view / layout / widget / painter   —— 平台无关 C++      │
├──────────────────────────────────────────────────────────────┤
│ 渲染层   PaintBackend                                       │
│          ├─ SoftwareBackend   共享光栅器（离线渲染 + 像素测试）  │
│          └─ NativeBackend     GDI/GDI+ │ CoreGraphics │ Cairo │
├──────────────────────────────────────────────────────────────┤
│ 平台层   Platform 门面                                       │
│          window / titlebar / context menu / file assoc / tray │
└──────────────────────────────────────────────────────────────┘
```

**关键：引擎层与渲染层之间只有 `PaintBackend` 一个接口**，换后端不改控件一行。

---

## 2. 为什么有两条渲染路径

| | SoftwareBackend | NativeBackend |
|---|---|---|
| 代码 | 一份，三平台共用 | 每平台一份 |
| 像素 | **三平台完全一致** | 各有差异（AA 算法不同） |
| 离线渲染 | ✅ 不需要显示器/窗口 | ⚠️ 需要平台上下文 |
| 像素测试 | ✅ 可复现 | ❌ 不可复现 |
| 屏幕绘制速度 | 够用（span 填充） | 更快（GDI `BitBlt` 直接呈现） |

所以：

- **离线渲染、截图、打印、缩略图、CI 像素测试** 一律用 SoftwareBackend。
- **屏幕绘制** 用 NativeBackend；它复用同一套 `Painter` 调用序列。
- 两边都必须通过「绘制命令一致」测试，因此观感差异只来自后端光栅化，
  不会来自控件逻辑。

这就是「不同平台绘制结果一模一样」的落点：**命令层严格一致，像素层
在用共享光栅器时严格一致，用原生后端时按平台基线验收**。

---

## 3. 布局：XML → C++，但开发者只看到「名字」

### 3.1 这条边界怎么划

```
assets/layouts/main_frame.xml     ← 源文件，人来写
        │  pyui_layoutc（构建期）
        ▼
build/generated/main_frame.cpp    ← 工厂函数，开发者不看
        │  pyui_add_layouts() 自动注册
        ▼
LayoutInflater::inflate("main_frame")   ← 开发者唯一入口
```

调用方永远只写这两行：

```cpp
auto root = LayoutInflater::inflate("main_frame");
auto* ok = root->findView<Button>("ok");
```

**不生成 `MainFrame` 类，不生成公开成员。** 生成物只有一个匿名命名空间里的
工厂函数加一个注册项：

```cpp
namespace {
pyui::ViewGroup* createMainFrame() {
  auto* root = new pyui::LinearLayout(pyui::Orientation::kVertical);
  auto* crumb = new pyui::BreadcrumbBar();
  crumb->setId(pyui::makeId("crumb"));
  root->addView(crumb, pyui::LayoutParams{}.withSize(
      pyui::LayoutParams::kMatchParent, crumb->px(48)));
  ...
  return root;
}
const pyui::LayoutRegistration kRegMainFrame("main_frame", &createMainFrame);
}
```

这是刻意的：一旦生成 `MainFrame` 并暴露 `crumb` 成员，业务代码就会
`#include "main_frame.h"` 并写 `frame->crumb`。那时它就不再是「按名字取布局」，
而是一份编译期强耦合的 C++ 类——将来若要换成资源文件、热重载或远程下发，
每一处 `frame->crumb` 都得改。反过来，只要业务代码只用 `findView("crumb")`，
背后是生成代码还是二进制资源就完全无所谓。**这是这条边界唯一的意义。**

### 3.2 为什么选 C++ 而不是二进制资源

实测每节点边际开销（同一组假控件，setter 全为 inline 空实现，等价于真实
setter 的下限）：

| 节点数 | 编译成 C++ 语句 | 编译成数据记录 + 循环解释 |
|---|---|---|
| 200 | 28,832 B | 12,867 B（.text 4,440 + .rdata 8,427） |
| 2000 | 632,605 B | 80,417 B（.text 4,440 + .rdata 75,977） |
| **每节点边际** | **≈ 335 B** | **≈ 38 B** |

1000 节点差约 285 KB。但最终的 exe 里嵌着转译后的 Python，那是几百 KB 到
MB 级的开销；布局这 285 KB 只是其中零头。**不值得为它牺牲开发体验。**

选 C++ 换来的实质收益：

- 属性错误由 **C++ 编译器**报出。写 `radius="6`（少了引号）、把 `withSize`
  的第二个参数写成字符串，都是编译错误而不是运行期静默忽略。
- 生成物直接调强类型 setter，没有属性名解析、没有取值域校验函数、没有
  属性描述表进产物——类型系统就是校验器。
- 运行期零解析、零额外数据段。`inflate` 是一次函数指针调用。

### 3.3 `pyui_layoutc` 的责任：错误要在这里报，不要留给 C++ 编译器

C++ 编译器只能报「没有这个成员函数」，报不出「你应该写 `width` 而不是
`wide`」，也给不出 XML 的行列号。所以生成器必须**先**做一遍严格校验：

| 检查 | 报错示例 |
|---|---|
| 标签名存在 | `main_frame.xml:12:3: 未知控件 <Buton>，你是不是要 <Button>` |
| 属性名存在 | `main_frame.xml:14:5: <Button> 没有属性 wide，可用：width/height/...` |
| 取值域合法 | `main_frame.xml:15:5: orientation 只能是 horizontal/vertical` |
| 必填属性齐全 | `main_frame.xml:9:3: <Slider> 缺少 max` |
| id 不重复 | `main_frame.xml:20:7: id="ok" 与第 8 行重复` |
| 根必须是容器 | `main_frame.xml:3:1: 根节点必须是 ViewGroup（LinearLayout/FrameLayout/...）` |

属性清单来自 `pyui_specs`：一个只被工具链接的静态库，为每个控件描述可用
属性、类型、默认值、取值域。它与控件实现必须同源——新增属性时两处一起改，
`tests/test_specs.cpp` 断言「每个控件声明的 setter 都在 spec 里」。应用不链接
`pyui_specs`，所以这张表不进发布产物。

### 3.4 属性写法

XML 的写法尽量贴近 setter，减少心智负担：

```xml
<LinearLayout orientation="vertical" padding="16">
  <TextView id="title" text="选择文件" size="large" bold="true"/>
  <Button id="ok" text="确定" primary="true" width="96"/>
</LinearLayout>
```

生成成：

```cpp
root->setOrientation(pyui::Orientation::kVertical);
root->setPadding(root->px(16));
auto* title = new pyui::TextView();
title->setId(pyui::makeId("title"));
title->setText("选择文件");
title->setTextSize(pyui::TextSize::kLarge);
title->setBold(true);
```

三条规则：

1. **尺寸属性是 96 DPI 设计值**，生成代码里统一包一层 `px()`。这样 XML
   写 `48` 在 150% 屏上是 72 物理像素，不用设计师算两次。
2. **布尔属性写 `true`/`false`**，不写 `1`/`0`。
3. **枚举属性写名字**（`vertical`），不写数字——数字在生成器里做一次校验，
   拼错立刻报错。

### 3.5 改动的影响面

| 操作 | 代价 |
|---|---|
| 改一个布局 | 只重编该布局的生成单元 + 链接（生成器 1 ms/文件） |
| 加一个控件类型 | 改控件实现 + `pyui_specs` 条目 |
| 加一个属性 | 改 setter + `pyui_specs` 条目，XML 立即可用 |
| 布局里写错属性 | 构建失败，报 `file:line:col` |

---

## 4. 平台接口（按需求拆成五个独立接口）

刻意不做一个大而全的 `Platform`：每个能力单独成头文件，缺哪个平台就
只缺那一个实现，其余照常工作。

| 接口 | 头文件 | 职责 |
|---|---|---|
| `Window` / `WindowHost` | `platform/window.h` | 原生窗口、DPI、脏区、输入翻译 |
| `TitleBarAppearance` | `platform/title_bar.h` | 标题栏尺寸、按钮位置、默认配色、字体 |
| `ContextMenuHost` | `platform/context_menu.h` | 原生右键菜单（系统的，不自绘） |
| `FileAssociationHost` | `platform/file_association.h` | 后缀关联 + 关联后的默认图标 |
| `TrayHost` | `platform/tray.h` | 托盘图标与菜单 |
| `Platform` | `platform/platform.h` | 门面：聚合以上 + 剪贴板/字体/路径/事件循环 |

### 4.1 为什么右键菜单不自绘

自绘菜单要自己处理：屏幕边缘翻转、多显示器负坐标、键盘导航、无障碍、
高对比度主题、系统缩放、失焦消失、以及「点菜单外关闭」的鼠标捕获。
这些每个平台都有成熟实现，自绘只会更差。所以 `ContextMenuHost` 是**原生**
的，但菜单**模型**是共享的 `MenuItem`。

### 4.2 为什么标题栏只给「默认值」而不给「实现」

标题栏内容（图标、标题、三个按钮）用通用控件拼装，跨平台一致；各平台
不同的只有：高度、按钮在哪一侧、默认配色、标题字体。

```cpp
struct TitleBarAppearance {
  int bandHeight;                    // 96 DPI 设计值
  int visualHeight;                  // 0 = 等于 bandHeight
  TitleButtonSide buttonsOnSide;     // kRight=Windows，kLeft=macOS
  int buttonWidth, buttonHeight;
  bool roundButtons;                 // macOS 红绿灯
  int contentInset, iconTextGap, iconSize;
  const char* titleFontFamily;
  float titlePointSize;
  Color background, backgroundInactive;
  Color titleInk, titleInkInactive;
  Color buttonHover, buttonDown;
  Color closeHover, closeDown, closeInk;
  Color bottomLine;
  bool drawBottomLine;               // Windows 画，macOS 不画
};
TitleBarAppearance titleBarAppearance();   // 由 src/os/<platform>/ 提供
```

配色也在这里给而不是只从 `Theme` 取：标题栏要在「应用尚未设置主题」时就
已经看起来像本平台的原生窗口，那是这个结构存在的理由。`Theme` 可以覆盖，
但覆盖的是另一个方向——它不该是唯一的来源。

这样标题栏只有「数字、方向、默认色」是平台差异，绘制代码一份。

### 4.3 文件关联与默认图标

```cpp
struct FileAssociation {
  std::string progId;                  // 稳定标识，注册后不可改
  std::string description;             // 「打开方式」里显示
  std::vector<std::string> extensions; // {".zip", ".7z"}
  std::string hostExePath;             // 留空 = 当前进程
};
```

- `registerFileAssociations` / `unregister` / `areRegistered`：语义与 AiUI 一致。
  现代系统不允许静默抢占默认程序，后端只能登记为**候选**并说明系统会提示。
- `associationIcon(extension, pixelSize)`：**关联后**文件在资源管理器里显示
  的图标。必须由平台回答——Windows 从 `ProgID\DefaultIcon` 取，macOS 走
  LaunchServices 文档图标，Linux 从 `.desktop` 的 `MimeType` 反查。
  自绘一个「压缩包」图标是错的：用户要在文件列表里认出**那个文件**。

### 4.4 窗口后端的四个关键决定

这四条决定「屏幕绘制」能不能与「离线渲染」真正同源，以及自绘标题栏能不能
不牺牲系统的窗口行为。

**1. 客户区画布是 DIB + 共享光栅器，不是直接往 HDC 上画。**
`CreateDIBSection` 给一块 32 位内存，`Surface::wrapExternal` 直接包住它，
`SoftwareBackend` 往里画，析构时 `BitBlt` 上屏。这样屏幕与内存是**同一段
代码**；若用 GDI 画图元，像素断言就只能对着另一条路径验收。

**2. 非客户区靠 `WM_NCCALCSIZE` / `WM_NCHITTEST` 让出来。**
不是「去掉 `WS_CAPTION` 再自己画」——后者会丢掉贴边吸附、双击最大化、
Snap Layouts、窗口投影与 Win11 圆角，还得自己实现多显示器 DPI 切换。
让出非客户区只需把客户区顶边抬到窗口顶端（`WM_NCCALCSIZE`），并在
`WM_NCHITTEST` 里回 `HTCAPTION` / `HTLEFT` / `HTMAXBUTTON`，系统把这些
全部接过去。

**这几件事由谁决定，是这一节最容易走偏的地方**：命中结果来自**布局数据**，
不是核心代码里的坐标或 id。控件用 XML 的 `role` 属性声明自己在窗口里干什么：

| role | 命中 | 行为 |
|---|---|---|
| `caption` | `HTCAPTION` | 拖动窗口、双击最大化、贴边吸附 |
| `maximize` | `HTMAXBUTTON` | 贴靠布局预览；点击由我们接管 |
| `minimize` / `close` | `HTCLIENT` | 普通按钮，动作由我们接 |
| `title` / `icon` | 装饰 | 命中穿透到父容器 |

判定只看角色：把 `role="maximize"` 的按钮挪到标题栏最左边、换个 id，命中会
跟着跑到新位置。核心代码里没有任何「0x7A06 是最大化按钮」这样的假设。

**内置标题栏只是默认值，不是必需品。** 不写任何布局也有标题栏（由
`assets/layouts/titlebar.xml` 编进 `pyui_core`）；想自己画，就把
`customTitleBar` 关掉，在自己的布局里给控件写上同样的 role——拖动与三个
按钮照旧可用。**关掉标题栏不等于退回 Windows 系统标题栏**：PyUI 的窗口
在任何配置下都是自绘的，系统标题栏从不出现。

最大化时必须把窗口按边框厚度缩回来：系统会把最大化窗口放大到超出工作区
（让边框藏到屏幕外），不缩就裁掉一圈内容。

**3. 静态 WndProc + `GWLP_USERDATA`，不用 `SetWindowSubclass`。**
窗口类在进程内共享，带 `this` 的 WndProc 会把第一个窗口的对象指针泄漏给
所有窗口。子类化能解决，但要额外链 comctl32，且 `WM_NCCREATE` 时机还得
单独处理。`GWLP_USERDATA` 在 `WM_NCCREATE` 里写一次，比子类化少一层依赖。

**4. 文字自己栅格化，不用 `ExtTextOut`。**
`ExtTextOut` 只能画进 HDC，一旦用它，文字就绑死在 GDI 表面上。改用
`GetGlyphOutline(GGO_GRAY8_BITMAP)` 拿覆盖率矩阵，喂给
`PaintBackend::blendCoverageMask`——屏幕上和内存里走同一条路径。
GRAY8 是 0..64 共 65 级，扩到 255 用 `(v*255+32)/64`（直接乘 4 会在 64 时
溢出成 0）。

`DwmExtendFrameIntoClientArea` 通过 `GetProcAddress` 探测，不静态链
dwmapi：留 1px 不可见非客户区，DWM 才给窗口画投影与圆角。

---

## 5. 控件清单（纯绘制）

| 控件 | 说明 | 测试 |
|---|---|---|
| `TextView` | 单行/多行文本、省略号、对齐 | `test_widget_text_view.cpp` |
| `Button` | 圆角、悬停/按下/禁用、强调色 | `test_widget_button.cpp` |
| `CheckBox` | 勾选框 + 标签、三态预留 | `test_widget_check_box.cpp` |
| `RadioButton` | 圆点 + 标签、组内互斥 | `test_widget_radio_button.cpp` |
| `TextField` | 光标、选区、占位提示、滚动 | `test_widget_text_field.cpp` |
| `ProgressBar` | 确定/不确定两种 | `test_widget_progress_bar.cpp` |
| `Slider` | 轨道 + 滑块 + 步进 | `test_widget_slider.cpp` |
| `TitleBar` | 由 `TitleBarAppearance` 驱动的跨平台标题栏 | `test_widget_title_bar.cpp` |

全部自己画。禁止原生子控件，理由见 `AGENTS.md` 铁律二；对应的自动化检查在
`test_widget_no_native.cpp`（含「公开头文件不出现平台类型」的源码扫描）。

---

## 6. 测试策略：三层

| 层 | 内容 | 判定 | 跨平台 |
|---|---|---|---|
| **L1 行为** | 布局结果、命中测试、脏区、属性解析、资源加载 | 精确相等 | 必须逐位相同 |
| **L2 绘制命令** | 每个控件的 `RecordingPainter` 调用序列 | 精确相等 | **必须逐位相同** |
| **L3 像素** | `SoftwareBackend` + `MockTextRenderer` 渲染后断言不变量 | 结构断言 + 可选哈希基线 | 用共享光栅器时相同 |

L3 的哈希基线放在 `tests/golden/<platform>/`，`PYUI_UPDATE_GOLDEN=1` 生成。
没有基线文件时跳过哈希比对，但**不变量断言照常执行**——这样新控件可以先
有测试再补基线，不会因为「还没生成基线」而整个用例失效。

为什么不用真实字体做 L3：字形形状由平台字体决定，必然不同。用
`MockTextRenderer`（固定步进、方块字形）把「文字形状」这个变量固定住，
剩下的几何/配色/对齐就可以跨平台逐像素比对。

---

## 7. 体积与性能预算

以下是**实测值**（MSVC 14.44 / x64 / Release / `/MT` / `/OPT:REF,ICF`），
归因由 `tools/size_probe` 逐层链接得出，不是估算：

| 层次 | 内容 | exe 体积 | 边际增量 |
|---|---|---|---|
| LEVEL=0 | 空 `wWinMain`（仅静态 CRT） | 98.0 KB | — |
| LEVEL=1 | + 窗口、布局、脚本基类 | 385.5 KB | +287.5 KB |
| LEVEL=2 | + Button / TextView | 387.0 KB | +1.5 KB |
| LEVEL=3 | + Value 与脚本内建 | 387.0 KB | +0.0 KB |

实测的真实示例（含完整控件与业务逻辑）：

| 示例 | 体积 | 用到的扩展 |
|---|---|---|
| fileviewer | 416.5 KB | fs |
| progress | 422.5 KB | — |
| calculator | 437.0 KB | — |
| notes | 458.5 KB | fs |
| settings | 474.0 KB | fs |

**归因结论**（三条都反直觉，但都是实测）：

1. **静态 CRT 就是 98 KB 的固定地板**。任何 `/MT` 的 Windows 程序都躲不掉，
   不属于 PyUI 的成本。
2. **窗口 + 平台后端 + 布局系统占大头（≈288 KB）**。这层是自绘窗口、
   GDI 后端、DPI、输入法、拖放、脏区管理的总和，没有一项能省。
3. **控件几乎不花钱**。加 Button + TextView 只涨 1.5 KB，因为自绘控件只是
   `onDraw` 里的几个 `Painter` 调用；真正贵的是它依赖的平台层，而平台层
   无论如何都得在。

因此 300–500 KB 的目标**已经达成**，不需要为体积放弃 `/MT`、RTTI、异常
或自绘——这四样本来就不是主要开销。

对照 AiZip（C++ + GDI+）2.54 MB。差距主要来自 AiZip 用 GDI+ 与更多
标准库设施；PyUI 自己的软件光栅器只有几十 KB。

> 复现方式：
> ```
> cmake -S . -B build/size -DPYUI_BUILD_SIZE_PROBE=ON -DPYUI_BUILD_EXAMPLES=OFF
> cmake --build build/size --target size_probe_0 size_probe_1 size_probe_2 size_probe_3
> ```

性能目标：

| 项 | 目标 |
|---|---|
| 布局实例化（100 节点） | < 50 µs（纯 `new` + setter，无解析） |
| 整窗重绘 1080p | < 4 ms |
| 局部重绘 200×400 | < 0.3 ms |
| 悬停帧率 | 60 fps |

---

## 8. 当前实现状态

| 项 | 状态 |
|---|---|
| 类型 / 主题 / 事件 / id | ✅ |
| Surface + 共享光栅器（矩形/圆角/描边/渐变/棋盘/裁剪/变换） | ✅ |
| Painter 组合算法（省略号/图标居中） | ✅ |
| View / ViewGroup / FrameLayout / LinearLayout | ✅ |
| `LayoutInflater`（名字 → 工厂） | ✅ |
| `pyui_layoutc`（XML → C++）+ `pyui_specs` 属性清单 | ✅ |
| 控件：TextView/Button/CheckBox/RadioButton/TextField/ProgressBar/Slider/TitleBar | ✅ |
| 测试：L1 / L2 / L3 + 每控件独立用例 | ✅ 249 用例 |
| `TextRenderer` 接口 + `MockTextRenderer` | ✅ |
| Windows：标题栏默认值 / 菜单 / 关联 / 托盘 | ✅ |
| Windows：GDI 文本引擎（`GetGlyphOutline` GGO_GRAY8） | ✅ |
| Windows：Win32 窗口后端（DIB 画布 / 非客户区让出 / 输入法 / 拖放） | ✅ |
| Windows：`Platform` 门面（`platform()` / 剪贴板 / 路径 / 事件循环） | ✅ |
| `pyuic`（Python → C++）+ 四层语义守护 | ✅ 见 `docs/SUPPORT.md` |
| macOS / Linux 后端 | 🚧 仅接口，未实现 |

测试分四个 CTest 条目，**不能混链**：

| 脚本 | 内容 | 用例数 |
|---|---|---|
| `build/build_tests.bat` | 平台无关层，链接 `tests/test_platform_stubs.cpp` 的假标题栏 | 160 |
| `build/build_win_tests.bat` | 真后端（含菜单/托盘/关联，验证链接完整性） | 36 |
| `build/build_cmake.bat` | CMake 全量：`pyui_tests` + 三个编译器自检 | 249 + 3 |

`titleBarAppearance()` 在两个套件里各有定义，同时链接会重复定义——这是
「平台无关测试不该依赖真实 DPI」的必然结果，不是缺陷。

### 编译器自检（CTest 里的另外三条）

`pyuic` 本身错了照样能编过（错的 C++ 也是 C++），所以生成器必须自己被测：

| CTest 名 | 脚本 | 守护什么 |
|---|---|---|
| `pyuic_syntax` | `tools/pyuic/check_syntax.py` | 53 个语法用例：能翻译 / 必须报错 |
| `pyuic_scope` | `tools/pyuic/check_scope.py` | 局部量集合与 CPython `symtable` 对拍 |
| `pyuic_value_semantics` | `tools/pyuic/check_value_semantics.py` | 运行时语义的期望值拿 CPython 当基准 |

能力边界（A/B/C 三类）写在 `docs/SUPPORT.md`。加新语法时的规矩是：先在
`check_syntax.py` 里写一条期望，再改生成器——这样「它属于 A 类还是 C 类」
必须当场想清楚。
