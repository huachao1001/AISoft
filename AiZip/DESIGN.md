# 爱压 设计说明

## 目标

实现一个轻量的压缩/解压工具与文件浏览界面，剔除广告、推广、
升级弹窗、工具箱、图片/视频/PDF 压缩等无关模块。

## 技术选型

| 项 | 选择 | 理由 |
|---|---|---|
| 语言 | C++17 | 无 CRT 以外依赖，编译产物小 |
| 界面 | 自绘 UI 库 `AiUI`：GDI+ 离屏双缓冲 + 抽象平台层 | 不引入 Duilib/MFC/Qt；启动快、体积小，控件可换后端复用 |
| 布局 | `FrameLayout` / `LinearLayout` + XML 构建期生成 C++ | 手写装配代码改成声明式，缩放交给布局系统 |
| 压缩内核 | 官方 7-Zip 26.05（静态库） | 格式/算法/参数直接复用上游 |
| 运行时 | 静态链接 CRT (`/MT`) | 免安装 VC++ 运行库 |
| 构建 | CMake + MSVC | 单命令出包 |

体积预算：exe ≈ 2.2 MB（内核已链入，无外部 dll）。

## 功能范围

爱压 有两种用法：双击 exe 打开文件浏览窗口，或从 Windows 资源管理器右键菜单
直接发起压缩 / 解压，只弹出对应的小窗口、不建主窗口。

保留：
- 主窗口与文件浏览：自绘工具栏、地址栏、文件列表、状态栏；
  图标取自系统图标字体 Segoe Fluent Icons（缺失回退 MDL2）；
  双击压缩包进入包内浏览，地址栏可编辑，搜索框按名称过滤
- 压缩对话框：格式 ZIP/7Z/TAR/GZ 等 10 种、压缩级别（存储~极限）、分卷、密码
- 解压对话框：目标路径/浏览；标题带压缩包文件名；
  存在同名文件时逐个询问是否覆盖；需要密码时另弹输入框
- 资源管理器右键菜单：压缩、一键压缩到 ZIP、打开压缩文件、解压、解压到当前目录
  （按标识顺序相邻显示）。普通文件与目录显示「压缩到 "xxx.zip"」「添加到压缩文件...」；
  单个压缩包显示「打开压缩文件 / 解压到... / 解压到 "xxx" / 解压到当前目录」，
  其中「解压到 "xxx"」自动创建同名目录并解入，名字加引号以看清边界
- 文件关联：注册为受支持压缩包后缀（zip/7z/rar/tar/gz/tgz/bz2/tbz2/xz/txz/cab/
  iso/wim/img/jar/apk/aar/epub/lzh/arj/z/001）的候选打开方式。Win10+ 不允许
  程序静默抢占默认应用，只登记 ProgID 与 OpenWithProgids；后缀尚无默认程序时
  才直接设为默认，用户已选过的一律尊重

剔除（不实现）：
- 广告 SDK、首页推广、皮肤下载、升级检查、反馈上报
- 工具箱（批量改名、MD5、文本替换、虚拟光驱等）
- 图片/视频/PDF 压缩与预览、文件编辑器
- 云盘、分享、安全扫描、守护进程

## 已验证行为

以下均以官方 7-Zip 26.05 交叉校验：

