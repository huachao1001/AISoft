// id() 与 hash()。
//
// 两者看起来同源（都是「一个数」），语义却完全不同：
//   * id 是对象的**身份**，同一个对象在生命周期内恒等；
//   * hash 是**相等性**的投影，a == b 必须 hash(a) == hash(b)。
//
// 所以 id 直接取载荷地址，而 hash 必须按值算——否则 `{1: 'a'}[1]` 会失败
// （两次算出来的整数是不同的堆载荷，地址不同）。
#include "aipy/value.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace aipy {
namespace builtins {

namespace {

// 与 eq 的规则保持一致：数值按数值、字符串按内容、其余按下标顺序混入。
std::uint64_t hashValue(const Value& value) {
  switch (value.type()) {
    case Value::Type::kNone:
      return 0x9e3779b97f4a7c15ULL;  // 任意固定值：None 只有一个
    case Value::Type::kBool:
      return value.toBool() ? 1 : 0;
    case Value::Type::kInt:
      return static_cast<std::uint64_t>(value.toInt());
    case Value::Type::kFloat: {
      // CPython 里 1 == 1.0 且 hash(1) == hash(1.0)，所以整数值得折成整数
      // 哈希，否则 {1: 'a'}[1.0] 查不到。
      const double number = value.toFloat();
      const double truncated = static_cast<double>(static_cast<long long>(number));
      if (number == truncated) return hashValue(Value(static_cast<long long>(number)));
      std::uint64_t bits = 0;
      static_assert(sizeof(bits) == sizeof(number), "double 应为 8 字节");
      std::memcpy(&bits, &number, sizeof(bits));
      return bits;
    }
    case Value::Type::kString:
    case Value::Type::kBytes: {
      const std::string& text = value.asString();
      // FNV-1a：够用且不依赖平台，脚本只看「相等值哈希相同」。
      std::uint64_t hash = 1469598103934665603ULL;
      for (const char ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
      }
      return hash;
    }
    case Value::Type::kTuple: {
      std::uint64_t hash = 0x84222325cbf29ce4ULL;
      for (const Value& item : value.asList()) {
        hash = hash * 31 + hashValue(item);
      }
      return hash;
    }
    default:
      // 列表 / 字典 / 集合在 CPython 里不可哈希（unhashable type）。返回
      // 身份哈希是不对的，但也比崩溃好——真正的判据在 builtinHash 里。
      return reinterpret_cast<std::uintptr_t>(value.identity());
  }
}

bool isHashable(const Value& value) {
  switch (value.type()) {
    case Value::Type::kList:
    case Value::Type::kDict:
    case Value::Type::kSet:
      return false;
    default:
      return true;
  }
}

}  // namespace

Value builtinId(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "id expected 1 argument, got " + std::to_string(count);
    return Value();
  }
  // 小整数与 None/True/False 没有独立载荷（Value 内联存），所以给一个
  // 稳定但无意义的值。CPython 的 id 本来也不保证跨进程稳定。
  const void* identity = args[0].identity();
  if (identity == nullptr) {
    return Value(static_cast<long long>(
        std::hash<long long>{}(args[0].type() == Value::Type::kInt
                                   ? args[0].toInt()
                                   : static_cast<long long>(args[0].type())) |
        1));
  }
  return Value(static_cast<long long>(
      reinterpret_cast<std::uintptr_t>(identity)));
}

Value builtinHash(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "hash expected 1 argument, got " + std::to_string(count);
    return Value();
  }
  if (!isHashable(args[0])) {
    raiseException("TypeError", "unhashable type: '" + typeName(args[0]) + "'");
    return Value();
  }
  return Value(static_cast<long long>(hashValue(args[0])));
}

}  // namespace builtins
}  // namespace aipy
