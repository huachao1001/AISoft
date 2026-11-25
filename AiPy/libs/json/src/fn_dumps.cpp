// `json.dumps(value)` —— Value → JSON 文本。
//
// 与 Python 的 `json.dumps` 同义，但：
//   - 字典键必须是字符串（理由见 codec.cpp 文件头）
//   - 失败返回 None 而不是抛异常
#include "aipy/json/codec.h"
#include "aipy/pylib.h"

namespace {

aipy::Value dumpsImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  std::string text;
  std::string error;
  if (!aipy::json::serialize(args[0], &text, &error)) {
    aipy::diagnose("json.dumps: " + error);
    return aipy::Value();
  }
  return aipy::Value(text);
}

}  // namespace

AIPY_PYLIB_FUNCTION(json, dumps, dumpsImpl);
