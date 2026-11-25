// AiPy 独立性的活证据，也是一个可用的最小示例。
//
// 它**只链 aipy**：没有窗口、没有控件、没有任何界面头。能用能跑，就说明
// 「Python 语义层」真的可以脱离 PyUI 存在——这是项目边界的唯一可信证据，
// 比在 CMakeLists 里读依赖边可靠（依赖边会被 PUBLIC 传递进来）。
//
// 输出同时被 CTest 断言，所以它不只是演示，而是一个回归测试：
// 哪天某个内建悄悄改了行为，这里会红。
#include "aipy/value.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using aipy::Value;

  // 算术与真值
  const Value sum = aipy::add(Value(2LL), Value(40LL));
  expect(sum.toInt() == 42, "2 + 40 == 42");
  expect(Value(0).toBool() == false, "0 为假");
  expect(Value(std::string("")).toBool() == false, "空串为假");

  // 字符串内建（这些曾经在桥接层，现在属于语言本身）
  expect(aipy::upper(Value(std::string("aipy"))).toString() == "AIPY", "upper");
  expect(aipy::lower(Value(std::string("AIPY"))).toString() == "aipy", "lower");
  expect(aipy::strip(Value(std::string("  x  "))).toString() == "x", "strip");

  const Value parts =
      aipy::split(Value(std::string("a,b,c")), Value(std::string(",")));
  expect(aipy::length(parts).toInt() == 3, "split 出 3 段");
  expect(aipy::join(Value(std::string("-")), parts).toString() == "a-b-c", "join");

  expect(aipy::startsWith(Value(std::string("aipy")), Value(std::string("ai"))).toBool(),
         "startswith");
  expect(aipy::findIn(Value(std::string("aipy")), Value(std::string("py"))).toInt() == 2,
         "find 返回字符下标");

  // 容器引用语义：Python 的 b = a 是共享，不是拷贝
  Value list = Value::list({Value(1LL), Value(2LL)});
  Value alias = list;
  aipy::append(alias, Value(3LL));
  expect(aipy::length(list).toInt() == 3, "列表赋值是共享而不是深拷贝");

  // 字典保持插入顺序
  Value dict = Value::dict({Value(std::string("a")), Value(std::string("b"))},
                           {Value(1LL), Value(2LL)});
  expect(aipy::dictGet(dict, Value(std::string("b"))).toInt() == 2, "字典取值");
  expect(dict.dictKeys().size() == 2, "字典键数量");

  // UTF-8：按字符而不是字节
  expect(aipy::length(Value(std::string("中文"))).toInt() == 2, "中文算 2 个字符");
  expect(aipy::at(Value(std::string("中文")), Value(1LL)).toString() == "文",
         "按字符下标取");

  // 内建
  expect(aipy::maximum(Value(3LL), Value(7LL)).toInt() == 7, "max");
  expect(aipy::parseNumber(Value(std::string("12.5"))).toFloat() == 12.5, "parse_number");
  expect(aipy::parseNumber(Value(std::string("12abc"))).isNone(), "parse_number 拒绝脏串");
  expect(aipy::charFromCode(Value(0x4E2DLL)).toString() == "中", "char_from_code");

  std::printf("aipy standalone: %s\n", failures == 0 ? "OK" : "FAILED");
  return failures == 0 ? 0 : 1;
}
