// `open` 这个**裸名**的注册。
//
//   AIPY_PYLIB_BUILTIN(fs, open, open);
//
// 意思是：脚本里写 `open(p)` 就是 `fs.open(p)`。对脚本作者来说 `open` 与
// `len` / `print` 没有任何区别——都是「不用 import 就能用的函数」。
//
// ── 为什么单独一个文件 ──
//
// 按需链接的粒度是 .obj。裸名项若和 `openImpl` 在同一个 .obj 里，只点名
// 裸名就会把整个 .obj 拖进来，「只链一个函数」在粒度上就失效了。这与
// AIPY_PYLIB_FUNCTION 要求「一个函数一个文件」是同一条理由。
//
// ── 为什么还要 AIPY_PYLIB_MODULE ──
//
// 裸名项说明「可以这么叫」，模块项说明「这个库参与了本次链接」。运行时
// resolveBare 会先查模块是否注册过：只有裸名项、没有模块项的产物一律当作
// 「没链接进来」，否则脚本会调到一个本不该存在的实现。
#include "aipy/pylib.h"

AIPY_PYLIB_BUILTIN(fs, open, open);
