// `threading` 模块注册：只声明「这个模块参与本次链接」。
//
// 函数表不再由本库自己维护：每个导出函数在各自的 .cpp 里用
// `AIPY_PYLIB_FUNCTION(threading, ...)` 声明，VM 按「模块名.函数名」
// 在 aipy/pylib.h 的统一函数表里查。这就是「系统库与用户自定义库同一套
// 标准」的落点——本文件对一个新库来说是可以照抄的全部内容。
#include "aipy/pylib.h"

AIPY_PYLIB_MODULE(threading);