| 项目 | 结果 |
|---|---|
| ZIP 压缩 | 4 文件 / 3 目录完整保留，`7z t` 通过 |
| 7Z 加密压缩 | 极限级 → `LZMA2:12`，`7zAES` 加密，`Solid=+` |
| 密码校验 | 正确密码通过，错误密码报错 |
| 需要密码判定 | 只有内核索取过口令才弹密码框；损坏或格式不支持直接报错 |
| 7z 头部加密 | `-mhe=on` 的包可正常枚举与解压（需 `ICryptoGetTextPassword` 5,0x10） |
| 解压 | 4 个文件 SHA-256 与原文件逐字节一致 |
| 跨格式 | TAR / TAR.GZ / RAR / CAB 解压通过 |
| 中文文件名 | ZIP 往返保持 UTF-8 名称 |
| 句柄释放 | 解压结束后文件立即可读（无占用） |
| 一键压缩 | 右键「压缩到 ZIP」→ 直接产出 `源名.zip`，同名自动加 `(2)` |
| 右键菜单 | 普通文件/目录显示「压缩到 "xxx.zip"」「添加到压缩文件...」；单个压缩包显示「打开压缩文件 / 解压到... / 解压到 "xxx" / 解压到当前目录」；多选（含混选压缩包）一律显示压缩入口；多选多个普通文件合并成 1 个包；解压类动词仅在单选压缩包时出现 |
| 解压到 "xxx" | 自动创建与包主名同名的目录并解入（`abc.zip` → `abc\`）；包内唯一顶层目录与目标同名时剥离该层，避免 `abc\abc\` |
| 文件关联 | 22 个支持后缀全部登记为候选打开方式（ProgID `AIZip.Archive` + `OpenWithProgids` + `Applications`）；`.jar`、`.aar` 等原本无主的后缀直接设为默认，已被 2345 好压等占用的后缀保持用户原选择 |
| 动态菜单 | 菜单由进程内组件实时计算，文案带实际包名（`压缩到 "rnd.zip"`）；多选压缩包时按选择内容判定，不再受 `AppliesTo` 只认首项的退化影响 |
| 菜单图标 | 每项带宿主程序图标（按 DPI 缩放，150% 下为 24px，位图 32bpp 带 alpha） |
| 多选退化 | 已消除：判定逻辑在组件里逐项检查，不依赖注册表条件表达式 |
| 菜单可见性 | Win10 直接出现在右键第一层。Win11 的紧凑菜单只接受带包标识（MSIX/稀疏包）的 `IExplorerCommand`，未打包的 Win32 程序一律归入「显示更多选项」，7-Zip、WinRAR 同样如此；本项目不引入稀疏包，故 Win11 需点「显示更多选项」 |
| 安装包 | `AiZipSetup.exe` 由**通用安装器 + 追加数据**合成：`AiSetup.exe` 本体一字不改，构建期把 `setup.ini` 清单与 `AiZip.exe`、`aizip_config.dll`、`logo.png` 追加到它末尾，并改写图标与版本资源。安装包本身**只负责解压与删除**。默认装到非系统盘（策略内嵌在 `AiInstaller` 库：按盘符字母序找第一个可写的固定盘并跳过系统盘，非系统盘全部不可写时才用系统盘；盘根不可写会换下一个盘），安装子目录 `AISoft\AIZip` 不含盘符。两个安装范围共用同一套选盘与子目录 |
| 安装范围 | 当前用户写 HKCU；所有用户写 HKLM（需提权）。点一次「安装」后自动请求提权，新实例按 `--elevated-retry --dir=` 直接完成安装，无需再点第二次。安装时把范围记进安装目录的 `installer.scope`，卸载按它读回——**不靠「当前是否提权」推断**，因为卸载器带 `requireAdministrator`，那样永远会猜成「所有用户」而清错根键 |
| 配置模块 | 右键菜单与文件关联封装在 `aizip_config.dll`，按标准 ABI 导出 `AiSetupConfigApply / Remove / IsReady / Version`（见 `AiInstaller/include/AiInstaller/config_abi.h`）。安装包只「按 ABI 调用」它，自己不写注册表；爱压主程序也调同一份实现，因此能自愈菜单 |
| 安装器库 | 通用能力（清单解析、包读取、路径策略、落盘、快捷方式、卸载入口、提权、卸载收尾、通用界面）在 `AiInstaller`，与 `AiUI` 同样定位，可被其它应用用 CMake 引入 |
| 界面 | 只有一个通用模板（图标 + 文案 + 安装范围 + 目录），品牌化全靠 `setup.ini` 的 `[ui]` 与 `logo.png`。不引入运行时 XML 解析：布局构建期编译，清单是 INI |
| 安装后行为 | 装完不弹提示框，按清单 `[run] afterInstall` 启动目标目录里的 `AiZip.exe` |
| 组件位置 | 组件与宿主 exe 同目录（安装版在程序目录，开发期在构建目录），文件名带内容指纹，如 `aizip_5705E357.dll`；指纹与清理规则都在 AiUI 内部，调用方不必知道 |
| 卸载 | 从「应用与功能」拉起安装目录里的 `uninstall.exe --uninstall`（安装器本体换名后的裁剪副本，`UninstallString` 直接指向它）：先调配置模块撤销菜单与关联，再删清单文件、快捷方式、卸载项，最后删空目录。`uninstall.exe` 自己与安装时新建的父目录（`installer.dirs` 记录）由「延迟脚本重试删除」收尾，被资源管理器占用时也能清干净；目录里用户自己放的文件一律保留。卸载读安装目录里的 `installer.manifest`——那份副本已被裁掉载荷，打不开自己末尾的包 |
| 启动自愈 | 每次启动检查配置模块是否就绪，不就绪则调 `Apply` 修一次，失败静默忽略。范围仍从注册表探测（要求注册项指向本程序），避免与另一份安装互相误判 |
| 打开压缩文件 | 无动作开关启动 → 主窗口浏览包内内容；按压缩包路径区分窗口：同一个包再次打开复用已有窗口并前置，不同的包各开独立窗口与进程；新窗口显式置顶，焦点不会被资源管理器留住 |
| 解压到当前目录 | 包内有公共顶层目录时复用它，否则新建同名目录并剥离该层，不出现 `a\a\`；冲突逐个询问 |
| 进度窗口 | 主行固定为「正在压缩/解压：包名」（压缩取输出包、解压取输入包），当前成员跟在副行尾部；短任务也走完 900ms 进度动画 |
| 高 DPI | 150% 下布局与图标正确缩放 |

## 体积

| 产物 | 大小 |
|---|---|
| AIZip.exe（7-Zip 静态链接） | 2.2 MB |

单文件分发，无需附带任何 dll，只依赖系统 DLL。

## 单文件实现

官方 7-Zip 源码编成静态库 `7zstatic` 后直接链进 exe，格式与算法支持和官方
`7z.dll` 完全一致。两个必须注意的点：

- **构建清单**取自官方 dll 配方 `CPP/7zip/Bundles/Format7zF/Arc.mak`，
  由 `tools/gen_sources.ps1` 解析生成 `sources.cmake`（225 cpp / 46 c / 8 asm）。
  各 `*Opt.c` 与同名 `.asm` 互斥，x64 下只编汇编版，否则同名符号重复定义。
- **`/WHOLEARCHIVE:7zstatic`**：各格式靠 `*Register.cpp` 里的全局对象在静态
  初始化阶段注册，这些对象没有任何外部引用，普通静态库链接会被整体丢弃，
  结果是格式列表为空。整库拉入后由 `/OPT:REF` 清理真正死掉的函数。

汇编模块用 `ml64` 编译。上游 `.asm` 写的是 `include ../../../../Asm/x86/7zAsm.asm`，
而 MASM 按**当前工作目录**解析相对 include（不是源文件目录），CMake 的 ASM_MASM
规则固定以构建目录为 CWD，无法复刻官方的 `Format7zF` 工作目录。因此配置期会生成
一份把该 include 改写成绝对路径的副本再交给 CMake 编译。

7z 头部加密（`-mhe=on`）的包在**打开阶段**只查询老接口
`ICryptoGetTextPassword`（5,0x10），而不是解压阶段用的 2 版（5,0x11）。
两者签名与 vtable 槽位都不同，必须各用一个独立对象实现，否则参数错位。

## 目录结构

根目录只放 `README.md` 与 git 元数据，两个工程并列且各自独立构建：

```
AiUI/
├─ CMakeLists.txt
├─ include/AiUI/          公开头文件（按 "AiUI/xxx.h" 引用）
│  ├─ widget.h            控件基类：树结构、测量、布局、事件分发、DPI 换算
│  ├─ painter.h           PaintBackend（最小图元）+ Painter（组合绘制）+ Canvas（离屏双缓冲）
│  ├─ theme.h             配色与尺寸常量
│  ├─ font.h              FontSet：按 DPI 创建的一组界面字体
│  ├─ icon.h              矢量图标 ID
│  ├─ platform.h          平台后端接口：窗口、字体、图片、绘制子系统生命周期
│  ├─ layouts/            FrameLayout / LinearLayout
│  └─ widgets/            Button / ButtonBar / BreadcrumbBar / InputBox /
│                         ListView / TreeView / StatusBar / TextView / ImageView
├─ src/
│  ├─ layouts/            布局实现
│  ├─ widgets/            控件实现
│  └─ os/                 唯一碰系统 API 的目录
│     ├─ os.h             系统能力纯接口：os::Backend 抽象类
│     ├─ windows/         Windows 实现（窗口、GDI+、外壳集成、安装）
│     ├─ macos/           macOS 实现（占位）
│     └─ ubuntu/          Linux 实现（占位）
├─ tools/layoutgen/       XML → C++ 布局代码生成器
└─ cmake/                 aiui_add_layouts() 构建规则

