// 极简测试框架：用例注册、断言、统一入口
//
// 断言失败抛 Failure，由 runner 捕获后记一条失败并继续跑下一个用例。
// 用异常而不是提前 return，是为了让 RAII 清理（如恢复平台后端）照常执行。
#pragma once

#include <string>
#include <vector>

namespace aiui_test {

using TestFn = void (*)();

struct TestCase {
  const char* name;
  TestFn fn;
};

std::vector<TestCase>& registry();

// 静态对象构造时把用例塞进全局表
struct Registrar {
  Registrar(const char* name, TestFn fn) { registry().push_back(TestCase{name, fn}); }
};

struct Failure {
  const char* file;
  int line;
  std::string message;
};

[[noreturn]] void fail(const char* file, int line, std::string message);

// 宽字符串降级成窄字符串，失败信息里可读
std::string narrow(const std::wstring& text);

}  // namespace aiui_test

#define AIUI_TEST(name)                                        \
  static void name();                                          \
  static ::aiui_test::Registrar name##_registrar(#name, &name); \
  static void name()

#define AIUI_CHECK(cond) \
  do {                   \
    if (!(cond)) ::aiui_test::fail(__FILE__, __LINE__, "条件不成立：" #cond); \
  } while (0)

#define AIUI_CHECK_EQ(actual, expected)                              \
  do {                                                               \
    const auto& actualValue = (actual);                              \
    const auto& expectedValue = (expected);                          \
    if (!(actualValue == expectedValue)) {                            \
      ::aiui_test::fail(__FILE__, __LINE__,                           \
                        "值不相等：" #actual " != " #expected);        \
    }                                                                \
  } while (0)
