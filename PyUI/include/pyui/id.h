// 把字符串折成 32 位整数
//
// 视图 id 在编译期折成整数：XML 里写 id="ok"，生成的代码是
// setId(makeId("ok"))，业务代码 findView("ok") 也走同一个常量。运行期只做
// 整数比较，不建字符串表、不查字典。
//
// 用 FNV-1a：同一条字符串在任何平台、任何编译器下都得到同一个值，因此生成
// 的布局代码与业务代码各自算常量即可对上，不需要共享符号表。
#pragma once

#include <cstdint>

namespace pyui {
namespace detail {

constexpr std::uint32_t kFnvOffset32 = 2166136261u;
constexpr std::uint32_t kFnvPrime32 = 16777619u;
constexpr std::uint64_t kFnvOffset64 = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime64 = 1099511628211ull;

constexpr std::uint32_t fnv1a32(const char* text, std::uint32_t hash) {
  return *text == '\0'
             ? hash
             : fnv1a32(text + 1,
                       (hash ^ static_cast<std::uint32_t>(static_cast<unsigned char>(*text))) *
                           kFnvPrime32);
}

constexpr std::uint64_t fnv1a64(const char* text, std::uint64_t hash) {
  return *text == '\0'
             ? hash
             : fnv1a64(text + 1,
                       (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(*text))) *
                           kFnvPrime64);
}

}  // namespace detail

// 由字符串算出视图 id。0 保留给「没有 id」，哈希撞到 0 时改用 1。
constexpr int makeId(const char* text) {
  const std::uint32_t hash = detail::fnv1a32(text, detail::kFnvOffset32);
  return static_cast<int>(hash == 0u ? 1u : hash);
}

constexpr std::uint64_t makeHash64(const char* text) {
  return detail::fnv1a64(text, detail::kFnvOffset64);
}

// 运行期对已取到的字节串算同一种哈希（读文件、比对内容时用）
std::uint32_t hashBytes32(const char* data, std::size_t size);
std::uint64_t hashBytes64(const char* data, std::size_t size);

}  // namespace pyui
