// JSON 编解码：`json.dumps` / `json.loads` 的共用实现。
//
// ── 与 Python 的差别（刻意，且有理由）──
//
// 1. **字典键必须是字符串**。Python 允许 `{1: 2}` 并序列化成 `{"1": 2}`，
//    但那样「读回来变成字符串键」会让脚本静默拿到不同类型的键。这里直接
//    报错，逼脚本自己决定要不要 str()。
//
// 2. **解析失败返回 None，不抛异常**。AiPy 没有异常通道（见 vm.h 文件头），
//    与 `parse_number` 失败返回 None 是同一条约定。失败原因走 log。
//
// 3. **数字只有 double 与 int64 两种**。JSON 的 `1.0` 读回来是浮点，
//    `1` 读回来是整数——与 CPython 的 json 一致。超出 i64 的整数解析失败
//    而不是静默截断：`Value` 只有 64 位（见 LANGUAGE.md 的溢出说明）。
#include "aipy/json/codec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aipy {
namespace json {
namespace {

// ── 序列化 ──

// 转义一个字符串，写成带引号的形式。
//
// 控制字符一律 \u00XX：JSON 不允许裸控制字符，而脚本里真的会出现它们
// （从文件读到的内容可能有 \r、\t）。
void writeString(const std::string& text, std::string* out) {
  out->push_back('"');
  for (unsigned char ch : text) {
    switch (ch) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          out->append(buffer);
        } else {
          // 其余字节原样输出：源串已经是 UTF-8，JSON 也按 UTF-8 处理，
          // 不需要逐字符转 \uXXXX（转了对，但会让中文变成不可读的一串）。
          out->push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out->push_back('"');
}

// 浮点：用 NSLog 的 %.17g 保住往返精度，再把 ".0" 补回来。
//
// 为什么不用默认的 %g：它只给 6 位有效数字，`0.1 + 0.2` 会输出成 `0.3`，
// 再读回来就不等于原值了。17 位是 double 的往返安全位数。
void writeNumber(double value, std::string* out) {
  if (!std::isfinite(value)) {
    // NaN / Infinity 不是合法 JSON。Python 默认输出 NaN，但那是扩展语法，
    // 别的解析器读不了；这里写成 null，明确表示「这里本来是个数，但不是
    // 可表示的数」。
    out->append("null");
    return;
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  out->append(buffer);
  // 整数值的浮点要带 .0：否则 2.0 写成 2，读回来变成整数，类型就丢了。
  // Python 的 json 也做这件事（`json.dumps(2.0)` 是 `2.0`）。
  if (std::strpbrk(buffer, ".eE") == nullptr) out->append(".0");
}

bool serializeImpl(const Value& value, std::string* out, std::string* error,
                   int depth) {
  // 深度上限：容器可以自我引用吗？AiPy 的 list 不能直接装自己（要装得先
  // 有名字，而赋值是共享句柄——所以是可能的）。限制深度防止无限递归。
  if (depth > 200) {
    *error = "嵌套层数过深（可能自引用）";
    return false;
  }

  if (value.isNone()) {
    out->append("null");
    return true;
  }
  if (value.type() == Value::Type::kBool || value.type() == Value::Type::kInt) {
    if (value.type() == Value::Type::kBool) {
      out->append(value.toBool() ? "true" : "false");
    } else {
      out->append(std::to_string(value.toInt()));
    }
    return true;
  }
  if (value.type() == Value::Type::kFloat) {
    writeNumber(value.toFloat(), out);
    return true;
  }
  if (value.type() == Value::Type::kString) {
    writeString(value.asString(), out);
    return true;
  }
  if (value.isList()) {
    out->push_back('[');
    const List& items = value.asList();
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i > 0) out->push_back(',');
      if (!serializeImpl(items[i], out, error, depth + 1)) return false;
    }
    out->push_back(']');
    return true;
  }
  if (value.isDict()) {
    out->push_back('{');
    const List& keys = value.dictKeys();
    const List& values = value.dictValues();
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i > 0) out->push_back(',');
      // 键必须是字符串，理由见文件头第 1 条。
      if (keys[i].type() != Value::Type::kString) {
        *error = "字典的键必须是字符串，第 " + std::to_string(i + 1) +
                 " 个键不是（用 str() 转一下）";
        return false;
      }
      writeString(keys[i].asString(), out);
      out->push_back(':');
      if (!serializeImpl(values[i], out, error, depth + 1)) return false;
    }
    out->push_back('}');
    return true;
  }

