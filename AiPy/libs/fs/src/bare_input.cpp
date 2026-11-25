// `input` 这个**裸名**的注册。
//
//   AIPY_PYLIB_BUILTIN(fs, input, input);
//
// 意思是：脚本里写 `input("name? ")` 就是 `fs.input("name? ")`。与 `open`
// 完全同构——两者都让脚本以「不用 import 的内建」形态使用库能力。
//
// ── 为什么单独一个文件 ──
//
// 按需链接的粒度是 .obj。裸名项若与 `inputImpl` 同处一个 .obj，只点名裸名
// 就会把整个 .obj 拖进来。这与 AIPY_PYLIB_FUNCTION 要求「一个函数一个文件」
// 是同一条理由，见 bare_open.cpp。
#include "aipy/pylib.h"

AIPY_PYLIB_BUILTIN(fs, input, input);
