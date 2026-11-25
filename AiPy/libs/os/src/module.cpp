// `os` 模块注册。
//
// 与 libs/threading 完全同形：本文件只声明「这个模块参与本次链接」，
// 每个函数在各自的 fn_*.cpp 里用 AIPY_PYLIB_FUNCTION 登记。
#include "aipy/pylib.h"

AIPY_PYLIB_MODULE(os);