  // kHost / kObject：无法序列化。明确报错而不是写成 null——静默丢掉一个
  // 字段比拿到错误难查得多。
  *error = "这个类型不能转成 JSON（只有 None/bool/数字/字符串/列表/字典）";
  return false;
}

// ── 解析 ──

class Reader {
 public:
  explicit Reader(const std::string& text) : text_(text) {}

  bool parse(Value* out, std::string* error) {
    skipSpace();
    if (!parseValue(out, 0)) {
      *error = error_ + "（位置 " + std::to_string(index_ + 1) + "）";
      return false;
    }
    skipSpace();
    if (index_ != text_.size()) {
      *error = "JSON 结束后还有多余内容（位置 " +
               std::to_string(index_ + 1) + "）";
      return false;
    }
    return true;
  }

 private:
  void skipSpace() {
    while (index_ < text_.size()) {
      const char ch = text_[index_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++index_;
      } else {
        break;
      }
    }
  }

  bool fail(const std::string& message) {
    error_ = message;
    return false;
  }

  bool literal(const char* word) {
    const std::size_t length = std::strlen(word);
    if (text_.compare(index_, length, word) != 0) return false;
    index_ += length;
    return true;
  }

  bool parseValue(Value* out, int depth) {
    if (depth > 200) return fail("嵌套层数过深");
    if (index_ >= text_.size()) return fail("JSON 不完整，缺少值");

    const char ch = text_[index_];
    if (ch == 'n') {
      if (!literal("null")) return fail("无法识别 true/false/null");
      *out = Value();
      return true;
    }
    if (ch == 't') {
      if (!literal("true")) return fail("无法识别 true/false/null");
      *out = Value(true);
      return true;
    }
    if (ch == 'f') {
      if (!literal("false")) return fail("无法识别 true/false/null");
      *out = Value(false);
      return true;
    }
    if (ch == '"') return parseString(out);
    if (ch == '[') return parseArray(out, depth);
    if (ch == '{') return parseObject(out, depth);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return parseNumber(out);
    return fail(std::string("无法识别的字符 '") + ch + "'");
  }

  bool parseString(Value* out) {
    ++index_;  // 开头的引号
    std::string text;
    while (index_ < text_.size()) {
      const char ch = text_[index_++];
      if (ch == '"') {
        *out = Value(text);
        return true;
      }
      if (ch != '\\') {
        text.push_back(ch);
        continue;
      }
      if (index_ >= text_.size()) return fail("字符串在转义符后结束");
      const char escape = text_[index_++];
      switch (escape) {
        case '"': text.push_back('"'); break;
        case '\\': text.push_back('\\'); break;
        case '/': text.push_back('/'); break;
        case 'b': text.push_back('\b'); break;
        case 'f': text.push_back('\f'); break;
        case 'n': text.push_back('\n'); break;
        case 'r': text.push_back('\r'); break;
        case 't': text.push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          if (!readHex4(&code)) return fail("\\u 后面必须是 4 位十六进制");
          // 代理对：JSON 用 \uD800-\uDFFF 表示 BMP 之外的字符。
          if (code >= 0xD800 && code <= 0xDBFF) {
            if (index_ + 1 >= text_.size() || text_[index_] != '\\' ||
                text_[index_ + 1] != 'u') {
              return fail("高代理项后面缺少 \\u");
            }
            index_ += 2;
            unsigned low = 0;
            if (!readHex4(&low)) return fail("\\u 后面必须是 4 位十六进制");
            if (low < 0xDC00 || low > 0xDFFF) return fail("低代理项不合法");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          appendUtf8(code, &text);
          break;
        }
        default:
          return fail("不认识的转义符");
      }
    }
    return fail("字符串没有结束引号");
  }

