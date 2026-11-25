// 字节码表：操作码、内建名、算子编号。
//
// ── 为什么用名字而不是数字 ──
//
// 编号必须与 `AiPy/include/aipy/vm.h` 的 enum 完全一致。数字写死时，
// vm.h 里插一条新指令就会让所有指令集体错位——症状是「程序算出莫名其妙
// 的结果」而不是崩溃，最难查。用名字查表，顺序变了立刻能对拍出来
// （tests/build_bytecode.py 解析 vm.h 的枚举逐条比对）。
#pragma once

#include "pcc/ast.h"

#include <string>

namespace pcc {

// 操作码 / 内建 / 算子的静态表。实现在 tables.cpp。
struct Tables {
  // 操作码名字表。顺序即编号。
  static const char* const* opNames();
  static int opCount();
  static int opByName(const char* name);

  static const char* const* builtinNames();
  static int builtinCount();
  static bool isBuiltin(const std::string& name);

  // 算子编号。返回 -1 表示这个算子不在子集里。
  static int binopNumber(Kind op);
  static int cmpopNumber(Kind op);
  static int unopNumber(Kind op);

  // 报错文案里用的算子名（与 Python 的 ast.op 类名一致）。
  static const char* binopSpelling(Kind op);
  static const char* cmpopSpelling(Kind op);
  static const char* unopSpelling(Kind op);
};

}  // namespace pcc
