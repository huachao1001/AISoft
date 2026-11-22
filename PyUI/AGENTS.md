# PyUI 开发约定

PyUI 是自绘 UI 库。与 AiUI 的关系是「同一套绘制思路，不同的资源与平台边界」。
本文只写**必须遵守的铁律**，想论证过程看 `DESIGN.md`。

## 与 AiUI 的三处根本差异

动手之前先记住这三点，它们决定了这里不能照抄 AiUI：

| | AiUI | PyUI |
|---|---|---|
| 布局资源 | XML 在构建期编译成 C++，生成带成员的布局类 | XML 在构建期编译成 C++，**只生成工厂函数**，按名字取 |
| 字符串 | `std::wstring` | **`std::string`（UTF-8）**，只在平台边界转 UTF-16 |
| 控件测试 | 逻辑测试 | **每个控件一份绘制测试**，跨平台结果必须一致 |

## 铁律一：公开头文件不得出现平台类型

`include/pyui/` 下**任何**头文件都不允许出现 `HWND`、`HDC`、`HBITMAP`、
`HBRUSH`、`COLORREF`、`POINT`、`RECT`、`MSG`、`CGContextRef`、`cairo_t`、
`NSView*`，也不允许 `#include <windows.h>`。

- 需要句柄时用 `pyui::NativeHandle`（`void*`），含义由后端解释。
- 矩形/点/颜色一律用 `pyui::Rect` / `Point` / `Color`。
- 平台调用一律走 `platform/` 下的抽象接口。

一个例外就会让第三个平台的移植多一道全局改动，不要开口子。

## 铁律二：控件是纯绘制，不得创建原生子控件

「表单控件（纯绘制）」是硬性要求，不是优化项：

- 按钮、复选框、单选框、输入框、进度条、滑块**全部自己画**。
- 不允许 `CreateWindowExW(L"BUTTON", ...)`、`NSButton`、`gtk_button_new`。
- 整个界面树只有一个原生窗口；子视图没有任何句柄。
- 焦点、光标、输入法由 `ViewRoot` 统一管理，不交给系统控件。

理由：原生控件的外观由系统版本决定，跨平台必然不一致；而且句柄一多，
脏区、裁剪、DPI 就都要逐控件重做一遍才对齐。这一条一旦破例，铁律一的
「一行平台代码不落地到控件」也跟着失效。

对应的自动化检查见 `tests/test_widget_no_native.cpp`：控件树里除根之外
不得出现非空的 `NativeHandle`。

## 铁律三：每个控件必须有独立的绘制测试

新增控件时**同时**新增 `tests/test_widget_<name>.cpp`，不允许合并进公共测试。
两条断言都必须有：

1. **绘制命令一致**：用 `RecordingPainter` 记录全部绘制调用，与期望序列比对。
   这一条是**跨平台必须逐字节相同**的——控件的绘制意图不能因平台而异。
2. **像素不变量**：用 `SoftwareBackend` + `MockTextRenderer` 渲染到 `Surface`，
   断言结构性质（圆角外透明、悬停色不同、文字块落在行框内、无越界绘制）。

为什么分两条：绘制命令证明「控件想画什么」在三个平台一致；像素不变量证明
「画出来的东西是对的」。真实字形由平台 `TextRenderer` 提供，形状必然不同，
所以像素级**完全一致**只在用 `MockTextRenderer` 时成立——这是设计选择，
不是妥协，见 `DESIGN.md` 的「一致性三层」。

## 铁律四：布局按名字取，开发者不感知底层形态

`assets/layouts/*.xml` 是源文件，构建期由 `pyui_layoutc` 编译成 C++。但对开发者
只暴露一个入口：

```cpp
auto root = LayoutInflater::inflate("main_frame");
auto* btn = root->findView<Button>("ok");
```

**两条硬性约束，缺一条就会让底层形态泄漏到业务代码：**

1. **不生成带具名成员的布局类**。不要出现 `MainFrame` 这种类，更不要有
   `mainFrame->crumb` 这种公开成员。一旦生成，业务代码就会 `#include
   "main_frame.h"`，那时把布局换成资源文件就要改遍所有业务代码——
   「无法感知」也就没了。
2. **`inflate` 只认布局名**（XML 文件主名，不带扩展名）。名字与控件树的对应
   关系由构建期注册表决定，运行期只做一次查表。

因此 `LayoutInflater` 的签名**永远不该因为这个实现选择而变化**。将来若要做
热重载（dev 直读 XML）或远程下发布局，只换注册表背后填的东西，业务代码一行
不改。这是这条铁律真正的目的。

### 为什么选 C++ 而不是二进制资源

实测每节点边际开销：C++ 语句 ≈ 335 B，数据记录 ≈ 38 B。1000 节点相差约
285 KB，但相比「exe 里嵌 Python」那几百 KB 到 MB 级开销只是零头，**不值得为
体积牺牲开发体验**。选 C++ 的真正理由是：

- 属性错误在**编译期**报出，带行号，而不是运行期静默忽略；
- 生成的代码直接调 `setText(...)` 这类强类型 setter，没有字符串解析、
  没有属性哈希表、没有取值域校验——**类型系统替你校验**；
- 运行期零解析开销、零额外数据段。

### 代价与对应的补救

| 代价 | 补救 |
|---|---|
| 改布局要重新编译 | 每个布局独立编译单元，改一个只重编一个；生成器 1 ms/文件 |
| 布局不能单独替换、不能热重载 | 接受。若将来要做，只在 dev 构建里加一条直读 XML 的路径，业务代码不变 |
| 离线渲染要先有生成代码 | 预览工具链接同一份生成目标，不需要窗口 |

