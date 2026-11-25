// `text.repeat` —— 用户自定义库的函数（样板）。
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value repeat(const aipy::Value* args, int count) {
  if (count < 2) return aipy::Value();
  const std::string text = args[0].toString();
  const long long times = args[1].toInt();
  std::string out;
  for (long long i = 0; i < times; ++i) out += text;
  return aipy::Value(out);
}

}  // namespace

AIPY_PYLIB_FUNCTION(text, repeat, repeat);
