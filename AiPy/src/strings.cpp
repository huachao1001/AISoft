// 字符串方法与 str.format 的格式说明符支持。
//
// 与 value.cpp 里的字符串内建分开：那些是「一个函数一个语义」的朴素实现，
// 这里是 Python str 类型上的方法集合，条数多、签名统一（都是
// `(receiver, args)`），放一个文件里更容易看出「还差哪些」。
#include "aipy/utf8.h"
#include "aipy/value.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace aipy {
namespace {

bool isSpaceByte(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// 按字符（不是字节）补到指定宽度。中文一个字符占 3 字节，
// 按字节补会让 "ab".ljust(4) 与 "中文".ljust(4) 宽度不一致。
std::string padTo(std::string text, long long width, char fill, bool left,
                  bool right) {
  const long long count = static_cast<long long>(utf8Length(text));
  if (width <= count) return text;
  const std::string padding(static_cast<size_t>(width - count), fill);
  if (left && right) {
    // center：多出来的那一个放在右边，与 CPython 一致。
    const size_t leftCount = padding.size() / 2;
    return padding.substr(0, leftCount) + text + padding.substr(leftCount);
  }
  if (left) return padding + text;
  return text + padding;
}

}  // namespace

// ── 拆分与修剪 ──

Value splitlines(const Value& text) {
  const std::string source = text.toString();
  Value::List parts;
  std::string current;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\n') {
      parts.emplace_back(std::move(current));
      current.clear();
      continue;
    }
    if (c == '\r') {
      // \r\n 只算一个换行；单独的 \r 也算。
      if (i + 1 < source.size() && source[i + 1] == '\n') ++i;
      parts.emplace_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  // 末尾没有换行时，剩下的才是最后一行；以换行结尾不会多出一个空串。
  if (!current.empty()) parts.emplace_back(std::move(current));
  return Value::list(std::move(parts));
}

Value lstrip(const Value& text) {
  const std::string source = text.toString();
  std::size_t begin = 0;
  while (begin < source.size() && isSpaceByte(static_cast<unsigned char>(source[begin]))) {
    ++begin;
  }
  return Value(source.substr(begin));
}

Value rstrip(const Value& text) {
  const std::string source = text.toString();
  std::size_t end = source.size();
  while (end > 0 && isSpaceByte(static_cast<unsigned char>(source[end - 1]))) --end;
  return Value(source.substr(0, end));
}

Value title(const Value& text) {
  const std::string source = text.toString();
  std::string out;
  out.reserve(source.size());
  // 词边界是「非字母」：CPython 里 "a-b".title() == "A-B"。
  bool atWordStart = true;
  for (char c : source) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (alpha) {
      if (atWordStart && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      else if (!atWordStart && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      atWordStart = false;
    } else {
      atWordStart = true;
    }
    out.push_back(c);
  }
  return Value(std::move(out));
}

// ── 补齐 ──

Value zfill(const Value& text, const Value& width) {
  std::string source = text.toString();
  const long long wanted = width.toInt();
  const long long count = static_cast<long long>(utf8Length(source));
  if (wanted <= count) return Value(std::move(source));
  const std::size_t padding = static_cast<std::size_t>(wanted - count);
  // 符号留在最前面："-1".zfill(4) == "-001"
  if (!source.empty() && (source[0] == '-' || source[0] == '+')) {
    return Value(source.substr(0, 1) + std::string(padding, '0') + source.substr(1));
  }
  return Value(std::string(padding, '0') + source);
}

Value ljust(const Value& text, const Value& width) {
  return Value(padTo(text.toString(), width.toInt(), ' ', false, false));
}

Value rjust(const Value& text, const Value& width) {
  return Value(padTo(text.toString(), width.toInt(), ' ', true, false));
}

Value center(const Value& text, const Value& width) {
  return Value(padTo(text.toString(), width.toInt(), ' ', true, true));
}

// ── 判定 ──

Value isdigit(const Value& text) {
  const std::string source = text.toString();
  if (source.empty()) return Value(false);
  for (char c : source) {
    if (c < '0' || c > '9') return Value(false);
  }
  return Value(true);
}

Value isalpha(const Value& text) {
  const std::string source = text.toString();
  if (source.empty()) return Value(false);
  for (char c : source) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!alpha) return Value(false);
  }
  return Value(true);
}

Value isalnum(const Value& text) {
  const std::string source = text.toString();
  if (source.empty()) return Value(false);
  for (char c : source) {
    const bool digit = c >= '0' && c <= '9';
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!digit && !alpha) return Value(false);
  }
  return Value(true);
}

Value isSpaceText(const Value& text) {
  const std::string source = text.toString();
  if (source.empty()) return Value(false);
  for (char c : source) {
    if (!isSpaceByte(static_cast<unsigned char>(c))) return Value(false);
  }
  return Value(true);
}

Value islower(const Value& text) {
  const std::string source = text.toString();
  bool hasCased = false;
  for (char c : source) {
    const bool lower = c >= 'a' && c <= 'z';
    const bool upper = c >= 'A' && c <= 'Z';
    if (upper) return Value(false);
    if (lower) hasCased = true;
  }
  return Value(hasCased);
}

Value isupper(const Value& text) {
  const std::string source = text.toString();
  bool hasCased = false;
  for (char c : source) {
    const bool lower = c >= 'a' && c <= 'z';
    const bool upper = c >= 'A' && c <= 'Z';
    if (lower) return Value(false);
    if (upper) hasCased = true;
  }
  return Value(hasCased);
}

}  // namespace aipy
