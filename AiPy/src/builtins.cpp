// AiPy 的内建函数表。
//
// ── 为什么要一张表，而不是编译期直接调 C++ 函数 ──
//
// 编译成 C++ 的时候（另一条路线），`len(x)` 直接就是 `aipy::length(x)`，
// 零开销。但脚本是运行期才加载的字节码，编译器不知道运行期有没有这个
// 内建——所以只能带一张名字表，由 VM 按下标取函数指针。
//
// 代价是每次调用多一层间接跳转（实测单次约 94 ns，其中大部分是这个）。
// 收益是「改脚本不用重编译」，这对界面开发更值钱。
//
// ── 为什么内建与扩展分两张表 ──
//
// 内建属于语言本身，永远在；扩展是外部能力（文件、网络），只在 import 了
// 才链接。「用了才链接」这条要求在编译期就已经完成筛选，两张表只是让
// VM 的报错能分清「语言没有这个函数」和「扩展没链接进来」——这两种错误
// 的修法完全不同。
#include "aipy/builtins.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace aipy {
namespace builtins {

// ── 各内建 ──
//
// 签名统一为 (args, count, error)。带 error 而不是像扩展那样只返回值：
// 内建的参数错误（`len()` 少给参数）必须能报出「哪一行、哪个函数」，
// 静默返回 None 会让脚本在几十行之后才爆，那时已经看不出根因。

Value builtinLen(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "len 需要 1 个参数";
    return Value();
  }
  return length(args[0]);
}

Value builtinStr(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "str 需要 1 个参数";
    return Value();
  }
  return Value(args[0].toString());
}

Value builtinInt(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "int 需要 1 个参数";
    return Value();
  }
  if (args[0].type() == Value::Type::kString) {
    // 整串必须是整数：CPython 的 int("12.5") 是 ValueError，不是 12。
    const Value parsed = parseInteger(args[0]);
    if (parsed.isNone()) {
      raiseException("ValueError",
                     "invalid literal for int() with base 10: '" +
                         args[0].toString() + "'");
      return Value();
    }
    return parsed;
  }
  if (args[0].type() == Value::Type::kNone ||
      args[0].type() == Value::Type::kList ||
      args[0].type() == Value::Type::kDict) {
    raiseException("TypeError",
                   "int() argument must be a string, a bytes-like object or "
                   "a real number, not '" + typeName(args[0]) + "'");
    return Value();
  }
  return Value(args[0].toInt());
}

Value builtinFloat(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "float 需要 1 个参数";
    return Value();
  }
  if (args[0].type() == Value::Type::kString) {
    const Value parsed = parseFloat(args[0]);
    if (parsed.isNone()) {
      raiseException("ValueError",
                     "could not convert string to float: '" +
                         args[0].toString() + "'");
      return Value();
    }
    return parsed;
  }
  if (args[0].type() == Value::Type::kNone ||
      args[0].type() == Value::Type::kList ||
      args[0].type() == Value::Type::kDict) {
    raiseException("TypeError",
                   "float() argument must be a string or a real number, not '" +
                       typeName(args[0]) + "'");
    return Value();
  }
  return Value(args[0].toFloat());
}

Value builtinBool(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "bool 需要 1 个参数";
    return Value();
  }
  return Value(args[0].toBool());
}

Value builtinAbs(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "abs 需要 1 个参数";
    return Value();
  }
  return absolute(args[0]);
}

Value builtinRound(const Value* args, int count, std::string* error) {
  if (count == 1) return roundValue(args[0]);
  if (count == 2) return roundDigits(args[0], args[1]);
  *error = "round 需要 1 或 2 个参数";
  return Value();
}

Value builtinMin(const Value* args, int count, std::string* error) {
  if (count == 0) {
    *error = "min 至少需要 1 个参数";
    return Value();
  }
  // min([1,2,3]) 与 min(1, 2, 3) 都要支持：前者是脚本里的常见写法。
  if (count == 1 && args[0].isList()) {
    const Value::List& items = args[0].asList();
    if (items.empty()) {
      *error = "min() 的参数是空列表";
      return Value();
    }
    Value best = items[0];
    for (std::size_t i = 1; i < items.size(); ++i) {
      if (lt(items[i], best).toBool()) best = items[i];
    }
    return best;
  }
  Value best = args[0];
  for (int i = 1; i < count; ++i) {
    if (lt(args[i], best).toBool()) best = args[i];
  }
  return best;
}

Value builtinMax(const Value* args, int count, std::string* error) {
  if (count == 0) {
    *error = "max 至少需要 1 个参数";
    return Value();
  }
  if (count == 1 && args[0].isList()) {
    const Value::List& items = args[0].asList();
    if (items.empty()) {
      *error = "max() 的参数是空列表";
      return Value();
    }
    Value best = items[0];
    for (std::size_t i = 1; i < items.size(); ++i) {
      if (gt(items[i], best).toBool()) best = items[i];
    }
    return best;
  }
  Value best = args[0];
  for (int i = 1; i < count; ++i) {
    if (gt(args[i], best).toBool()) best = args[i];
  }
  return best;
}

Value builtinSum(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "sum 需要 1 个参数";
    return Value();
  }
  return sumOf(args[0]);
}

Value builtinList(const Value* args, int count, std::string* error) {
  if (count == 0) return Value::list({});
  if (count != 1) {
    *error = "list 需要 0 或 1 个参数";
    return Value();
  }
  // list("abc") → ['a','b','c']；list([1,2]) → 副本。
  return Value::list(iterate(args[0]));
}

