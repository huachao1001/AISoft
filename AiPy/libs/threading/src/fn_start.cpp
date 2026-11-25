// `threading.start(name, ...)` —— AiPy 扩展：把一个脚本函数丢到后台，
// 返回任务号。标准库没有对应物（标准库用 `Thread(...).start()`）。
//
// 保留它是因为 `Thread` 对象要两行（`t = Thread(...)`、`t.start()`），
// 而「丢出去跑完收结果」这种最常见的用法一行更直接。
//
// 独立编译单元：只调这一个函数的脚本，产物里不该出现 Thread/join 的代码。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

#include <string>
#include <vector>

namespace aipy {
namespace threading {
namespace {

Value startImpl(const Value* args, int count) {
  if (count < 1) return Value(0LL);
  const std::string name = args[0].toString();
  std::vector<Value> callArgs;
  for (int i = 1; i < count; ++i) callArgs.push_back(args[i]);

  // 结果进池里的队列，用 waitAll/poll/pending 取。
  const long long id = submitNamed(name, callArgs, nullptr);
  return Value(id);
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, start, startImpl);

}  // namespace threading
}  // namespace aipy
