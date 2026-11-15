# AI 软件集

一组自绘界面的桌面小工具，共用 `AiUI` 这个 UI 库。

## 工程

```
AiUI/        自绘 UI 库，独立工程（Windows 后端 + 跨平台抽象）
AiZip/       压缩 / 解压工具，通过 add_subdirectory 引入同级 AiUI
AiPic/       看图工具，同样只依赖 AiUI
```

各应用工程可以分别构建，`AiUI` 不反向依赖任何一个。

```bat
call "D:\vs2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: UI 库
cmake -S AiUI -B AiUI\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiUI\build --parallel

:: 应用（会自动带上 AiUI）
cmake -S AiZip -B AiZip\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiZip\build --parallel

cmake -S AiPic -B AiPic\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiPic\build --parallel
```

需要 VS2022（MSVC 14.4x）+ CMake 3.20+ + Ninja。

## AiZip

压缩 / 解压工具。界面全部自绘（GDI+），内核用官方 7-Zip 静态链接，
无广告、无推广、无工具箱。

**单文件分发：`AiZip.exe` 约 2.2 MB**。7-Zip 内核已编成静态库链进 exe，
不依赖外部 dll，拷走一个文件即可运行。

- 全自绘界面：工具栏、面包屑、地址栏、搜索框、文件列表、侧树、状态栏
  都由 `AiUI` 绘制，不使用 `SysListView32` / `SysHeader32` / 原生 `EDIT`
- 布局系统：`FrameLayout` / `LinearLayout`（含 `weight` 按比例分配剩余空间）
- XML 布局：`.xml` 在构建期自动转成 C++ 并加入编译，改布局不用手写装配代码
- 压缩格式：7Z / ZIP / TAR / GZ
- 解压格式：全格式（zip、7z、rar、tar、gz、bz2、xz、cab、iso、wim 等），
  含 7z 头部加密

## AiPic

看图工具。同样全自绘，第一版只做浏览：缩放（滚轮 / 双击 / 工具栏）、
拖拽平移、90° 旋转、同目录左右键翻页、另存为 PNG / JPEG / BMP。

- 放大到 3 倍以上自动改最近邻采样，凑近看到的是像素格而不是插值糊块
- 旋转只做绘制变换、不重采样；另存时按当前角度重排像素，来回转不掉画质
- 打开时保留一份内存像素用于导出，绘制用另一份位图，互不影响
- `PhotoView` 是 `AiUI` 的通用控件，AiPic 只负责接线

## 文档

- [AiUI/README.md](AiUI/README.md) —— UI 库架构、绘制分层、布局与 XML 生成
- [AiZip/README.md](AiZip/README.md) —— 应用功能、目录结构与构建细节
- [AiPic/README.md](AiPic/README.md) —— 看图功能、像素读写位置与构建细节
