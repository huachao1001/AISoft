# AIZip

压缩 / 解压工具。界面全部自绘（GDI+），内核用官方 7-Zip 静态链接，
无广告、无推广、无工具箱。

**单文件分发：`AiZip.exe` 约 2.2 MB**。7-Zip 内核已编成静态库链进 exe，
不依赖外部 dll，拷走一个文件即可运行。

## 工程

```
AiUI/     自绘 UI 库，独立工程（Windows 后端 + 跨平台抽象）
AiZip/    应用工程，通过 add_subdirectory 引入同级 AiUI
```

两个工程可以分别构建，`AiUI` 不反向依赖 `AiZip`。

```bat
call "D:\vs2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: UI 库
cmake -S AiUI -B AiUI\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiUI\build --parallel

:: 应用（会自动带上 AiUI）
cmake -S AiZip -B AiZip\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiZip\build --parallel
```

需要 VS2022（MSVC 14.4x）+ CMake 3.20+ + Ninja。

## 特性

- 全自绘界面：工具栏、面包屑、地址栏、搜索框、文件列表、侧树、状态栏
  都由 `AiUI` 绘制，不使用 `SysListView32` / `SysHeader32` / 原生 `EDIT`
- 布局系统：`FrameLayout` / `LinearLayout`（含 `weight` 按比例分配剩余空间）
- XML 布局：`.xml` 在构建期自动转成 C++ 并加入编译，改布局不用手写装配代码
- 压缩格式：7Z / ZIP / TAR / GZ
- 解压格式：全格式（zip、7z、rar、tar、gz、bz2、xz、cab、iso、wim 等），
  含 7z 头部加密

## 文档

- [AiUI/README.md](AiUI/README.md) —— UI 库架构、绘制分层、布局与 XML 生成
- [AiZip/README.md](AiZip/README.md) —— 应用功能、目录结构与构建细节
