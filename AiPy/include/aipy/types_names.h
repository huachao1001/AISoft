// 内建类型名清单：哪些名字在值位置会被当成「类型」。
//
// ── 为什么与 exception.h 分开 ──
//
// 两者回答的是不同的问题：异常类要判「except 能不能接住」，类型名要判
// 「这个名字作值有没有意义」。`int` 是类型但不是异常，`KeyError` 是异常
// 也是类型。混成一张表会让 `except int:` 看起来合法。
//
// ── 用途 ──
//
// 编译器看到 `int`、`str` 出现在**值位置**（`type(x) is int`、
// `isinstance(x, str)`）时发 LOAD_TYPE；VM 按名字造一个类值。
// 只在 `len(x)` 这种调用位置出现的名字不走这里。
#pragma once

#include <cstddef>
#include <string>

namespace aipy {

struct BuiltinTypeName {
  const char* name;
};

// 只收 `type()` 会返回、且脚本真会拿来比的那批。CPython 的内建类型有
// 几十个，把用不到的搬来只是多一份没人测的清单。
inline constexpr BuiltinTypeName kBuiltinTypeNames[] = {
    {"NoneType"}, {"bool"},   {"int"},    {"float"},  {"complex"},
    {"str"},      {"bytes"},  {"list"},   {"tuple"},  {"dict"},
    {"set"},      {"range"},  {"object"}, {"type"},
};

inline constexpr std::size_t kBuiltinTypeNameCount =
    sizeof(kBuiltinTypeNames) / sizeof(kBuiltinTypeNames[0]);

inline bool isBuiltinTypeName(const std::string& name) {
  for (std::size_t i = 0; i < kBuiltinTypeNameCount; ++i) {
    if (name == kBuiltinTypeNames[i].name) return true;
  }
  return false;
}

}  // namespace aipy
