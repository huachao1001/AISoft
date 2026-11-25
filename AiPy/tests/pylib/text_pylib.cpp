// 示例 pylib：给脚本提供 `text` 模块。
//
// ── 它是「用户自定义库」的样板 ──
//
// 这里只有两个宏：`AIPY_PYLIB_MODULE(text)` 声明模块、每个函数一行
// `AIPY_PYLIB_FUNCTION(text, 名字, 实现)`。系统库 `libs/threading` 用的是
// 完全一样的两条——没有各库私有的 registry.h，也没有各库私有的符号前缀。
//
// 为什么不把三个函数写在一个文件里：静态库的抽取粒度是 .obj，三个函数
// 在同一个 obj 里，`/INCLUDE` 点到 `upper` 就会把 `repeat`/`shout` 一起
// 拖进产物，函数级按需失效。所以分成三个文件——这是用这套标准时唯一要
// 多留心的一条，也是它与 pybind11 的唯一差别（见 aipy/pylib.h 文件头）。

#include "aipy/pylib.h"

AIPY_PYLIB_MODULE(text);
