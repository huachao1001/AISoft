// `json` pylib 的共用接口：序列化与解析。
//
// 为什么不直接 include 一个现成的 JSON 库：AiPy 的 Value 只有七种类型
// （None/bool/int/float/string/list/dict），JSON 的模型与它几乎一一对应。
// 一百来行就能覆盖，而引入第三方库会带来「解析失败怎么报错」这类与
// AiPy 返回值风格不符的问题（这里统一返回 None）。
#pragma once

#include "aipy/value.h"

#include <string>

namespace aipy {
namespace json {

// Value → JSON 文本。失败（如 dict 的键不是字符串）返回 false 并写 error。
bool serialize(const Value& value, std::string* text, std::string* error);

// JSON 文本 → Value。失败返回 false 并写 error（带位置）。
bool parse(const std::string& text, Value* value, std::string* error);

}  // namespace json
}  // namespace aipy
