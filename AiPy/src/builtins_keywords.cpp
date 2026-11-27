// 内建的关键字参数名 → 位置映射。
//
// ── 为什么需要一张表 ──
//
// 内建的签名统一是 `(args, count, error)`，没有形参名字。而 CPython 里
// `print(sep="-")`、`sorted(reverse=True)`、`int("ff", base=16)` 都是
// 正常写法，脚本里很常见。
//
// 所以关键字在 VM 里先按名字换成位置：`sep` 是 print 的第 3 个位置实参
// （前两个是 *objects…），换好之后再走原来的 CALL_BUILTIN 路径。
//
// ── 为什么是最小表 ──
//
// 只收 CPython 真的接受关键字、且我们实现了的内建。不在表里的内建收到
// 关键字时明确报 TypeError，与 CPython 的 "takes no keyword arguments"
// 一致——静默忽略会变成「写错了也能跑」。
#include "aipy/builtins.h"

#include <cstddef>
#include <string>

namespace aipy {

namespace {

struct KeywordEntry {
  const char* builtin;   // 内建名
  const char* keyword;   // 关键字名
  int position;          // 换成第几个位置实参（0 起）
};

// `*objects` 是变长位置参数，所以 print 的 sep/end 位置随实参个数变，
// 单独在 VM 里处理；这里只收固定位置的。
const KeywordEntry kKeywordTable[] = {
    // int(x, base=10)
    {"int", "base", 1},
    // round(x, ndigits=None)
    {"round", "ndigits", 1},
    // sorted(iterable, *, key=None, reverse=False)
    // key / reverse 在 CPython 里都是仅关键字，所以位置是「第 1、第 2 个」。
    {"sorted", "key", 1},
    {"sorted", "reverse", 2},
    // enumerate(iterable, start=0)
    {"enumerate", "start", 1},
    // str(object='')
    {"str", "object", 0},
    // sum(iterable, start=0)。注意 sum 的实际执行在 VM 里（它要吃生成器），
    // 这张表只决定「这个内建接受哪些关键字」。
    {"sum", "start", 1},
};

}  // namespace

int builtinKeywordPosition(const std::string& builtin,
                           const std::string& keyword) {
  for (const KeywordEntry& entry : kKeywordTable) {
    if (builtin == entry.builtin && keyword == entry.keyword) {
      return entry.position;
    }
  }
  return -1;
}

bool builtinTakesKeywords(const std::string& builtin) {
  for (const KeywordEntry& entry : kKeywordTable) {
    if (builtin == entry.builtin) return true;
  }
  // print 的 sep/end 在 VM 里按变长位置处理；dict 的键名任意；
  // min/max 的 key= 是仅关键字。这三类不能靠「名字→位置」表表达。
  return builtin == "print" || builtin == "dict" || builtin == "min" ||
         builtin == "max";
}

// 仅关键字参数（sorted 的 key/reverse）不属于位置表，但必须被接受。
// 单独一张表而不是给 KeywordEntry 加个 negative position：位置表要参与
// 「换到第几个槽」的换算，混进 -1 会让那个循环多一个分支。
bool builtinAcceptsKeyword(const std::string& builtin,
                           const std::string& keyword) {
  if (builtin == "sorted") return keyword == "key" || keyword == "reverse";
  // min/max 的 key= 是仅关键字参数。
  if (builtin == "min" || builtin == "max") return keyword == "key";
  // dict(**kwargs)：键名任意，不能按名单校验。
  if (builtin == "dict") return true;
  return false;
}

}  // namespace aipy
