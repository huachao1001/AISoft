# AiPic

看图工具应用。界面用 `AiUI` 自绘，第一版只做「看」——预览、缩放、平移、
旋转、另存为，不做像素级编辑。

## 功能

- **打开**：双击 exe 进首页；命令行传图片直接显示 `AiPic.exe <图片>`
  （也支持把图片拖到 exe 上）
- **缩放**：滚轮按 1.15 倍步进；工具栏有放大 / 缩小 / 适应窗口 / 100%；
  双击在看整图与原大之间切换。放大到 3 倍以上自动改用最近邻采样，
  凑近看的是像素格而不是被插值抹开的色块
- **平移**：按住左键拖动；图比窗口小的方向锁在居中，不会被拖出屏幕
- **旋转**：左转 / 右转各 90°。旋转只做绘制变换，不重采样——来回转几次
  也不会掉画质；另存时才按当前角度重排像素
- **翻页**：`←` / `→` 在同目录的图片之间切换（按文件名排序，与资源管理器一致）
- **另存为**：默认格式跟随源文件（jpg 存 jpg、png 存 png），也可在保存框里
  改成 PNG / JPEG / BMP，最终编码按目标后缀定
- 快捷键：`Ctrl+O` 打开、`Ctrl+S` 另存为、`0` 适应窗口、`1` 原大
- 单实例：重复启动会把图片路径转发给已运行窗口，不会开出第二个窗口

支持格式：PNG / JPEG / BMP / GIF / TIFF / ICO / WEBP（解码走系统图像库）。

## 目录

```
src/
  app/main.cpp        程序入口：单实例、命令行、主窗口尺寸
  app/app.cpp         当前图状态：像素、路径、旋转角、同目录翻页
  app/app.h
  ui/main_activity.cpp  主界面：XML 布局 + 控件装配 + 快捷键
  ui/ui_main.cpp      宿主接入：文件拖放、单实例转发
  common/util.cpp     路径、格式化、设置持久化
  res/                图标与 comctl32 v6 清单
assets/layouts/       XML 界面布局与配色（构建期自动转 C++）
tools/make_icon.py    生成 aipic.ico 与 logo.png
```

## 界面装配

主窗口是一个 `Activity`，布局来自 XML：

```xml
<!-- assets/layouts/main_frame.xml -->
<LinearLayout orientation="vertical">
  <ButtonBar id="toolBar" height="52"/>
  <FrameLayout id="content" weight="1">
    <PhotoView id="photo" width="match" height="match"/>
    <LinearLayout id="homePane" .../>   <!-- 没有图时盖在上面 -->
  </FrameLayout>
  <StatusBar id="status" height="36"/>
</LinearLayout>
```

`PhotoView` 是 `AiUI` 的通用控件（不是本工程写的）：缩放、平移、旋转都在
它里面，AiPic 只负责给它喂文件、把工具栏按钮接到它的方法上。

## 像素在哪

界面上的 `Image` 只是后端句柄，取不到内容。需要真正读写像素的地方
（另存为、按角度重排）走 `Platform::loadImagePixels` / `saveImagePixels`：

- `App` 打开图片时解码一份 `RgbaImage` 留着，另存与旋转都以它为基准
- `PhotoView` 自己再解码一份位图用于绘制

两处解码是刻意的：平台没有「从位图取像素」的入口，合成一个解码器只会把
两边的生命周期绑死。一个目录几千张图也不预加载——只记路径，翻到才解。

## 构建

```bat
call "D:\vs2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S AiPic -B AiPic\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiPic\build --parallel
```

产物是单个 `AiPic\build\AiPic.exe`，CRT 已静态链接，目标机无需装 VC++ 运行库。
链接后会自动同步到本工程的 `AiPic\dist\`。

`AiPic` 通过 `add_subdirectory` 引入同级 `AiUI`，`AiUI` 不反向依赖本工程。

## 后续

第一版刻意不含：裁剪、调色、批量转换、缩略图列表、幻灯片、安装包与
文件关联。看图的路径先走通，编辑能力再按实际需要加——每加一项都要在
`App` 里给出「改了什么、怎么撤销」的说法，否则又是一堆改完回不去的小工具。
