// 类的语义：MRO、属性查找、实例化、方法绑定。
//
// 为什么在 VM 里而不是 Value 里：这些是 Python 的对象模型——多继承的
// 查找顺序、字段与类成员的优先级、方法绑定的第一个实参——都是语言语义。
// 按铁律一，VM 不认识具体能力，但「类」正是语法本身。
#include "vm/internal.h"

#include <string>

namespace aipy {
namespace vm {
namespace detail {

namespace {

// 深度优先、左到右、去重。CPython 用 C3 线性化；脚本里的类层次几乎都是
// 单继承或简单的菱形，C3 与这个顺序在那些形状上一致。真正需要「不同的
// 线性化才有区别」的层次（基类顺序冲突）会被 CPython 拒绝，这里不追求
// 复现那条诊断——但查找顺序本身要与 CPython 一样，否则同名方法取错。
void collectMro(const Value& cls, List* out) {
  if (!isClass(cls)) return;
  // 去重按类名判：没有公开的「同一个类对象」判据，而脚本里的类名在
  // 模块内唯一（重复定义会被编译器拒绝）。
  for (const Value& existing : *out) {
    if (classNameOf(existing) == classNameOf(cls)) return;
  }
  out->push_back(cls);
  for (const Value& base : classBases(cls)) collectMro(base, out);
}

List mroOf(const Value& cls) {
  List out;
  collectMro(cls, &out);
  return out;
}

// 在类对象的成员表里按名字找。返回下标，找不到 -1。
long long memberIndex(const Value& cls, const std::string& name) {
  const List& keys = classMemberKeys(cls);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].asString() == name) return static_cast<long long>(i);
  }
  return -1;
}

}  // namespace

Value classLookupRaw(const Value& cls, const std::string& name) {
  for (const Value& current : mroOf(cls)) {
    const long long index = memberIndex(current, name);
    if (index >= 0) return classMemberValues(current)[static_cast<std::size_t>(index)];
  }
  return Value();
}

bool classLookup(const Value& cls, const std::string& name, Value* out) {
  for (const Value& current : mroOf(cls)) {
    const long long index = memberIndex(current, name);
    if (index >= 0) {
      if (out != nullptr) {
        *out = classMemberValues(current)[static_cast<std::size_t>(index)];
      }
      return true;
    }
  }
  return false;
}

// `super().m` 的查找：跳过「当前类」本身，从它的基类开始。
// 遍历顺序是 MRO 里当前类之后的那些，与 CPython 的 super() 一致。
Value superLookupRaw(const Value& self, const Value& currentClass,
                     const std::string& name) {
  for (const Value& current : mroOf(currentClass)) {
    if (classNameOf(current) == classNameOf(currentClass)) continue;  // 跳过当前类
    const long long index = memberIndex(current, name);
    if (index >= 0) {
      return classMemberValues(current)[static_cast<std::size_t>(index)];
    }
  }
  (void)self;
  return Value();
}

bool superLookupValue(const Value& self, const Value& currentClass,
                      const std::string& name, Value* out) {
  for (const Value& current : mroOf(currentClass)) {
    if (classNameOf(current) == classNameOf(currentClass)) continue;
    const long long index = memberIndex(current, name);
    if (index >= 0) {
      if (out != nullptr) {
        *out = classMemberValues(current)[static_cast<std::size_t>(index)];
      }
      return true;
    }
  }
  (void)self;
  return false;
}

Value classAttrOf(const Value& cls, const std::string& name) {
  return classLookupRaw(cls, name);
}

Value bindMethod(const Value& function, const Value& self) {
  // 方法值 = 函数号 + self。调用时 VM 把 self 补成第一个位置实参，
  // 不借用捕获列表（捕获是闭包的外层变量，两者语义不同）。
  return makeBoundMethod(functionIndexOf(function), self);
}


// 把类成员按描述符语义变成「访问到的值」。
//
// receiver 是访问的起点：实例（`obj.m`）或类（`A.m`）。asInstance 说明
// 这次访问是否经由实例——只有通过实例访问才绑 self。
//
// @staticmethod 与 @classmethod 的差别正在这里：
//   * staticmethod 丢掉接收者，返回裸函数；
//   * classmethod 绑定的永远是**接收者的类**，而不是定义它的那个类。
//     子类继承后 `Leaf.cm()` 里的 cls 是 Leaf，`super().cm()` 也是 Leaf，
//     与 CPython 一致。
Value descriptorValue(const Value& member, const Value& receiver,
                      bool asInstance) {
  if (isStaticMethod(member)) return staticMethodFunction(member);
  if (isClassMethod(member)) {
    const Value owner = isClass(receiver) ? receiver : instanceClass(receiver);
    return bindMethod(classMethodFunction(member), owner);
  }
  if (isFunction(member)) {
    return asInstance ? bindMethod(member, receiver) : member;
  }
  return member;
}

Value instanceAttrOf(const Value& instance, const std::string& name,
                     std::string* error) {
  if (!isInstance(instance)) {
    if (error != nullptr) *error = "not an instance";
    return Value();
  }

  // 1. 自己的字段优先。CPython 里实例属性遮蔽类属性。
  {
    const List& keys = instanceKeys(instance);
    const List& values = instanceValues(instance);
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i].asString() == name) return values[i];
    }
  }

  // 2. 沿 MRO 找类成员。用 memberIndex 判断「找到没有」而不是看返回值
  // 是不是 None——成员本身就可以是 None。
  const Value cls = instanceClass(instance);
  for (const Value& current : mroOf(cls)) {
    const long long index = memberIndex(current, name);
    if (index < 0) continue;
    const Value member = classMemberValues(current)[static_cast<std::size_t>(index)];
    // 3. 按描述符语义绑定：普通函数绑 self，staticmethod 不绑，
    //    classmethod 绑实例的类。
    return descriptorValue(member, instance, true);
  }

  if (error != nullptr) {
    *error = "'" + classNameOf(cls) + "' object has no attribute '" + name + "'";
  }
  return Value();
}

void instanceSetAttr(const Value& instance, const std::string& name,
                     const Value& value) {
  if (!isInstance(instance)) return;
  List& keys = mutableInstanceKeys(instance);
  List& values = mutableInstanceValues(instance);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].asString() == name) {
      values[i] = value;
      return;
    }
  }
  keys.push_back(Value(name));
  values.push_back(value);
}

}  // namespace detail
}  // namespace vm
}  // namespace aipy
