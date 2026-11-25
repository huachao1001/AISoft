// `threading.poll()` —— 非阻塞取一个已完成结果（AiPy 扩展）。
//
// 独立编译单元：只调 poll 的脚本不该把 start 的实现拖进来。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

namespace aipy {
namespace threading {
namespace {

Value pollImpl(const Value*, int) { return poll(); }

}  // namespace

AIPY_PYLIB_FUNCTION(threading, poll, pollImpl);

}  // namespace threading
}  // namespace aipy
