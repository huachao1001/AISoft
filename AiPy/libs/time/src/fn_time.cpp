// `time.time()` —— 自 Unix 纪元（1970-01-01 UTC）起的秒数，浮点。
//
// 与 Python 的 `time.time()` 同义。用 C++ 的 steady/wall 时钟而不是
// `::time()`：后者只有整秒精度，而脚本里量耗时（「这一步花了多久」）需要
// 毫秒级。
#include "aipy/pylib.h"

#include <chrono>

namespace {

aipy::Value timeImpl(const aipy::Value*, int) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const double seconds =
      std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1e6;
  return aipy::Value(seconds);
}

}  // namespace

AIPY_PYLIB_FUNCTION(time, time, timeImpl);
