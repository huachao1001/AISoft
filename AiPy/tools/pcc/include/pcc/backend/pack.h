// 字节码二进制格式：写入与读回。
//
// ── 与 pack.py 的关系 ──
//
// 格式定义只有一份权威来源：`tools/aipyic/pack.py` 的头部注释。这里逐字段
// 照抄它的写出顺序，因为读取方（`src/bytecode.cpp` 与 Python 版反汇编器）
// 都是按那个顺序读的。字段顺序错了不会报错，只会读到垃圾——所以字段之间
// 一律不省略、不合并。
#pragma once

#include "pcc/codegen/codegen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pcc {

// 文件头：`AIPYBC01` + u16 版本。
constexpr char kBytecodeMagic[8] = {'A', 'I', 'P', 'Y', 'B', 'C', '0', '1'};
constexpr std::uint16_t kBytecodeVersion = 11;

// 常量类型标记，与 cpp 侧 ConstType 一致。
enum class ConstType : std::uint8_t {
  kNone = 0,
  kBool = 1,
  kInt = 2,
  kFloat = 3,
  kStr = 4,
  kBytes = 5,
};

// 把编译结果写成二进制。返回值与 pack.write_file 一样是字节数。
std::vector<std::uint8_t> packProgram(const Program& program);

// 写文件。失败返回 false（原因在 error 里）。
bool writeBytecode(const Program& program, const std::string& path,
                   std::string* error);

}  // namespace pcc
