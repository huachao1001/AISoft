// UTF-8 字符串工具
//
// 为什么独立成头：需要这些函数的层不止绘制层。语言层的 len()/下标/切片
// 要按字符算，而它不该为了「数一个中文字符」把整个图形层链进来。
//
// 约定：所有函数都只做**边界扫描**，不校验合法性、不做正规化。
// 遇到非法字节就前进 1 字节，保证调用方的循环一定终止。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aipy {

// text 的前 maxBytes 字节能完整覆盖多少个字符（不会切断字符）。
size_t utf8PrefixLength(std::string_view text, size_t maxBytes);

// 字符数（不是字节数）。中文 1 个字算 1。
size_t utf8Length(std::string_view text);

// 前 charCount 个字符占多少字节。
size_t utf8PrefixByChars(std::string_view text, size_t charCount);

// 第 charIndex 个字符的起始字节偏移；越界返回 text.size()。
size_t utf8OffsetOfChar(std::string_view text, size_t charIndex);

// 把任意字节偏移退到所在字符的起点。
size_t utf8AlignDown(std::string_view text, size_t byteOffset);

// byteOffset 所在字符之后的下一个字符起点。
size_t utf8NextBoundary(std::string_view text, size_t byteOffset);

// byteOffset 所在字符之前的上一个字符起点。
size_t utf8PrevBoundary(std::string_view text, size_t byteOffset);

// 码点 → UTF-8 字节串。
std::string utf8Encode(std::uint32_t codepoint);

}
