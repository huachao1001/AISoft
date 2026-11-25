// 极简测试框架：不引 gtest/catch2
//
// 理由和 XML 解析器一样——这几个宏足够跑完所有控件测试，而第三方测试库
// 会变成构建依赖，还得在两个平台上各配一遍。这里只要一个 main 顺序跑完
// 所有用例、失败时打印文件行号、最后返回失败数。
//
// 用例注册用静态对象：`TEST(名字)` 展开成一个类 + 一个静态实例，
// 静态初始化阶段自己登记进注册表。这样新增测试文件不用改任何清单。
#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace aipy {
namespace test {

struct Case {
  std::string name;
  std::function<void()> fn;
};

std::vector<Case>& registry();

// 当前用例失败计数。断言只增加计数不抛异常：控件测试里常见「一处画错
// 导致后面几条也失败」，一次跑完能一次改完，比每错一条就中断强。
int& currentFailures();

void reportFailure(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back(Case{name, std::move(fn)}); }
};

// 字符串化辅助：断言失败时打印实际值
template <typename T>
std::string show(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string show(const std::string& value) { return '"' + value + '"'; }
inline std::string show(bool value) { return value ? "true" : "false"; }

}  // namespace test
}

#define TEST(name)                                                     \
  static void aipyTest_##name();                                       \
  static ::aipy::test::Registrar aipyTestRegistrar_##name(#name, &aipyTest_##name); \
  static void aipyTest_##name()

#define FAIL_AT(message) ::aipy::test::reportFailure(__FILE__, __LINE__, (message))

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) FAIL_AT(std::string("断言失败: ") + #cond);                  \
  } while (false)

#define CHECK_MSG(cond, message)                                             \
  do {                                                                       \
    if (!(cond)) FAIL_AT(std::string("断言失败: ") + #cond + " —— " + (message)); \
  } while (false)

#define CHECK_EQ(actual, expected)                                           \
  do {                                                                       \
    const auto& a_ = (actual);                                               \
    const auto& e_ = (expected);                                             \
    if (!(a_ == e_)) {                                                       \
      FAIL_AT(std::string(#actual " 应为 ") + ::aipy::test::show(e_) + "，实际 " + \
              ::aipy::test::show(a_));                                       \
    }                                                                        \
  } while (false)

// 浮点比较：控件里的坐标是取整后的像素，容差只用于确实会引入误差的地方
#define CHECK_NEAR(actual, expected, tolerance)                              \
  do {                                                                       \
    const double a_ = static_cast<double>(actual);                           \
    const double e_ = static_cast<double>(expected);                         \
    const double d_ = a_ > e_ ? a_ - e_ : e_ - a_;                           \
    if (d_ > static_cast<double>(tolerance)) {                               \
      FAIL_AT(std::string(#actual " 应为 ") + ::aipy::test::show(e_) + "（容差 " + \
              ::aipy::test::show(static_cast<double>(tolerance)) + "），实际 " + \
              ::aipy::test::show(a_));                                       \
    }                                                                        \
  } while (false)
