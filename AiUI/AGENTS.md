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

## 分层：`src/os/` 是唯一碰操作系统的目录

```
include/AiUI/          公开接口，平台无关
src/                   通用实现（视图树、布局、控件），不得出现系统 API
src/os/os.h            系统能力的纯接口：os::Backend 抽象类
src/os/windows/        Windows 实现：WindowsBackend + 窗口、GDI+ 绘制、图标、标题栏
src/os/macos/          macOS 实现（占位，尚未实现）
src/os/ubuntu/         Linux 实现（占位，尚未实现）
```

- **系统能力先过 `src/os/os.h`**：文件、进程、对话框、剪贴板、外壳集成、
  安装、托盘、线程投递、单实例全部是 `os::Backend` 的纯虚函数，只用
  `AiUI::` 中性类型。`os.h` 位于 `src/os/` 但要求与公开头相同——出现
  `HWND` / `HKEY` / COM 就等于没抽象。
- 换平台 = 在 `src/os/<platform>/` 实现一份 `Backend`，再补
  `PaintBackend`、`Platform`、`PlatformWindow`、`titleBarDefaults()` 和窗口
  消息到中性事件的翻译。`CMakeLists.txt` 里按 `WIN32` / `APPLE` / `UNIX`
  选目录。
- **`src/` 顶层与 `src/widgets/` 不允许 `#include <windows.h>`，也不允许调用
  任何系统 API**（剪贴板、路径比较、字体族、DPI 都要走 `Platform` 接口）。
- `#ifdef _WIN32` 不应出现在通用实现里。

### 接口在外、实现在内

分两层，缺一不可：

1. **`src/os/os.h`（内部契约）**：`os::Backend` 声明系统能力，各平台在
   `src/os/<platform>/<platform>_backend.h` 继承实现。`backend()` 返回进程内
   唯一的实例。
2. **`include/AiUI/platform.h`（公开门面）**：`Platform` 是应用与通用代码
   看到的中性接口，其实现（如 `WindowsPlatform`）只做转发——
   `os::backend().xxx(...)`。应用永远不需要知道 `os::Backend` 存在。

已有对应关系：

| 公开接口 | Windows 实现 |
|---|---|
| `Platform`（`platform.h`） | `window_windows.cpp`（`WindowsPlatform`）转发给 `os::backend()` |
| `os::Backend`（`src/os/os.h`） | `windows_backend.h` 声明，实现在 `platform_windows.cpp` / `install_windows.cpp` / `shell_menu_register.cpp` |
| `ShellVerb` / `registerShellVerbs` | `platform_windows.cpp` |
| `ShellMenuHandler` / `aiuiBuildShellMenu` | `shell_menu_windows.cpp`、`shell_menu_register.cpp` |
| `InstallScope` / `installFiles` / `writeInstallMarker` | `install_windows.cpp` |
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
函数生成，应用只声明源文件与标识：

| 函数 | 产物 |
|---|---|
| `aiui_add_shell_menu_dll` | 外壳：COM 入口 + `registerShellMenuHandler`，可 `MERGE_INTO` 一个已有 dll |
| `aiui_add_shell_menu_core` | 逻辑：`aiuiBuildShellMenu` 包成纯 C 导出，可随时替换 |

分两层是因为「谁被长期持有」：外壳会被资源管理器一直加载着，改不动；逻辑
dll 每次弹菜单才加载、算完立刻卸载，文件随时能换，改文案下次右键即生效。
两者的边界见 `include/AiUI/shell_menu_core.h`——**只用定长缓冲**，两个 dll
各有一份静态 CRT，跨边界传 `std::wstring` 会崩资源管理器。


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

## 动画：连续输入用跟随模型，不要重启固定时长动画

这条是滚轮缩放踩过坑之后定下来的，写死在这里，不要再用「每来一格就起一段
180ms 缓动」的做法。

**为什么**：固定时长动画每来一格都要丢弃旧线段、从当前值重新起一段。速度于是
每格都归零再拉起，连续输入时就变成一串「加速—刹车」的锯齿。换曲线治不好——
问题在模型，不在曲线。用户的原话是「动画不够丝滑」。

**怎么做**：连续输入（滚轮、拖动、连续 resize）走逐帧跟随，每帧朝目标收敛固定
比例：

