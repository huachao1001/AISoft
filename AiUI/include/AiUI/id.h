// 视图 id：把 XML 里的字符串 id 编译成 32 位整数
//
// findViewById 只比较整数，字符串在构建期就算完，运行时不建表、不查表。
// 用 FNV-1a：同一条字符串在任何平台、任何编译器下都得到同一个值，
// 布局生成代码与宿主代码各自算出常量即可对上，不需要共享符号。
#pragma once

#include <cstdint>

namespace AiUI {
namespace detail {

// FNV-1a 32 位：质数与偏移量取标准值
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

constexpr std::uint32_t fnv1a(const char* text, std::uint32_t hash) {
  return *text == '\0'
             ? hash
             : fnv1a(text + 1, (hash ^ static_cast<std::uint32_t>(
                                            static_cast<unsigned char>(*text))) *
                                   kFnvPrime);
}

}  // namespace detail

// 由字符串算出 id。0 保留给"没有 id"，所以哈希撞到 0 时改用 1。
constexpr int makeId(const char* text) {
  const std::uint32_t hash = detail::fnv1a(text, detail::kFnvOffset);
  return static_cast<int>(hash == 0u ? 1u : hash);
}

}  // namespace AiUI
