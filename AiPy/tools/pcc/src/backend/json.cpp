// 极简 JSON 读取的实现。支持范围见头文件。
#include "pcc/backend/json.h"

#include <cstdlib>

namespace pcc {
namespace {

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  bool parse(JsonValue* out) {
    skipSpace();
    if (!value(out)) return false;
    skipSpace();
    return pos_ >= text_.size();
  }

 private:
  void skipSpace() {
    while (pos_ < text_.size()) {
      const char ch = text_[pos_];
      if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break;
      ++pos_;
    }
  }

  bool value(JsonValue* out) {
    skipSpace();
    if (pos_ >= text_.size()) return false;
    switch (text_[pos_]) {
      case '"': return string(out);
      case '{': return object(out);
      case '[': return array(out);
      case 't': return literal(out, "true", JsonValue::Type::Bool);
      case 'f': return literal(out, "false", JsonValue::Type::Bool);
      case 'n': return literal(out, "null", JsonValue::Type::Null);
      default: return number(out);
    }
  }

  bool literal(JsonValue* out, const char* word, JsonValue::Type type) {
    const std::string text(word);
    if (text_.compare(pos_, text.size(), text) != 0) return false;
    pos_ += text.size();
    out->type = type;
    out->text = text;
    return true;
  }

  bool string(JsonValue* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    std::string result;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') {
        out->type = JsonValue::Type::String;
        out->text = result;
        return true;
      }
      if (ch != '\\') {
        result.push_back(ch);
        continue;
      }
      if (pos_ >= text_.size()) return false;
      switch (text_[pos_++]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          // 用到 \u 的只可能是路径里的非 ASCII 字符（json.dumps 默认
          // ensure_ascii=True）。这里按 UTF-8 还原，含代理对。
          unsigned code = 0;
          if (!hex4(&code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned low = 0;
            if (!hex4(&low)) return false;
            code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
          }
          appendUtf8(code, &result);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool hex4(unsigned* out) {
    if (pos_ + 4 > text_.size()) return false;
    const std::string digits = text_.substr(pos_, 4);
    pos_ += 4;
    *out = static_cast<unsigned>(std::strtoul(digits.c_str(), nullptr, 16));
    return true;
  }

  static void appendUtf8(unsigned code, std::string* out) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
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

  bool number(JsonValue* out) {
    const std::size_t begin = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_];
      const bool part = (ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' ||
                        ch == 'E' || ch == '+' || ch == '-';
      if (!part) break;
      ++pos_;
    }
    if (pos_ == begin) return false;
    out->type = JsonValue::Type::Number;
    out->text = text_.substr(begin, pos_ - begin);
    return true;
  }

  bool array(JsonValue* out) {
    ++pos_;  // '['
    out->type = JsonValue::Type::Array;
    skipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      JsonValue item;
      if (!value(&item)) return false;
      out->items.push_back(std::move(item));
      skipSpace();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool object(JsonValue* out) {
    ++pos_;  // '{'
    out->type = JsonValue::Type::Object;
    skipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skipSpace();
      JsonValue key;
      if (!string(&key)) return false;
      skipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return false;
      ++pos_;
      JsonValue item;
      if (!value(&item)) return false;
      out->fields[key.text] = std::move(item);
      skipSpace();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

}  // namespace

const JsonValue* JsonValue::get(const std::string& key) const {
  const auto found = fields.find(key);
  return found == fields.end() ? nullptr : &found->second;
}

std::string JsonValue::getString(const std::string& key,
                                 const std::string& defaultText) const {
  const JsonValue* found = get(key);
  if (found == nullptr || !found->isString()) return defaultText;
  return found->text;
}

int JsonValue::getInt(const std::string& key, int defaultInt) const {
  const JsonValue* found = get(key);
  if (found == nullptr || found->type != Type::Number) return defaultInt;
  return std::atoi(found->text.c_str());
}

bool parseJson(const std::string& text, JsonValue* out) {
  JsonReader reader(text);
  return reader.parse(out);
}

std::vector<std::string> jsonStringArray(const JsonValue& owner,
                                         const std::string& key) {
  std::vector<std::string> out;
  const JsonValue* found = owner.get(key);
  if (found == nullptr || !found->isArray()) return out;
  for (const JsonValue& item : found->items) {
    if (item.isString()) out.push_back(item.text);
  }
  return out;
}

}  // namespace pcc