```
alpha   = 1 - exp(-dt / tau)          // tau 是时间常数，photo_view 取 90ms
nextLog = fromLog + (toLog - fromLog) * alpha
```

三个性质正好对应要的观感：

| 现象 | 来自哪 |
|---|---|
| 一输入就动 | 首帧按 dt/tau 走掉约 16% |
| 连续输入跟手 | 目标不断前移，剩余量变大，速度自动跟着变大，永不归零 |
| 停下有收尾 | 目标停住，剩余量按同一条指数曲线衰减 |

四个必须配套的细节，缺一个都会退回「不顺」：

1. **在对数倍数上收敛**，不要在线性倍数上。滚轮一格是乘法，线性收敛时缩小那
   一侧会明显拖沓，放大缩小观感不一致。
2. **用真实帧间隔 dt**，不要按固定 16ms 推进。定时器精度不足时帧间隔会抖，固定
   步长会把抖动放大成忽快忽慢。dt 要夹住（1..64ms）：卡顿时别一帧走完。
3. **新输入不重开动画**，只推进目标值。重开就是把速度清零，正是锯齿的来源。
4. **收尾时用锚定公式重算**，不要写回开格时算的旧目标。连续输入后那个旧值早已
   过时，落位那一帧会跳一下。

**平台层的配套**：Windows 的 `SetTimer` 默认时钟粒度约 15.6ms，请求 16ms 会被
拉成 31ms（32fps 且不均）。`window_windows.cpp` 因此在有排帧任务时
`timeBeginPeriod(1)`，排空立刻 `timeEndPeriod(1)`——不要常开，那样整机更费电。

**一次性操作不要套这套**：按钮、双击、适应窗口、旋转是一次性动作，仍用固定时长
+ `easeInOut`。它们要的正是「两端收住、不生硬」，跟连续输入的需求相反。

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

## 文件系统图标按尺寸取，不要固定取小图

`fileIcon(path, isDirectory, pixelSize)` 的第三个参数是要画出来的**设备像素
边长**，不是设计值——调用方自己乘 DPI。把它当成必填来用：布局里写 `48` 的
位置在 150% 屏上要 72，只取 16×16 再放大就会糊成一团（维护页的应用图标
就是这么糊的，实测肉眼立辨）。

取图分两步，先后顺序不能反：

1. **文件自带图标**（exe / dll / ico）走 `PrivateExtractIconsW`，按需要的
   边长精确取。ico 里的原生档位很多（`aizip.ico` 有 16~256 十档），要 48
   就给 48。`PrivateExtractIconsW` 对没有内嵌图标的文件返回 NULL，所以必须
   留第二条路。
2. **没有内嵌图标的**（目录、文档）走 `SHGetFileInfoW` 要关联图标。shell
   只有 `SHGFI_SMALLICON`(16) 与 `SHGFI_LARGEICON`(32) 两档，不能追问更大
   的自定义尺寸；按需求选一档即可。

缓存键必须带上档位：同一文件不同尺寸是不同位图，否则 16 的那张会顶替 48 的。

## 注册表：HKCR 是合并视图，HKCU 优先

写 `Software\Classes\` 下的东西时，HKCU 与 HKLM 会合并成 `HKEY_CLASSES_ROOT`
给系统看，**同名项以 HKCU 为准**。这带来一个很隐蔽的故障：程序换过安装位置、
或者开发期直接跑过免安装的构建产物之后，另一边根键里会留下一份同名的旧登记，
而实际生效的正是它——逐项查注册表全都指向正确路径，双击却打开了另一份程序，
甚至是一份已经不存在的 exe。

所以：

- **写自己那一边之后，要检查另一边有没有指向不同路径的同名登记，有就删掉。**
  只写自己那一边修不掉这种状态。
- **判断「已就绪」时两个根键都要看**，不能只查安装范围对应的那个。
- 前提是这些同名项属于程序独占的命名空间（例如以 ProgID 命名）。若名字是与
  别的软件共用的（如 `Applications\foo.exe`），删之前必须确认它确实指向自己。

## 注释与命名

- 注释、文档用中文，简洁，只说"为什么"。
- 函数动词开头，名词首字母大写 camelCase，与 Android 风格一致。
