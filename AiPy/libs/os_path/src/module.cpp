// `os.path` 模块注册。
//
// ── 为什么符号前缀与模块名不同 ──
//
// 对外模块名必须是 `os.path`（脚本里就写 `os.path.exists`），而 C++ 标识符
// 里不能有点。所以符号前缀用下划线版本 `os_path`，注册项里记带点的真名。
//
// 规则只有一条：**符号前缀 = 模块名把点换成下划线**。打包器（读 COFF 符号）
// 与链接器（拼 /INCLUDE）用同一条规则，不需要在 manifest 里多记字段。
//
// ── 为什么是一个独立 .pylib ──
//
// 判「文件在不在」与「列目录 / 建目录」是两件事：只用 `os.path.exists` 的
// 脚本不该把 listdir 的目录遍历代码拖进产物。分开打包，按需链接才落到
// 用户真正用得着的那几个函数上。
#include "aipy/pylib.h"

AIPY_PYLIB_MODULE_NAMED(os_path, "os.path");
