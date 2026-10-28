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
- 主体：白色大写 Z，右侧一条竖拉链（拉头 + 3 段齿）
  - Z 表意 AIZip，拉链表意压缩/打包
  - 小尺寸只保留粗齿和拉头，细线全部去掉，16px 下仍可辨认

## 注意

`CMakeLists.txt` 里给 `aizip.rc` 显式加了 `OBJECT_DEPENDS` 指向 `aizip.ico`。
没有这行的话，改图标不会触发 `.res` 重编，exe 里会一直保留旧图标
（CMake 不跟踪 `.rc` 里 `#include`/资源引用的文件）。
