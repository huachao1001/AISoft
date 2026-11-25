// Unicode 字符串工具：按「字符」而不是「字节」处理文本时全都要用到它。
//
// ── 为什么单独一个编译单元 ──
//
// 这些函数原本和绘制代码放在同一个文件里，谁需要就链整个绘制层。但需要
// 它们的不只有绘制：语言层的 Value（len("中文")、字符串下标）也要它，
// 而语言运行时不该被拖进图形代码。放在这里之后依赖方向是
//   value / widgets → utf8 → 无
// 单链一个 utf8.cpp 就能量出「字符串语义」的体积。
//
// 实现仍然极简：只做边界扫描，不做正规化、不做大小写折叠。
#include "aipy/utf8.h"

namespace aipy {
namespace {

// UTF-8 首字节 → 该字符的总字节数；非法首字节返回 1，保证游标一定前进。
size_t utf8CharLength(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead >> 5) == 0x6) return 2;
  if ((lead >> 4) == 0xE) return 3;
  if ((lead >> 3) == 0x1E) return 4;
  return 1;
}

}  // namespace

size_t utf8PrefixLength(std::string_view text, size_t maxBytes) {
  if (maxBytes >= text.size()) return text.size();
  size_t i = 0;
  while (i < text.size()) {
    const size_t len = utf8CharLength(static_cast<unsigned char>(text[i]));
    if (i + len > maxBytes) break;
    i += len;
  }
  return i;
}

size_t utf8Length(std::string_view text) {
  size_t count = 0;
  size_t i = 0;
  while (i < text.size()) {
    i += utf8CharLength(static_cast<unsigned char>(text[i]));
    ++count;
  }
  return count;
}

size_t utf8PrefixByChars(std::string_view text, size_t charCount) {
  size_t i = 0;
  size_t seen = 0;
  while (i < text.size() && seen < charCount) {
    i += utf8CharLength(static_cast<unsigned char>(text[i]));
    ++seen;
  }
  return i < text.size() ? i : text.size();
}

size_t utf8OffsetOfChar(std::string_view text, size_t charIndex) {
  size_t i = 0;
  size_t seen = 0;
  while (i < text.size() && seen < charIndex) {
    i += utf8CharLength(static_cast<unsigned char>(text[i]));
    ++seen;
  }
  return i < text.size() ? i : text.size();
}

size_t utf8AlignDown(std::string_view text, size_t byteOffset) {
  if (byteOffset >= text.size()) return text.size();
  size_t i = 0;
  while (i < text.size() && i < byteOffset) {
    const size_t next = i + utf8CharLength(static_cast<unsigned char>(text[i]));
    if (next > byteOffset) return i;  // 落在字符中间，退到该字符起点
    i = next;
  }
  return i < text.size() ? i : text.size();
}

size_t utf8NextBoundary(std::string_view text, size_t byteOffset) {
  const size_t at = utf8AlignDown(text, byteOffset);
  if (at >= text.size()) return text.size();
  const size_t next = at + utf8CharLength(static_cast<unsigned char>(text[at]));
  return next < text.size() ? next : text.size();
}

size_t utf8PrevBoundary(std::string_view text, size_t byteOffset) {
  size_t at = utf8AlignDown(text, byteOffset);
  if (at == 0) return 0;
  // 退一个字节再回退到字符起点：从字符中间出发也能正确退到前一个字符
  --at;
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return at;
}

std::string utf8Encode(std::uint32_t codepoint) {
  std::string out;
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return out;
}

}
