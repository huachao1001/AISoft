// 内建函数表：语言自带的那批函数（len/str/range…）。
//
// ── 为什么是「表」而不是直接调用 ──
//
// 脚本在运行期才加载，编译器不知道运行期会遇见哪个内建，所以只能把名字
// 编成下标、让 VM 按下标取函数指针。一次间接跳转的成本（实测约 94 ns）
// 换来「改脚本不用重编译」。
//
// ── 与扩展的区别 ──
//
// 内建永远在（语言的一部分）；扩展只在 import 了才链接进来。「用了才链接」
// 的筛选发生在编译期，两张表只是让报错能分清「语言没有这个函数」和
// 「扩展没链接进来」——这两种错误修法完全不同。
#pragma once

#include "aipy/value.h"

#include <string>

namespace aipy {

// 内建与扩展的统一调用形状：裸函数指针 + 指针/个数。
// 不用 std::function：DLL 与静态库要能共用同一签名，调用边界上也不分配。
using BuiltinFn = Value (*)(const Value* args, int count, std::string* error);

// 表的大小与查表。
int builtinCount();
const char* builtinName(int index);
BuiltinFn builtinAt(int index);

// 名字 → 下标，没有返回 -1。编译器用它把 `len(x)` 定成下标。
int builtinIndexFromName(const std::string& name);

// 关键字实参的名字 → 位置实参下标；这个名字的内建不接受该关键字时返回 -1。
//
// 内建签名统一是 `(args, count, error)`，没有形参名，所以 `print(sep="-")`
// 这类写法要先在 VM 里按名字换成位置。表见 builtins_keywords.cpp。
int builtinKeywordPosition(const std::string& builtin,
                           const std::string& keyword);

// 这个内建接受关键字实参吗（不接受时明确报 TypeError，与 CPython 一致）。
bool builtinTakesKeywords(const std::string& builtin);

// 仅关键字参数（sorted 的 key/reverse）：不在位置表里，但必须被接受。
bool builtinAcceptsKeyword(const std::string& builtin,
                           const std::string& keyword);

// print 的 sep/end 形式。
//
// 内建签名统一是 `(args, count, error)`，放不下两个可选的尾部参数，
// 所以 `print(a, b, sep="-", end="")` 由 VM 单独调这个入口。
Value printWithSeparators(const Value* args, int count, const Value& sep,
                          const Value& end);

}  // namespace aipy
