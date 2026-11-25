// `threading.active_count()` —— 当前活着的线程数。
//
// 与 CPython 的 threading.active_count() 同义：包括主线程。
//
// 这里数的是「正在执行任务的后台线程」而不是「池创建了几个 worker」：
// worker 是复用的，join 之后仍然活着，数它们会让 active_count() 一直返回
// 5，与 CPython「线程结束后回到 1」的语义相悖。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

namespace aipy {
namespace threading {
namespace {

Value activeCountImpl(const Value*, int) {
  return Value(static_cast<long long>(runningCount()) + 1);
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, active_count, activeCountImpl);

}  // namespace threading
}  // namespace aipy