// dict() 只支持无参形式。带参数的构造（`dict([["a", 1]])`）依赖可迭代的
// 键值对，而语言子集里没有元组，写不出合法的输入——与其支持一个只有
// 一种写法的形式，不如明确拒绝。
Value builtinDict(const Value*, int count, std::string* error) {
  if (count != 0) {
    *error = "dict 只支持无参形式（用 d[k] = v 填内容）";
    return Value();
  }
  return Value::dict({}, {});
}

// range 物化成列表。
//
// 为什么不在 VM 里做惰性迭代器：脚本里的 range 几乎都直接进 for，而 for
// 本来就把容器物化一次（见 kIterInit 的注释）。惰性迭代器只是把「一次
// 分配」换成「每次一步的状态机」，在这个规模上看不出差别，却多一个
// 需要维护的运行期对象类型。
Value builtinRange(const Value* args, int count, std::string* error) {
  if (count < 1 || count > 3) {
    *error = "range 需要 1-3 个参数";
    return Value();
  }
  long long start = 0;
  long long stop = 0;
  long long step = 1;
  if (count == 1) {
    stop = args[0].toInt();
  } else {
    start = args[0].toInt();
    stop = args[1].toInt();
    if (count == 3) {
      step = args[2].toInt();
      if (step == 0) {
        *error = "range 的步长不能是 0";
        return Value();
      }
    }
  }

  Value::List items;
  if (step > 0) {
    if (stop > start) {
      // 除以步长要向上取整，否则 range(0, 10, 3) 会少最后一段。
      items.reserve(static_cast<std::size_t>((stop - start + step - 1) / step));
    }
    for (long long value = start; value < stop; value += step) {
      items.push_back(Value(value));
    }
  } else {
    if (start > stop) {
      items.reserve(static_cast<std::size_t>((start - stop - step - 1) / (-step)));
    }
    for (long long value = start; value > stop; value += step) {
      items.push_back(Value(value));
    }
  }
  return Value::list(std::move(items));
}

// print：走 log sink。GUI 宿主把它接到 OutputDebugString，命令行宿主接
// stderr——语言层不该知道自己跑在哪儿。
Value builtinPrint(const Value* args, int count, std::string*) {
  std::string text;
  for (int i = 0; i < count; ++i) {
    if (i > 0) text.push_back(' ');
    text += args[i].toString();
  }
  log(Value(std::move(text)));
  return Value();
}

Value builtinFormatText(const Value* args, int count, std::string* error) {
  if (count != 2) {
    *error = "format_text 需要 2 个参数";
    return Value();
  }
  return formatText(args[0], args[1]);
}

Value builtinSet(const Value* args, int count, std::string* error) {
  if (count == 0) return Value::set(Value::List{});
  if (count != 1) {
    *error = "set 最多 1 个参数";
    return Value();
  }
  // set(iterable)：去重后建成集合。字符串按字符拆开（与 list() 一致）。
  Value::List items;
  for (const Value& item : iterate(args[0])) {
    bool seen = false;
    for (const Value& existing : items) {
      if (eq(existing, item).toBool()) {
        seen = true;
        break;
      }
    }
    if (!seen) items.push_back(item);
  }
  return Value::set(std::move(items));
}

Value builtinCharFromCode(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "char_from_code 需要 1 个参数";
    return Value();
  }
  return charFromCode(args[0]);
}

Value builtinTextOfChar(const Value* args, int count, std::string* error) {
  if (count != 1) {
    *error = "text_of_char 需要 1 个参数";
    return Value();
  }
  return textOfChar(args[0]);
}

// 放在单独一个 .cpp 里：它们要判脚本类的 MRO，与其余内建（纯值运算）
// 关注点不同。
Value builtinType(const Value* args, int count, std::string* error);
Value builtinIsInstance(const Value* args, int count, std::string* error);

// ── 名字表 ──
//
// 顺序必须与 Python 侧编译器（tools/pyuic/vm_compiler.py）的 BUILTINS 一致。
// 两边靠一个测试对齐（见 tests/test_vm.cpp 的 builtin_table_matches_python）。
//
// 用一张静态数组而不是哈希表：名字只在编译期查（编译器按名字定下标），
// 运行期只按下标取。数组让「下标 → 名字」也能反查，报错信息要用。

struct Entry {
  const char* name;
  BuiltinFn fn;
};

const Entry kBuiltinTable[] = {
    {"len", builtinLen},
    {"str", builtinStr},
    {"int", builtinInt},
    {"float", builtinFloat},
    {"bool", builtinBool},
    {"abs", builtinAbs},
    {"round", builtinRound},
    {"min", builtinMin},
    {"max", builtinMax},
    {"sum", builtinSum},
    {"list", builtinList},
    {"dict", builtinDict},
    {"range", builtinRange},
    {"print", builtinPrint},
    {"format_text", builtinFormatText},
    {"char_from_code", builtinCharFromCode},
    {"text_of_char", builtinTextOfChar},
    {"set", builtinSet},
    // `next` 只是登记：真正的推进在 VM 里（值层拿不到 Program）。
    {"next", nullptr},
    {"type", builtinType},
    {"isinstance", builtinIsInstance},
};

}  // namespace builtins

int builtinCount() {
  return static_cast<int>(sizeof(builtins::kBuiltinTable) / sizeof(builtins::kBuiltinTable[0]));
}

const char* builtinName(int index) {
  if (index < 0 || index >= builtinCount()) return nullptr;
  return builtins::kBuiltinTable[index].name;
}

BuiltinFn builtinAt(int index) {
  if (index < 0 || index >= builtinCount()) return nullptr;
  return builtins::kBuiltinTable[index].fn;
}

int builtinIndexFromName(const std::string& name) {
  for (int i = 0; i < builtinCount(); ++i) {
    if (name == builtins::kBuiltinTable[i].name) return i;
  }
  return -1;
}

}  // namespace aipy
