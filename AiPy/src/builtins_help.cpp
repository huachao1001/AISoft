// help() 与 dir()。
//
// ── 为什么这两个能给「最小实现」──
//
// CPython 的 help 是一个完整的文档系统，dir 会列出对象的所有属性。那套
// 机制依赖 `__doc__`、`__dict__`、描述符遍历——本语言没有这些。但脚本里
// 用这两个的真实意图很朴素：**看某个内建有哪些方法可用**。所以这里给的
// 是「能回答那个问题」的最小版本，并在输出里明确说明范围，不假装完整。
#include "aipy/builtins.h"
#include "aipy/value.h"

#include <algorithm>
#include <string>

namespace aipy {
namespace builtins {

namespace {

// 各类型上已实现的方法名，顺序即输出顺序。这张表要与 methods.cpp 的分派
// 保持一致：改动其中一边时，aipy_activity 里的 dir 用例会提醒另一边。
const char* const kStringMethods[] = {
    "upper", "lower", "strip", "lstrip", "rstrip", "replace", "split",
    "splitlines", "startswith", "endswith", "find", "count", "join", "format",
    "title", "zfill", "ljust", "rjust", "center", "isdigit", "isalpha",
    "isalnum", "isspace", "islower", "isupper", nullptr};

const char* const kListMethods[] = {
    "append", "extend", "insert", "pop", "clear", "sort", "reverse",
    "index", "remove", "copy", "count", nullptr};

const char* const kDictMethods[] = {
    "get", "keys", "values", "items", "update", "pop", "setdefault", "copy",
    "clear", nullptr};

void appendNames(List* out, const char* const* names) {
  for (const char* const* cursor = names; *cursor != nullptr; ++cursor) {
    out->push_back(Value(std::string(*cursor)));
  }
}

}  // namespace

// help()：无参列出可用内建；给定一个值就说明它是什么类型、有哪些方法。
Value builtinHelp(const Value* args, int count, std::string* error) {
  std::string out;
  if (count == 0) {
    out = "Available builtins: ";
    for (int i = 0; i < builtinCount(); ++i) {
      if (i > 0) out += ", ";
      out += builtinName(i);
    }
    out += "\n";
    log(out);
    return Value();
  }
  if (count != 1) {
    *error = "help expected at most 1 argument, got " + std::to_string(count);
    return Value();
  }
  const std::string kind = typeName(args[0]);
  out = kind;
  if (args[0].isList()) {
    out += " methods: append, extend, insert, pop, clear, sort, reverse, "
           "index, remove, copy, count";
  } else if (args[0].isDict()) {
    out += " methods: get, keys, values, items, update, pop, setdefault, "
           "copy, clear";
  } else if (args[0].type() == Value::Type::kString) {
    out += " methods: upper, lower, strip, lstrip, rstrip, replace, split, "
           "splitlines, startswith, endswith, find, count, join, format, "
           "title, zfill, ljust, rjust, center, isdigit, isalpha, isalnum, "
           "isspace, islower, isupper";
  } else {
    out += " has no methods";
  }
  out += "\n";
  log(out);
  return Value();
}

// dir(value)：返回该值可用方法的**列表**（CPython 返回排好序的列表）。
// 无参时给出模块级名字——本语言没有真正的模块命名空间，所以返回空列表，
// 而不是编一份可能过时的清单。
Value builtinDir(const Value* args, int count, std::string* error) {
  if (count == 0) return Value::list({});
  if (count != 1) {
    *error = "dir expected at most 1 argument, got " + std::to_string(count);
    return Value();
  }
  List names;
  if (args[0].isList()) {
    appendNames(&names, kListMethods);
  } else if (args[0].isDict()) {
    appendNames(&names, kDictMethods);
  } else if (args[0].type() == Value::Type::kString) {
    appendNames(&names, kStringMethods);
  }
  // CPython 的 dir 返回排好序的列表；脚本常把它直接打印出来看。
  std::sort(names.begin(), names.end(),
            [](const Value& a, const Value& b) {
              return a.asString() < b.asString();
            });
  return Value::list(std::move(names));
}

}  // namespace builtins
}  // namespace aipy
