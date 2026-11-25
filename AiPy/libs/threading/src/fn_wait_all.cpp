// `threading.waitAll()` —— 阻塞等全部任务完成，返回结果列表（AiPy 扩展）。
//
// 标准库里对应的写法是逐个 `t.join()`；这里保留它是因为「丢出去、收结果」
// 在脚本里太常见，逐个 join 要自己维护一个列表。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

namespace aipy {
namespace threading {
namespace {

Value waitAllImpl(const Value*, int) { return waitAll(); }

}  // namespace

AIPY_PYLIB_FUNCTION(threading, waitAll, waitAllImpl);

}  // namespace threading
}  // namespace aipy
