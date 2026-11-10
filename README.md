# AIZip

压缩 / 解压工具的极简实现。纯 Win32 + GDI 界面，官方 7-Zip 内核，
无广告、无推广、无工具箱。

**单文件分发：`AIZip.exe` 2.0 MB**。7-Zip 内核已作为静态库
链接进 exe，不依赖任何外部 dll，拷走一个文件即可运行。

## 构建

需要 VS2022（MSVC 14.4x）+ CMake 3.20+ + Ninja。

两个工程彼此独立，可以分别打开构建：

```bat
call "D:\vs2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: UI 库（可选，AiZip 会通过 add_subdirectory 自动带上）
cmake -S AUI -B AUI\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AUI\build --parallel

:: 应用
cmake -S AiZip -B AiZip\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiZip\build --parallel
```

产物是单个 `AiZip\build\AiZip.exe`。CRT 已静态链接，目标机无需安装 VC++ 运行库。

`AiZip` 通过 `add_subdirectory` 引入同级 `AUI`，`AUI` 不反向依赖应用。
单独构建 `AUI` 可得到静态库与头文件，供其它宿主工程复用。

## 单文件是怎么做到的

`AiZip/third_party/7z/build-cmake` 把官方 7-Zip 26.05 源码编成静态库 `7zstatic`，直接链进 exe。
构建清单取自官方 `7z.dll` 的配方 `CPP/7zip/Bundles/Format7zF/Arc.mak`，
所以格式与算法支持和 7z.dll 完全一致。

与原版 dll 构建的必要差异：

- 不定义 `Z7_EXTERNAL_CODECS`：静态链接下没有外部 codec 宿主
- 入口换成 `Z7StaticExports.cpp`，去掉 `DllMain`
- 链接时加 `/WHOLEARCHIVE:7zstatic`：各格式靠 `*Register.cpp` 的全局对象在
  静态初始化阶段注册，这些对象没有外部引用，普通链接会被整体丢弃，
  结果是格式列表为空

清单由 `tools/gen_sources.ps1` 从 `Arc.mak` 解析生成（225 cpp / 46 c / 8 asm）。
汇编模块（CRC/AES/SHA/LZMA 解码加速）用 `ml64` 编译；配置期会把 `.asm` 里的
共享宏 include 改写成绝对路径，因为 MASM 按工作目录而非源文件目录解析相对路径。


## 使用

- 双击 exe 打开文件浏览；从命令行传目录或压缩包可直接定位：
  `AIZip.exe <路径>`
- **添加**：把选中项（未选中则当前目录）打包，弹出压缩对话框
- **解压到**：解压选中项或整个压缩包
- **删除**：移入回收站
- 地址栏可编辑，回车跳转；右侧搜索框按名称过滤当前目录
- 双击压缩包进入浏览模式，双击目录进入下一层，双击文件直接解压
- 支持拖放：拖入压缩包直接浏览，拖入其他文件弹出压缩对话框
- 单实例：重复启动会把参数转发给已运行窗口

## 支持范围

压缩格式：7Z / ZIP / TAR / GZ
解压格式：全格式（zip、7z、rar、tar、gz、bz2、xz、cab、iso、wim、lzh、
arj、001 分卷等），含 7z 头部加密

界面列：浏览模式 4 列；压缩包模式 9 列（含压缩后大小、安全、CRC32、
压缩算法、路径）。

## 精简范围

剔除：广告 SDK、首页推广、皮肤下载、升级检查、反馈上报、工具箱（批量改名 /
MD5 / 虚拟光驱）、图片视频 PDF 压缩与预览、云盘、分享、安全扫描、守护进程。

未实现：自解压（SFX）生成、压缩包注释写入、压缩包内直接增删成员。

技术选型与验证记录见 [AiZip/DESIGN.md](AiZip/DESIGN.md)。

## 目录

```
AUI/                        自绘 UI 库（独立工程，可单独构建）
AUI/include/AUI/            公开头文件，按 "AUI/xxx.h" 引用
AUI/src/                    控件实现（按钮、输入框、列表、树、状态栏…）
AiZip/                      应用工程
AiZip/src/                  业务源码（界面装配、对话框、7-Zip 封装）
AiZip/src/res/              图标与 comctl32 v6 清单
AiZip/third_party/7z/       7-Zip 26.05 源码（与官方源码包同构）
AiZip/third_party/7z/build-cmake/  静态库的 CMake 构建配置与对象清单
AiZip/tools/                内核验证程序（test_arc.cpp）与清单生成脚本
```

主界面全部自绘：工具栏、面包屑、搜索框、地址栏、文件列表、侧树与状态栏
都是 `AUI` 的自绘窗口，不使用 `SysListView32` / `SysHeader32` / 原生 `EDIT`
这类子控件，为跨平台留出空间。

`AiZip/third_party/7z/` 只保留编进 `7zstatic` 的部分：上游的 GUI（`CPP/7zip/UI`）、
编译器工具链（`C/Util`）、非 Format7zF 的 bundle 和 7z.dll 分发件都已移除。
需要核对上游改动时，可直接与官方 7-Zip 26.05 源码包对应目录 diff。
