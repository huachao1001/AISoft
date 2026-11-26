// 一批基础内建：容器变换与编码类。
//
// 与 builtins.cpp 分开：那些是数值/字符串/打印，这些是「把容器换个形状」
// 与「码点 ↔ 字符」，关注点不同，而且这批大多要 iterate()，放一起会让
// 主文件过长。
#include "aipy/builtins.h"

#include <string>

namespace aipy {
namespace builtins {

// ── sorted / reversed / zip / enumerate ──

Value builtinSorted(const Value* args, int count, std::string* error) {
  // 只有位置形式：`reverse=` / `key=` 是关键字参数，而 CALL_BUILTIN 只带
  // 位置实参。CPython 里 sorted(x, True) 本身就是 TypeError（reverse 是
  // 仅关键字），所以这里也不能接受两个位置参数。
  if (count != 1) {
    // CPython 的措辞是 "sorted expected 1 argument, got 2"。
    *error = "sorted expected 1 argument, got " + std::to_string(count);
    return Value();
  }
  return sortedCopy(args[0], Value(), false);
}

Value builtinReversed(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "reversed 需要 1 个参数";
    return Value();
  }
  return reversedCopy(args[0]);
}

Value builtinZip(const Value* args, int count, std::string* error) {
  if (count != 2) {
    *error = "zip 需要 2 个参数";
    return Value();
  }
  return zipOf(args[0], args[1]);
}

Value builtinEnumerate(const Value* args, int count, std::string* error) {
  if (count < 1 || count > 2) {
    *error = "enumerate 需要 1 或 2 个参数";
    return Value();
  }
  return enumerateOf(args[0], count == 2 ? args[1] : Value());
}

// ── any / all ──

Value builtinAny(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "any 需要 1 个参数";
    return Value();
  }
  return anyOf(args[0]);
}

Value builtinAll(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "all 需要 1 个参数";
    return Value();
  }
  return allOf(args[0]);
}

// ── chr / ord / hex / bin / oct ──

Value builtinChr(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "chr 需要 1 个参数";
    return Value();
  }
  return chrOf(args[0]);
}

Value builtinOrd(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "ord 需要 1 个参数";
    return Value();
  }
  return ordOf(args[0]);
}

Value builtinHex(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "hex 需要 1 个参数";
    return Value();
  }
  return intToBase(args[0], 16);
}

Value builtinBin(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "bin 需要 1 个参数";
    return Value();
  }
  return intToBase(args[0], 2);
}

Value builtinOct(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "oct 需要 1 个参数";
    return Value();
  }
  return intToBase(args[0], 8);
}

// ── repr / divmod / pow / tuple ──

Value builtinRepr(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "repr 需要 1 个参数";
    return Value();
  }
  return Value(reprOf(args[0]));
}

Value builtinDivmod(const Value* args, int count, std::string* error) {
  if (count != 2) {
    *error = "divmod 需要 2 个参数";
    return Value();
  }
  const Value quotient = floorDiv(args[0], args[1]);
  if (hasPendingException()) return Value();
  const Value remainder = mod(args[0], args[1]);
  if (hasPendingException()) return Value();
  return Value::tuple({quotient, remainder});
}

Value builtinPow(const Value* args, int count, std::string* error) {
  if (count != 2) {
    *error = "pow 需要 2 个参数（三参数取模形式未支持）";
    return Value();
  }
  return power(args[0], args[1]);
}

Value builtinTuple(const Value* args, int count, std::string* error) {
  if (count > 1) {
    *error = "tuple 需要 0 或 1 个参数";
    return Value();
  }
  if (count == 0) return Value::tuple({});
  if (args[0].isTuple()) return args[0];
  return Value::tuple(iterate(args[0]));
}

}  // namespace builtins
}  // namespace aipy