  bool readHex4(unsigned* code) {
    if (index_ + 4 > text_.size()) return false;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = text_[index_++];
      value <<= 4;
      if (ch >= '0' && ch <= '9') {
        value |= static_cast<unsigned>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        value |= static_cast<unsigned>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        value |= static_cast<unsigned>(ch - 'A' + 10);
      } else {
        return false;
      }
    }
    *code = value;
    return true;
  }

  static void appendUtf8(unsigned code, std::string* out) {
    if (code <= 0x7F) {
      out->push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool parseNumber(Value* out) {
    const std::size_t begin = index_;
    if (text_[index_] == '-') ++index_;
    while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
      ++index_;
    }
    bool isFloat = false;
    if (index_ < text_.size() && text_[index_] == '.') {
      isFloat = true;
      ++index_;
      while (index_ < text_.size() && text_[index_] >= '0' &&
             text_[index_] <= '9') {
        ++index_;
      }
    }
    if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
      isFloat = true;
      ++index_;
      if (index_ < text_.size() && (text_[index_] == '+' || text_[index_] == '-')) {
        ++index_;
      }
      while (index_ < text_.size() && text_[index_] >= '0' &&
             text_[index_] <= '9') {
        ++index_;
      }
    }

    const std::string token = text_.substr(begin, index_ - begin);
    if (token.empty() || token == "-") return fail("数字格式不对");

    if (isFloat) {
      *out = Value(std::strtod(token.c_str(), nullptr));
      return true;
    }

    // 整数：先按 long long 解析，失败说明超出 64 位。
    errno = 0;
    const long long number = std::strtoll(token.c_str(), nullptr, 10);
    if (errno == ERANGE) {
      return fail("整数超出 64 位范围（" + token + "）");
    }
    *out = Value(number);
    return true;
  }

  bool parseArray(Value* out, int depth) {
    ++index_;  // '['
    List items;
    skipSpace();
    if (index_ < text_.size() && text_[index_] == ']') {
      ++index_;
      *out = Value::list(std::move(items));
      return true;
    }
    for (;;) {
      skipSpace();
      Value item;
      if (!parseValue(&item, depth + 1)) return false;
      items.push_back(std::move(item));
      skipSpace();
      if (index_ >= text_.size()) return fail("数组没有结束括号");
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == ']') {
        ++index_;
        *out = Value::list(std::move(items));
        return true;
      }
      return fail("数组里应当是 ',' 或 ']'");
    }
  }

  bool parseObject(Value* out, int depth) {
    ++index_;  // '{'
    List keys;
    List values;
    skipSpace();
    if (index_ < text_.size() && text_[index_] == '}') {
      ++index_;
      *out = Value::dict(keys, values);
      return true;
    }
    for (;;) {
      skipSpace();
      if (index_ >= text_.size() || text_[index_] != '"') {
        return fail("对象的键必须是字符串");
      }
      Value key;
      if (!parseString(&key)) return false;
      skipSpace();
      if (index_ >= text_.size() || text_[index_] != ':') {
        return fail("对象的键后面应当是 ':'");
      }
      ++index_;
      skipSpace();
      Value value;
      if (!parseValue(&value, depth + 1)) return false;
      keys.push_back(std::move(key));
      values.push_back(std::move(value));
      skipSpace();
      if (index_ >= text_.size()) return fail("对象没有结束括号");
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == '}') {
        ++index_;
        *out = Value::dict(keys, values);
        return true;
      }
      return fail("对象里应当是 ',' 或 '}'");
    }
  }

  const std::string& text_;
  std::size_t index_ = 0;
  std::string error_;
};

}  // namespace

bool serialize(const Value& value, std::string* text, std::string* error) {
  std::string out;
  if (!serializeImpl(value, &out, error, 0)) return false;
  text->swap(out);
  return true;
}

bool parse(const std::string& text, Value* value, std::string* error) {
  Reader reader(text);
  return reader.parse(value, error);
}

}  // namespace json
}  // namespace aipy
