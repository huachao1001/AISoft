// 极简 JSON 读取。
//
// ── 为什么不用现成的 JSON 库 ──
//
// 只读一种我们自己在构建期写出的文件：`.pylib` 里的 `manifest.json`。结构
// 固定、字段少。引一个 JSON 库会把几百 KB 源码带进发布件（pcc.exe 会被
// 拷进 dist/ 直接分发）。
//
// ── 支持范围 ──
//
// 字符串（含基本转义）、数字、布尔、null、数组、对象。不支持的是：
// 注解、单引号、尾逗号——都不是 json.dumps 会产出的形态。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace pcc {

struct JsonValue {
  enum class Type { Null, String, Number, Array, Object, Bool };

  Type type = Type::Null;
  std::string text;  // String 的内容 / Number 的原文 / Bool 的 "true"|"false"
  std::vector<JsonValue> items;
  std::map<std::string, JsonValue> fields;

  bool isString() const { return type == Type::String; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }

  // 对象里取字段；没有或类型不对时返回 nullptr。
  const JsonValue* get(const std::string& key) const;
  // 取字符串字段，缺失时返回 defaultText。
  std::string getString(const std::string& key,
                        const std::string& defaultText = std::string()) const;
  // 取整数字段，缺失时返回 defaultInt。
  int getInt(const std::string& key, int defaultInt = 0) const;
};

// 解析整段文本。失败返回 false。
bool parseJson(const std::string& text, JsonValue* out);

// 从对象里取字符串数组；字段缺失或类型不对时返回空。
std::vector<std::string> jsonStringArray(const JsonValue& owner,
                                         const std::string& key);

}  // namespace pcc
