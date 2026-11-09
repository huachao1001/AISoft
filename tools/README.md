# 生成 AIZip 图标

从仓库根目录的 `logo.svg` 生成 `src/res/aizip.ico`。
改完 `logo.svg` 后运行本脚本即可，它会覆盖 `src/res/aizip.ico`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1
```

脚本用 Chrome 或 Edge 的无头模式把 SVG 栅格化，再用 System.Drawing 降采样。
生成 10 个尺寸（16/20/24/32/40/48/64/96/128/256），每帧都是 32bpp PNG，
打包成单个 ICO。Windows Vista 以后都读得懂 PNG 帧，Explorer 各视图也不需要
另存 BMP 帧。

## 设计

图标直接来自 `logo.svg`：三层彩色包体（青、红、绿）+ 黄色 Y 型拉链。

`logo.svg` 的 `viewBox` 是 40×40，但图形只占中间一条，
直接渲染会在四周留下大片空白。脚本在页面里用 `getBBox()` 取图形实际边界，
按最长边加 10% 余量重设 `viewBox`，让图形撑满画布。

## 注意

`CMakeLists.txt` 里给 `aizip.rc` 显式加了 `OBJECT_DEPENDS` 指向 `aizip.ico`。
没有这行的话，改图标不会触发 `.res` 重编，exe 里会一直保留旧图标
（CMake 不跟踪 `.rc` 里 `#include`/资源引用的文件）。
