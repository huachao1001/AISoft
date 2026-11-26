// 内建 `type(x)` 与 `isinstance(x, T)`。
//
// 两者都要把「类型」当作可比较的值，所以返回/接受的是内建类值
// （`makeBuiltinClass`），显示成 `<class 'int'>`，与 CPython 一致。
#include "aipy/value.h"

#include <string>

namespace aipy {
namespace builtins {

namespace {

// 脚本类实例的类型名就是它的类名；其它值走 typeName 的规则。
//
// 脚本类带上 `__main__.` 前缀：CPython 里单文件脚本的类 `__module__`
// 就是 `__main__`，`type(A())` 显示 `<class '__main__.A'>`。不加前缀的
// 症状是「打印出来少一截」，与 CPython 逐行对拍时会立刻暴露。
std::string displayTypeName(const Value& value) {
  if (isInstance(value)) {
    const Value cls = instanceClass(value);
    if (isClass(cls)) return "__main__." + classNameOf(cls);
  }
  return typeName(value);
}

// isinstance 比较用的类型名：不带模块前缀，与 `class A:` 定义的名字一致。
std::string comparableTypeName(const Value& value) {
  if (isInstance(value)) {
    const Value cls = instanceClass(value);
    if (isClass(cls)) return classNameOf(cls);
  }
  return typeName(value);
}

// 类层次里有没有 wantedName。深度优先、左到右，与 MRO 的查找顺序一致。
// 递归深度上限兜底：类表写错成环时宁可判不匹配，也不要在 isinstance 里
// 死循环——那会把脚本错误变成程序卡死。
bool classHierarchyHas(const Value& cls, const std::string& wantedName,
                       int depth) {
  if (depth > 64 || !isClass(cls)) return false;
  if (classNameOf(cls) == wantedName) return true;
  for (const Value& base : classBases(cls)) {
    if (classHierarchyHas(base, wantedName, depth + 1)) return true;
  }
  return false;
}

}  // namespace

Value builtinType(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "type() takes exactly one argument (" + std::to_string(count) +
             " given)";
    return Value();
  }
  return makeBuiltinClass(displayTypeName(args[0]));
}

Value builtinIsInstance(const Value* args, int count, std::string* error) {
  if (count != 2) {
    *error = "isinstance() takes exactly two arguments (" +
             std::to_string(count) + " given)";
    return Value();
  }
  const Value& value = args[0];
  const Value& wanted = args[1];

  // 第二个参数只支持单个类型。元组形式（`isinstance(x, (A, B))`）等有
  // 需求再加——现在明确报错，不静默返回 False。
  if (!isBuiltinClass(wanted) && !isClass(wanted)) {
    *error = "isinstance() arg 2 must be a type or tuple of types";
    return Value();
  }

  const std::string wantedName =
      isBuiltinClass(wanted) ? builtinClassName(wanted) : classNameOf(wanted);

  // 1. 脚本类实例：沿类层次往上找，多继承也能命中。
  if (isInstance(value)) {
    const Value cls = instanceClass(value);
    if (isClass(cls) && classHierarchyHas(cls, wantedName, 0)) {
      return Value(true);
    }
  }

  // 2. 异常对象：按异常类的层级判断（`isinstance(e, Exception)` 要成立）。
  if (isException(value)) {
    return Value(exceptionMatches(exceptionType(value), wantedName));
  }

  // 3. 其余按类型名比。这是 CPython 里 isinstance(1, int) 的等价语义。
  return Value(comparableTypeName(value) == wantedName);
}

}  // namespace builtins
}  // namespace aipy
