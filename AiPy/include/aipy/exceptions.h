// 内建异常类清单。
//
// ── 为什么单独一个头 ──
//
// 两处都要它，且必须完全一致：
//   - 编译器（tools/pcc）要在编译期认出 `except ValueError:` / `raise KeyError`
//     里的名字，拼错时报错而不是留到运行期；
//   - VM（src/value.cpp）要判 `except Exception` 能不能接住 ValueError。
//
// 各写一份必然慢慢分叉：编译器认的名字运行期不认，症状是「编译通过、运行时
// 这个 except 永远不匹配」——不报错，只是行为悄悄错。
//
// ── 为什么只有这些 ──
//
// 收的是脚本里真能遇到的。CPython 有六十多个，把用不到的也搬来只会多一份
// 需要维护的清单，而且没有任何测试会覆盖它们。
#pragma once

#include <cstddef>
#include <string>

namespace aipy {

struct ExceptionClass {
  const char* name;
  const char* parent;  // nullptr 表示直接继承 BaseException
};

inline constexpr ExceptionClass kExceptionClasses[] = {
    {"BaseException", nullptr},
    {"Exception", "BaseException"},
    {"SystemExit", "BaseException"},
    {"KeyboardInterrupt", "BaseException"},

    {"ArithmeticError", "Exception"},
    {"ZeroDivisionError", "ArithmeticError"},
    {"OverflowError", "ArithmeticError"},

    {"LookupError", "Exception"},
    {"IndexError", "LookupError"},
    {"KeyError", "LookupError"},

    {"ValueError", "Exception"},
    {"TypeError", "Exception"},
    {"NameError", "Exception"},
    {"AttributeError", "Exception"},
    {"RuntimeError", "Exception"},
    {"RecursionError", "RuntimeError"},
    {"NotImplementedError", "RuntimeError"},
    {"OSError", "Exception"},
    {"FileNotFoundError", "OSError"},
    {"PermissionError", "OSError"},
    {"FileExistsError", "OSError"},
    {"IsADirectoryError", "OSError"},
    {"UnicodeError", "ValueError"},
    {"StopIteration", "Exception"},
    {"AssertionError", "Exception"},
};

inline constexpr std::size_t kExceptionClassCount =
    sizeof(kExceptionClasses) / sizeof(kExceptionClasses[0]);

// 这个名字是不是一个内建异常类。
inline bool isExceptionClassName(const std::string& name) {
  for (std::size_t i = 0; i < kExceptionClassCount; ++i) {
    if (name == kExceptionClasses[i].name) return true;
  }
  return false;
}

// 直接父类的名字；不在表里返回 nullptr。
inline const char* exceptionParentOf(const std::string& name) {
  for (std::size_t i = 0; i < kExceptionClassCount; ++i) {
    if (name == kExceptionClasses[i].name) return kExceptionClasses[i].parent;
  }
  return nullptr;
}

}  // namespace aipy