`pyui_layoutc` 还必须做**前置严格校验**：标签名、属性名、属性取值域全部对着
`pyui_specs`（控件属性清单）核对一遍，错了就构建失败并给出 `file:line:col`。
不能把校验推迟到 C++ 编译器——编译器只会报「没有这个成员函数」，报不出
「你应该写 width 而不是 wide」。

属性名清单与控件实现必须同源，靠 `pyui_specs` 这个只被工具链接的目标保证：
应用不链接它，所以属性描述表不会进发布产物。

## 铁律五：widget 只依赖 Painter，不依赖后端

- 控件的 `onDraw` 只能调用 `Painter` 的方法。
- 不允许 `#ifdef _WIN32`、不允许直接调 GDI/Cairo/CoreGraphics。
- `Painter` 之上的组合算法（省略号、图标墨迹居中、棋盘格相位）写在
  `src/painter.cpp`，三平台共用一份；只有「碰像素或碰字形」的才进后端。

## 铁律六：像素语义八条（三后端必须一致）

缺任何一条都会出现「同一份设计稿在三个平台看起来不一样」的细微 bug。

| # | 约定 |
|---|---|
| 1 | `Rect` 右下边界**开区间**；`{0,0,10,10}` 是 100 像素 |
| 2 | 填充取整：左/上 `floor`，右/下 `ceil`——相邻视图不留缝、不丢像素 |
| 3 | 描边**向内**，`lineWidth=1` 恰好占 1 像素 |
| 4 | 圆角半径夹到 `min(w,h)/2` |
| 5 | `drawGradientRect` 严格垂直线性，`top` 对应 `rc.top`，含首末行 |
| 6 | 棋盘格相位锚在**坐标系原点**，不锚在 `rc` |
| 7 | 裁剪求交；`save/restore` 必须配对；圆角裁剪按行内插 |
| 8 | 文本 `rc` 是行框，垂直居中；`drawTextRun` 原点即 `rc.left` |

## 铁律七：文字四处同源

`measureText` / `drawText` / `drawTextRun` / `advanceText` 必须用同一套排版。
任何一处单独实现，输入框的光标就会与文字错开半个留白。这条是 AiUI 踩过
的坑，原样继承。

## 命名与注释

- 注释、文档用中文，只说「为什么」。
- 函数动词开头，名词首字母大写 camelCase，与 AiUI 保持一致。
- 常量 `k` 前缀；成员变量 `_` 后缀（与 AiUI 一致）。
- 尺寸参数一律是 **96 DPI 设计值**，控件内部用 `px()` 换算。

## 构建与测试

三个脚本，各有边界，不要互相替代：

| 脚本 | 内容 |
|---|---|
| `build/build_tests.bat` | 平台无关层；链接 `tests/test_platform_stubs.cpp` 的假标题栏 |
| `build/build_win_tests.bat` | 真实 Windows 后端，含菜单/托盘/关联（顺带验证链接完整性） |
| `build/build_cmake.bat` | CMake 全量构建；Windows 上自动接真实后端 |

`build_cmake.bat` 跑的 CTest 里除 `pyui_tests` 外还有三条**编译器自检**，
它们检查 `pyuic` 自己：`pyuic_syntax`（语法覆盖）、`pyuic_scope`（作用域）、
`pyuic_value_semantics`（运行时语义）。生成器错了照样能编出合法 C++，
所以这三条不能省。能力边界见 `docs/SUPPORT.md`。

### 构建期需要 Python

`pyuic` 与 `pyui_layoutc` 的语义校验都跑在 CPython 上（`ast` / `symtable`），
所以**构建期需要 Python 3.8+**，运行期不需要。本机没有全局 `python`，
脚本默认用 `D:\Software\miniconda3\envs\ys\python.exe`；要换解释器就设
`PYUI_PYTHON`：

```bat
set PYUI_PYTHON=C:\Python312\python.exe
build\build_cmake.bat
```

CMake 侧对应变量 `-DPYUI_PYTHON=...`；不设时先看 `PYTHON`，再找
`Python3_EXECUTABLE`，都没有就报错并提示怎么指定。

**两个 `.bat` 不能混链**：`titleBarAppearance()` 在桩与真后端里各有定义，
同时链接就是重复定义。CMake 已经按 `if(WIN32)` 自动选对，手写脚本不要跨过去。

`src/os/windows/` 的每个 TU 都要在 `build_win_tests.bat` 里编到，包括测试
根本不调用的菜单、托盘、文件关联——**缺符号只有在应用第一次链接时才暴露**，
那时才发现已经太晚。

## 编辑中文文件的硬性要求

**不要用 PowerShell 的 `-replace` / `Set-Content` / `Get-Content -Raw` 改含中文的
文件**。它们会按控制台代码页（GBK）解码 UTF-8，把中文和相邻的引号一起弄坏，
而且损坏是静默的——文件仍然能读，只是内容已经变了。

改中文文件只用 `edit` / `write` 工具。确实需要脚本处理时，必须显式指定编码：

```powershell
$c = Get-Content file -Raw -Encoding UTF8
[System.IO.File]::WriteAllText(path, $c, (New-Object System.Text.UTF8Encoding($false)))
```

改完看一眼。判断 XML 有没有被改坏，最快的办法是重跑生成器：
`pyui_layoutc` 的校验会立刻报出引号丢失这类结构错误。
