// `json.loads(text)` —— JSON 文本 → Value。
//
// 与 Python 的 `json.loads` 同义，但失败返回 None 而不是抛异常。
// `1` 读回来是整数、`1.0` 读回来是浮点，与 CPython 一致。
#include "aipy/json/codec.h"
#include "aipy/pylib.h"

namespace {

aipy::Value loadsImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  aipy::Value value;
  std::string error;
  if (!aipy::json::parse(args[0].toString(), &value, &error)) {
    aipy::diagnose("json.loads: " + error);
    return aipy::Value();
  }
  return value;
}

}  // namespace

AIPY_PYLIB_FUNCTION(json, loads, loadsImpl);
