// 方法分派：字符串 / 列表 / 字典三个接收者类型上的方法。
//
// ── 为什么按接收者类型分三段，而不是一张 name → 函数指针的哈希表 ──
//
// 名字表只有三十来条，线性比较比建表更快、更省体积；更重要的是，同名方法
// 在三种类型上的签名不同（`pop` 列表上取位置、字典上取键），一张全局表
// 反而要在每个条目里再分一次类型。
//
// ── 为什么方法不在 Value 里 ──
//
// `Value` 要守 16 字节，放不下方法表；而且「哪些方法存在」是语言层的
// 决定，不是数据结构的事实——`add` 是 Value 的语义，`append` 是解释器的
// 可调用面。这两件事分开，加一个方法不会让 Value 变大。
#include "vm/internal.h"

namespace aipy {
namespace vm {
namespace detail {

Value callMethod(const Value& target, const std::string& name,
                 const Value* args, int argc, std::string* error) {
  // 参数个数不匹配统一在这里报，省得每个分支各写一遍。
  const auto need = [&](int wanted) {
    if (argc != wanted) {
      *error = "方法 " + name + " 需要 " + std::to_string(wanted) +
               " 个参数，实际给了 " + std::to_string(argc) + " 个";
      return false;
    }
    return true;
  };

  // ── 字符串 ──
  if (target.type() == Value::Type::kString) {
    if (name == "upper" && need(0)) return upper(target);
    if (name == "lower" && need(0)) return lower(target);
    if (name == "strip" && need(0)) return strip(target);
    if (name == "replace" && need(2)) return replace(target, args[0], args[1]);
    if (name == "split" && need(1)) return split(target, args[0]);
    if (name == "startswith" && need(1)) return startsWith(target, args[0]);
    if (name == "endswith" && need(1)) return endsWith(target, args[0]);
    if (name == "find" && need(1)) return findIn(target, args[0]);
    if (name == "count" && need(1)) return countIn(target, args[0]);
    // `join` 在 Python 里是分隔符调用的：sep.join(items)
    if (name == "join" && need(1)) return join(target, args[0]);
    if (name == "format") {
      // CPython 的 `str.format(*args)`：任意个位置实参。
      Value::List collected;
      collected.reserve(static_cast<std::size_t>(argc));
      for (int i = 0; i < argc; ++i) collected.push_back(args[i]);
      return formatText(target, Value::list(std::move(collected)));
    }
    // ── 下面这批与 CPython 的 str 方法同形，实现见 strings.cpp ──
    if (name == "splitlines" && need(0)) return splitlines(target);
    if (name == "lstrip" && need(0)) return lstrip(target);
    if (name == "rstrip" && need(0)) return rstrip(target);
    if (name == "title" && need(0)) return title(target);
    if (name == "zfill" && need(1)) return zfill(target, args[0]);
    if (name == "ljust" && need(1)) return ljust(target, args[0]);
    if (name == "rjust" && need(1)) return rjust(target, args[0]);
    if (name == "center" && need(1)) return center(target, args[0]);
    if (name == "isdigit" && need(0)) return isdigit(target);
    if (name == "isalpha" && need(0)) return isalpha(target);
    if (name == "isalnum" && need(0)) return isalnum(target);
    if (name == "isspace" && need(0)) return isSpaceText(target);
    if (name == "islower" && need(0)) return islower(target);
    if (name == "isupper" && need(0)) return isupper(target);
    if (name == "encode") {
      if (argc == 0) return encodeText(target, "utf-8");
      if (argc == 1) return encodeText(target, args[0].toString());
      *error = "encode 最多 1 个参数";
      return Value();
    }
    *error = "字符串没有方法 " + name;
    return Value();
  }

  // ── bytes ──
  if (target.isBytes()) {
    if (name == "decode") {
      if (argc == 0) return decodeBytes(target, "utf-8");
      if (argc == 1) return decodeBytes(target, args[0].toString());
      *error = "decode 最多 1 个参数";
      return Value();
    }
    *error = "bytes 没有方法 " + name;
    return Value();
  }

  // ── 列表 ──
  if (target.isList()) {
    if (name == "append" && need(1)) {
      append(target, args[0]);
      return Value();
    }
    if (name == "extend" && need(1)) {
      extend(target, args[0]);
      return Value();
    }
    if (name == "insert" && need(2)) {
      insertAt(target, args[0], args[1]);
      return Value();
    }
    if (name == "pop") {
      if (argc == 0) return popAt(target, Value(-1LL));
      if (argc == 1) return popAt(target, args[0]);
      *error = "pop 最多 1 个参数";
      return Value();
    }
    if (name == "clear" && need(0)) {
      clearContainer(target);
      return Value();
    }
    if (name == "sort" && need(0)) {
      sortList(target);
      return Value();
    }
    if (name == "reverse" && need(0)) {
      reverseList(target);
      return Value();
    }
    // ── 与 CPython 的 list 方法同形 ──
    if (name == "index" && need(1)) return listIndexOf(target, args[0]);
    if (name == "remove" && need(1)) {
      listRemove(target, args[0]);
      return Value();
    }
    if (name == "copy" && need(0)) return listCopy(target);
    if (name == "count" && need(1)) return listCount(target, args[0]);
    *error = "列表没有方法 " + name;
    return Value();
  }

  // ── 字典 ──
  if (target.isDict()) {
    if (name == "get") {
      if (argc == 1) return dictGetOr(target, args[0], Value());
      if (argc == 2) return dictGetOr(target, args[0], args[1]);
      *error = "get 需要 1 或 2 个参数";
      return Value();
    }
    if (name == "keys" && need(0)) return dictKeysOf(target);
    if (name == "values" && need(0)) return dictValuesOf(target);
    if (name == "items" && need(0)) return dictItemsOf(target);
    if (name == "update" && need(1)) {
      dictUpdate(target, args[0]);
      return Value();
    }
    if (name == "pop") {
      if (argc == 1) return dictPop(target, args[0], Value());
      if (argc == 2) return dictPop(target, args[0], args[1]);
      *error = "pop 需要 1 或 2 个参数";
      return Value();
    }
    // ── 与 CPython 的 dict 方法同形 ──
    if (name == "setdefault") {
      if (argc == 1) return dictSetDefault(target, args[0], Value());
      if (argc == 2) return dictSetDefault(target, args[0], args[1]);
      *error = "setdefault 需要 1 或 2 个参数";
      return Value();
    }
    if (name == "copy" && need(0)) return dictCopyOf(target);
    if (name == "clear" && need(0)) {
      dictClear(target);
      return Value();
    }
    *error = "字典没有方法 " + name;
    return Value();
  }

  // ── 带方法的宿主对象 ──
  //
  // 表由 pylib 提供，VM 只按名字转发。这里不认识锁、文件、网络——加一个
  // 新对象类型不需要改这个文件，只要 pylib 带自己的 MethodTable。
  //
  // 传**接收者 Value 本身**而不是 ObjectData*：`__enter__` 要返回自身，
  // 只有 Value 能安全地复制一份句柄（共享同一份 HeapData）。
  if (target.type() == Value::Type::kObject) {
    const ObjectData* data = target.asObject();
    if (data == nullptr || data->table == nullptr) {
      *error = "对象没有方法表（pylib bug）";
      return Value();
    }
    for (const MethodEntry* entry = data->table->methods;
         entry != nullptr && entry->name != nullptr; ++entry) {
      if (name != entry->name) continue;
      return entry->fn(target, args, argc);
    }
    *error = "这个对象没有方法 " + name;
    return Value();
  }

  // 其他类型本来就没有方法：对数字或 None 调方法一定是脚本写错了。
  // 报错里带上接收者的前 24 字节，是为了让「None.upper()」这类错误能
  // 直接看出实际拿到的是什么。
  *error = std::string("这个类型没有方法 ") + name + "（接收者是 " +
           target.toString().substr(0, 24) + "）";
  return Value();
}

}  // namespace detail
}  // namespace vm
}  // namespace aipy
