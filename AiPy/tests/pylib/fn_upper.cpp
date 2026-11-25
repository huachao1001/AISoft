// `text.upper` —— 用户自定义库的第一个函数（样板）。
//
// 一个函数一个编译单元：函数级按需链接的前提。见 text_pylib.cpp 头部。
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value upper(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  std::string text = args[0].toString();
  for (char& ch : text) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  return aipy::Value(text);
}

}  // namespace

AIPY_PYLIB_FUNCTION(text, upper, upper);
