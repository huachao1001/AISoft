# AiUI 开发约定

AiUI 是跨平台自绘 UI 库。当前只实现了 Windows 后端，**未来要接入 macOS 与
Linux**——所有设计决策都要按这个前提审查。

## 铁律：公开头文件不得出现平台类型

`AiUI/include/` 下**任何**头文件都不允许出现 `HWND`、`HDC`、`HBRUSH`、
`WNDPROC`、`HINSTANCE`、`UINT`、`RECT`、`POINT`、`COLORREF`、`MSG` 等
Windows 类型，也不允许 `#include <windows.h>`。

- 需要句柄时用 `AiUI::NativeHandle`（`void*`），由后端负责解释含义。
- 需要矩形/点/颜色时用 `AiUI::Rect` / `Point` / `Color`。
- 平台调用一律走 `AiUI::Platform` / `PlatformWindow` / `PaintBackend` 三个抽象。

历史上 `widgets/dialog.h` 曾以"宿主接入层"为名例外暴露 `HWND`，已删除。不要再
以任何理由开口子——一个例外就会让 macOS 移植多一道全局改动。

## 分层：`src/os/<platform>/` 是唯一碰操作系统的目录

```
include/AiUI/          公开接口，平台无关
src/                   通用实现（视图树、布局、控件），不得出现系统 API
src/os/windows/        Windows 后端：窗口、GDI+ 绘制、图标、标题栏外观、外壳集成
src/os/macos/          预留（尚未实现）
src/os/linux/          预留（尚未实现）
```

- 换平台 = 新写一份 `src/os/<platform>/`，实现 `PaintBackend`、`Platform`、
  `PlatformWindow`、`titleBarDefaults()`，外加窗口消息到中性事件的翻译。
  `CMakeLists.txt` 里按 `WIN32` / `APPLE` / `UNIX` 选目录。
- **`src/` 顶层与 `src/widgets/` 不允许 `#include <windows.h>`，也不允许调用
  任何系统 API**（剪贴板、路径比较、字体族、DPI 都要走 `Platform` 接口）。
- `#ifdef _WIN32` 不应出现在通用实现里。

### 接口在外、实现在内

系统相关能力一律**先在 `include/AiUI/` 定义平台无关接口，再在
`src/os/<platform>/` 实现**，两者同名同义。新平台只需补实现，公开接口与
调用方一行不改。

已有对应关系：

| 公开接口 | Windows 实现 |
|---|---|
| `Platform`（`platform.h`） | `window_windows.cpp` 转发到 `platform_windows.cpp` |
| `ShellVerb` / `registerShellVerbs` | `platform_windows.cpp` |
| `ShellMenuHandler` / `aiuiBuildShellMenu` | `shell_menu_windows.cpp`、`shell_menu_register.cpp` |
| `TrayIcon` | `platform_windows.cpp` |
| `titleBarDefaults()` | `title_bar_windows.cpp` |

**应用工程里不允许出现 `#include <windows.h>`、注册表、COM、`HWND` 之类的
东西。** 应用只能：
1. 在公开头里挑一个描述结构填参数（如 `ShellMenuHandler`），
2. 实现一个纯逻辑函数（如 `aiuiBuildShellMenu`），
3. 调用 `AiUI::` 命名空间下的封装函数。

反例：把释放 DLL、写注册表、拼 `IContextMenu` 的代码放在
`AiZip/src/app/shell_menu.cpp` 里——那是把后端代码写到了应用层，换平台时
整份作废，且应用被迫链接 `shell32` / `ole32`。正确做法是只留一份
`ShellMenuHandler` 描述加一个纯菜单模型。

平台特有的构建产物（如 Windows 的进程内 COM 组件）也由 AiUI 的 CMake
函数生成（`aiui_add_shell_menu_dll`），应用只声明源文件与标识。


## 标题栏由通用控件拼装

`TitleBar` 就是一个 `FrameLayout`：

```
TitleBar (FrameLayout)
└── band : LinearLayout(水平)   贴顶，高度即"逻辑高度"
    ├── ImageView   应用图标
    ├── TextView    标题
    ├── View        弹簧
    └── LinearLayout 三个系统按钮
```

- **逻辑高度** = band 高度：决定按钮悬停底色高度、图标与标题的垂直居中
  基准，也是可拖动区域。`TitleBar::setBandHeight` 控制。
- **视觉高度** = 标题栏实际总高，`TitleBar::setHeight` 控制；0 表示等于
  逻辑高度。多出来的部分留给宿主自绘，超出即裁剪不显示。
- 所有部件都带 id（`kTitleBarIdBand` 等），宿主用 `findViewById` 拿到后即可
  改样式、换配色、加子视图，**不要**为了改外观去继承 `TitleBar`。
- 平台差异只有数字与按钮在哪一侧，收在 `TitleBarDefaults` + `titleBarDefaults()`
  （实现在 `src/os/windows/title_bar_windows.cpp`）。

## 事件与坐标

- `MouseEvent::position` 永远是**当前视图的局部坐标**，平台后端负责先转成
  窗口坐标，`ViewRoot` 再转到命中视图的局部坐标。
- 平台后端把原生消息翻译成 `WindowHost` 上的中性回调后，不得再做业务判断。
- 非客户区（标题栏拖动、边缘缩放）用 `HitTarget` 表达，由 `WindowHost::onHitTest`
  回答。Windows 后端把它映射成 `WM_NCHITTEST` 返回值，其它平台映射成各自的
  拖动/缩放机制。

## 窗口边框策略

保留系统外框（`WS_CAPTION | WS_THICKFRAME`），只在 `WM_NCCALCSIZE` 里把标题栏
那一条划给客户区——这样投影、圆角、贴边吸附、边缘缩放全部由系统提供，只需
自绘标题栏内容。不要改成 `WS_POPUP` 全自绘，否则这些都要自己重做。

## 窗口与对话框

- `Window` 是窗口组件（标题栏 + 内容区），`TitleBar` 是自绘标题栏。
  不要新增直接 `#include <windows.h>` 的窗口封装。
- **应用界面一律写成 `Activity` 子类**：只声明 `layoutName()`，在
  `onCreate()` 里用 `findView<T>("id")` 取控件挂回调。不要在 `onCreate`
  之外的模块里散落文件级全局控件指针——那就是没有 Activity 的后果。
- 复杂逻辑不塞进 `Activity`：抽成普通工具类，`Activity` 只做装配。
- XML 的 `id` 由 `layoutgen` 生成 `setId(makeId("..."))`；`findView` 按
  字符串查，整数在编译期算好，不要手工写数字 id。
- 模态/非模态由 `Window::showModal` / `show` 提供，模态循环在平台层
  （`runModalLoop`），因为各平台锁父窗口的方式不同。
- 关闭回调返回 `false` 表示拒绝关闭；`Activity::onCloseRequest` 同理。
- 主窗口销毁由平台层投递退出信号；带 owner 的窗口是对话框，销毁不退出。
- 应用侧原生对话框（压缩/解压/进度等）保留系统控件，其 Win32 辅助属于
  应用工程，不要回流到 AiUI。等 AiUI 补齐 ComboBox / CheckBox /
  ProgressBar 后再逐个迁移。

## 注释与命名

- 注释、文档用中文，简洁，只说"为什么"。
- 函数动词开头，名词首字母大写 camelCase，与 Android 风格一致。
