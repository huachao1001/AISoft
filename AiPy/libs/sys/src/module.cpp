// `sys` 模块注册。
//
// 目前只有一个函数 `argv`，它读的是语言层记录的命令行（见 vm.h 的
// setCommandLine）——库本身不碰平台 API，所以将来换平台不用改这个库。
#include "aipy/pylib.h"

AIPY_PYLIB_MODULE(sys);
