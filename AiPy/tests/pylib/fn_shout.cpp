// `text.shout` —— 用户自定义库的函数（样板）。
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value shout(const aipy::Value* args, int count) {
  // 没参数也要给出可预期的结果：扩展失败用返回值表示，不报错。
  if (count < 1) return aipy::Value(std::string("!"));
  std::string text = args[0].toString();
  for (char& ch : text) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  return aipy::Value(text + "!");
}

}  // namespace

AIPY_PYLIB_FUNCTION(text, shout, shout);
