// 读回 `.bc` 并渲染成「偏移: 指令 操作数」。
//
// ── 为什么反汇编属于编译器 ──
//
// 调试编译器时最常问的是「这一行源代码生成了哪几条指令」。C++ 侧
// `vm::disassemble` 也能打，但它要先编一遍、再传文件，改一次编译器就得
// 重建。这里直接读 `.bc`、直接打，反馈是亚秒级的。
//
// 两份反汇编器（这里的与 `src/vm/disasm.cpp`）不算重复：它们读同一个文件、
// 各自解码，**对不上就说明加载器或格式有 bug**——免费的一致性证据。
#pragma once

#include "pcc/codegen/codegen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pcc {

// 读回来的程序，字段与 pack 的写出顺序对应。
struct LoadedProgram {
  int globalCount = 0;
  int localCount = 0;
  std::vector<std::string> globalNames;
  std::vector<std::string> consts;   // repr 后的文本，只给反汇编看
  struct Block {
    std::string name;
    int paramCount = 0;
    int localCount = 0;
    std::vector<std::uint8_t> code;
    std::vector<std::pair<std::uint32_t, int>> lines;
  };
  std::vector<Block> functions;
  std::vector<std::uint8_t> moduleCode;
  std::vector<std::pair<std::uint32_t, int>> moduleLines;
  std::vector<std::string> builtins;
  std::vector<std::string> imports;
  std::vector<std::string> extensions;
};

bool loadBytecodeFile(const std::string& path, LoadedProgram* program,
                      std::string* error);

// 打印反汇编。
void disassemble(const LoadedProgram& program, const std::string& path);

// 打印程序的结构信息（JSON），供测试与工具读取。
//
// 为什么不是「让测试自己解析 .bc」：解析是加载器的职责，散到每个测试里
// 就会有三份各自实现的解码器，而它们对不上时没人知道该信哪份。
void printInfo(const LoadedProgram& program);

}  // namespace pcc
