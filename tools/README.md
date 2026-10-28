# 生成 AIZip 图标

纯 PowerShell + System.Drawing，不依赖任何第三方工具。
改完配色/图形后运行本脚本，会覆盖 `src/res/aizip.ico`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1
```

脚本生成 10 个尺寸（16/20/24/32/40/48/64/96/128/256），每帧都是 32bpp PNG，
打包成单个 ICO。Windows Vista 以后都读得懂 PNG 帧，Explorer 各视图也不需要
另存 BMP 帧。

## 设计

- 底板：圆角方格，青绿 → 青 → 蓝紫 的对角三色渐变（`#00E8B0` → `#28C8F5` → `#764CFF`）
- 左上角一层柔光，避免整块平渐变显得呆板
- 主体：白色大写 **A** + 右侧一条**拉链竖条**，整体读作 **AI**
  - A 用圆头笔画，双斜边 + 横梁，内孔保持清晰的圆角三角，缩小后不糊
  - 右侧竖条即在表意 I，同时内部挖指孔与交错齿槽，表意拉链/压缩
  - 整体读作 AI（AIZip），拉链细节表意压缩/打包
  - 40px 以下省略齿槽，只留等宽竖条 + 指孔，16px 下仍可辨认

## 注意

`CMakeLists.txt` 里给 `aizip.rc` 显式加了 `OBJECT_DEPENDS` 指向 `aizip.ico`。
没有这行的话，改图标不会触发 `.res` 重编，exe 里会一直保留旧图标
（CMake 不跟踪 `.rc` 里 `#include`/资源引用的文件）。
