// `threading.pending()` —— 还有几个已完成但没取走的结果（AiPy 扩展）。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

namespace aipy {
namespace threading {
namespace {

Value pendingImpl(const Value*, int) { return Value(static_cast<long long>(pending())); }

}  // namespace

AIPY_PYLIB_FUNCTION(threading, pending, pendingImpl);

}  // namespace threading
}  // namespace aipy