AiInstaller/
├─ CMakeLists.txt         通用安装器 AiSetup.exe + 打包器 aiinstaller_pack
├─ include/AiInstaller/   公开接口：清单 / 包布局 / 安装卸载 / 路径策略 / 配置 ABI
├─ src/
│  ├─ manifest.cpp        INI 清单解析与序列化（自带 UTF-8 转换，不依赖 AiUI）
│  ├─ package.cpp         从自身末尾定位 trailer、解析清单、按偏移解压
│  ├─ path_policy.cpp     默认装到非系统盘、子目录不含盘符
│  ├─ install.cpp         安装/卸载编排、提权、配置调用、自我删除收尾
│  ├─ config_module.cpp   按标准 ABI 加载并调用应用的配置 dll
│  ├─ ui/                 通用安装界面（布局在 assets/layouts/dlg_setup.xml）
│  └─ os/windows/         平台差异（卷与系统盘判定由 AiUI 回答）
├─ tools/pack/            打包器：改图标与版本资源，再追加清单与载荷
├─ cmake/                 aiinstaller_add_package() 打包规则
└─ README.md / AGENTS.md

AiZip/
├─ CMakeLists.txt         add_subdirectory(../AiUI ../AiInstaller)
├─ setup.ini              安装清单：文案、logo、配置模块、afterInstall、文件表
├─ assets/logo.png        安装界面里的品牌图片（只随包携带，不落盘）
├─ assets/layouts/        XML 界面布局（构建期自动转 C++）
├─ src/
│  ├─ app/main.cpp        程序入口、单实例、命令行分发、系统集成自愈
│  ├─ app/config_client   加载 aizip_config.dll（主程序不碰注册表）
│  ├─ config/aizip_config 右键菜单与文件关联的标准 ABI 实现（独立 dll）
│  ├─ app.h/.cpp          应用状态、视图/导航编排、启动请求分发
│  ├─ archive.h/.cpp      7-Zip 封装：打开/枚举/解压/压缩
│  ├─ ui_main.h/.cpp      主窗口：装配生成的 MainFrame、拖放、窗口消息
│  ├─ ui_list.h/.cpp      列表的数据与交互编排（控件用 AiUI::ListView）
│  ├─ ui_toolbar.h/.cpp   工具按钮条的装配（控件用 AiUI::ButtonBar）
│  ├─ ui_crumb.h/.cpp     面包屑适配：视图状态 → BreadcrumbBar 的路径段与回调
│  ├─ ui_tree.h/.cpp      左侧文件夹树的数据编排（控件用 AiUI::TreeView）
│  ├─ ui_status.h/.cpp    状态栏文案与字段来源（控件用 AiUI::StatusBar）
│  ├─ ui_theme.h          对话框底色分量（GDI 与 GDI+ 共用）
│  ├─ ui.h                界面公共入口（转发对话框辅助头文件）
│  ├─ dlg_util.h/.cpp     对话框宿主辅助：窗口类、居中、模态循环、背景绘制
│  ├─ dlg_compress.h/.cpp 压缩对话框
│  ├─ dlg_extract.h/.cpp  解压对话框
│  ├─ dlg_input.h/.cpp    密码/文本输入框
│  ├─ dlg_progress.h/.cpp 进度对话框
│  ├─ shell_menu.h/.cpp   右键菜单描述与注册入口（不含平台代码）
│  ├─ shell_menu_model.cpp 菜单内容：选中项 → 显示哪些项
│  ├─ single_instance.h/.cpp  右键多选时的进程聚合
│  ├─ res/                图标与资源
│  └─ util.h/.cpp         路径、格式化、DPI
└─ third_party/7z/        7-Zip 26.05 源码（与官方源码包同构）
   └─ build-cmake/        静态库构建配置与对象清单
```

### 分层约定

- `AiUI` 不引用任何应用代码，只对外暴露抽象层；平台细节（`HWND` / `HDC` /
  GDI+）全部收在 `src/os/<platform>/`，系统能力先经 `src/os/os.h` 的
  `os::Backend` 抽象类声明，再由各平台继承实现，换平台只需补一份实现。
  `include/` 下的头文件一律不出现平台类型，原生对话框的 Win32 辅助走应用侧
  `ui::dlg_util`。
- 主界面全部自绘：`ListView` 取代 `SysListView32` + `SysHeader32`，
  `InputBox` 取代内嵌 `EDIT`（自己维护文本、光标、选区与 IME 组合串），
  `TreeView` / `ButtonBar` / `StatusBar` 同样没有原生控件。
- 主界面结构由 `assets/layouts/main_frame.xml` 描述，构建期生成
  `AiUI::generated::MainFrame`，`WM_SIZE` 直接交给布局系统。
- 应用侧的 `ui_*.cpp` 只做数据与交互编排，控件能力全部来自 `AiUI`，
  不再为单个控件写适配层。
