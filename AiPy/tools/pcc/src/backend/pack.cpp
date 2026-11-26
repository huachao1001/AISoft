// 字节码二进制的写出。
//
// ── 为什么不用 JSON ──
//
// 格式由我们独占，只在启动时读一次，没有任何互操作需求。JSON 要么拖一个
// 解析器进来（几百 KB），要么自己写一个正确实现（上千行，还要处理转义与
// 深度限制）。定长小端序两个方向都只要几十行。
//
// ── 全部小端、定长 ──
//
// 定长是有意的：加载器可以直接按偏移切片，不必逐字节累积状态机。代价是
// 长度字段固定 16 位——函数数、常量数、全局槽都远不可能超过 65535。
#include "pcc/backend/pack.h"

#include <cstdio>
#include <cstring>

namespace pcc {
namespace {

class Writer {
 public:
  void u8(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value & 0xFF));
  }

  void u16(std::uint32_t value) {
    data_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  }

  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void i32(std::int32_t value) {
    u32(static_cast<std::uint32_t>(value));
  }

  void i64(std::int64_t value) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (int shift = 0; shift < 64; shift += 8) {
      data_.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFF));
    }
  }

  void f64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double 必须是 8 字节");
    std::memcpy(&bits, &value, sizeof(bits));
    i64(static_cast<std::int64_t>(bits));
  }

  void raw(const std::uint8_t* bytes, std::size_t count) {
    data_.insert(data_.end(), bytes, bytes + count);
  }

  // u32 长度 + UTF-8 字节。length 用的是**字节数**，不是字符数。
  void text(const std::string& value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }

  std::vector<std::uint8_t> take() { return std::move(data_); }

 private:
  std::vector<std::uint8_t> data_;
};

void writeBlock(Writer* out, const std::vector<std::uint8_t>& code,
                const std::vector<std::pair<std::uint32_t, int>>& lines) {
  out->u32(static_cast<std::uint32_t>(code.size()));
  out->u16(static_cast<std::uint32_t>(lines.size()));
  out->raw(code.data(), code.size());
  for (const auto& entry : lines) {
    out->u32(entry.first);
    out->i32(entry.second);
  }
}

}  // namespace

std::vector<std::uint8_t> packProgram(const Program& program) {
  Writer out;
  out.raw(reinterpret_cast<const std::uint8_t*>(kBytecodeMagic),
          sizeof(kBytecodeMagic));
  out.u16(kBytecodeVersion);

  out.u16(static_cast<std::uint32_t>(program.globalCount));
  out.u16(static_cast<std::uint32_t>(program.moduleLocalCount));
  out.u16(static_cast<std::uint32_t>(program.consts.size()));
  out.u16(static_cast<std::uint32_t>(program.functions.size()));
  out.u16(static_cast<std::uint32_t>(program.builtins.size()));
  out.u16(static_cast<std::uint32_t>(program.imports.size()));
  out.u16(static_cast<std::uint32_t>(program.extensions.size()));

  // 全局变量名，按槽号排列。热加载要靠名字匹配灌状态：新程序里同一个变量
  // 的槽号可能整体移位，按槽号会把这个值灌进那个变量。
  out.u16(static_cast<std::uint32_t>(program.globals.size()));
  for (const std::string& name : program.globals) out.text(name);

  for (const Constant& constant : program.consts) {
    switch (constant.kind) {
      case ConstKind::Bool:
        out.u8(static_cast<std::uint16_t>(ConstType::kBool));
        out.i64(constant.intValue != 0 ? 1 : 0);
        break;
      case ConstKind::Int:
        out.u8(static_cast<std::uint16_t>(ConstType::kInt));
        out.i64(constant.intValue);
        break;
      case ConstKind::Float:
        out.u8(static_cast<std::uint16_t>(ConstType::kFloat));
        out.f64(constant.floatValue);
        break;
      case ConstKind::Str:
        out.u8(static_cast<std::uint16_t>(ConstType::kStr));
        out.text(constant.strValue);
        break;
      case ConstKind::Bytes:
        out.u8(static_cast<std::uint16_t>(ConstType::kBytes));
        out.text(constant.strValue);
        break;
      case ConstKind::None:
        // 编译期不会产出 None 常量（None 走 NONE 指令），但格式留了位置。
        out.u8(static_cast<std::uint16_t>(ConstType::kNone));
        break;
    }
  }

  for (const CompiledBlock& function : program.functions) {
    out.u16(static_cast<std::uint32_t>(function.paramCount));
    out.u16(static_cast<std::uint32_t>(function.localCount));
    out.u16(static_cast<std::uint32_t>(function.defaultCount));
    out.u16(function.defaultSlot < 0
                ? 0xFFFF
                : static_cast<std::uint32_t>(function.defaultSlot));
    out.u32(static_cast<std::uint32_t>(function.defaultMask & 0xFFFFFFFFu));
    out.u32(static_cast<std::uint32_t>(function.defaultMask >> 32));
    out.u16(function.varargSlot < 0
                ? 0xFFFF
                : static_cast<std::uint32_t>(function.varargSlot));
    out.u16(function.kwargSlot < 0
                ? 0xFFFF
                : static_cast<std::uint32_t>(function.kwargSlot));
    out.u16(function.kwonlyStart < 0
                ? 0xFFFF
                : static_cast<std::uint32_t>(function.kwonlyStart));
    out.u16(function.captureStart < 0
                ? 0xFFFF
                : static_cast<std::uint32_t>(function.captureStart));
    out.u16(static_cast<std::uint32_t>(function.paramNames.size()));
    for (const std::string& name : function.paramNames) out.text(name);
    out.u32(static_cast<std::uint32_t>(function.code.size()));
    out.u16(static_cast<std::uint32_t>(function.lines.size()));
    out.u16(static_cast<std::uint32_t>(function.name.size()));
    out.raw(reinterpret_cast<const std::uint8_t*>(function.name.data()),
            function.name.size());
    out.raw(function.code.data(), function.code.size());
    for (const auto& entry : function.lines) {
      out.u32(entry.first);
      out.i32(entry.second);
    }
  }

  writeBlock(&out, program.moduleCode, program.moduleLines);

  for (const std::string& name : program.builtins) out.text(name);
  for (const std::string& name : program.imports) out.text(name);
  for (const std::string& qualified : program.extensions) out.text(qualified);

  return out.take();
}

bool writeBytecode(const Program& program, const std::string& path,
                   std::string* error) {
  const std::vector<std::uint8_t> data = packProgram(program);
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  if (handle == nullptr) {
    *error = "cannot open " + path + " for writing";
    return false;
  }
  const std::size_t written =
      std::fwrite(data.data(), 1, data.size(), handle);
  const bool ok = written == data.size();
  std::fclose(handle);
  if (!ok) *error = "short write to " + path;
  return ok;
}

}  // namespace pcc
