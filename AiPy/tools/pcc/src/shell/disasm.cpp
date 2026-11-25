// 反汇编的实现。
//
// ── 操作数形状必须与 emit 一一对应 ──
//
// 漏写一条，后面的指令会整体错位，而反汇编本身不会报错，只会打出垃圾。
// 所以遇到未知指令或未知形状一律报错，绝不猜。
#include "pcc/shell/disasm.h"

#include "pcc/backend/fsutil.h"
#include "pcc/backend/pack.h"
#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

#include <cstdio>
#include <cstring>

namespace pcc {
namespace {

class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

  bool take(std::size_t count, const std::uint8_t** out) {
    if (pos_ + count > data_.size()) return false;
    *out = data_.data() + pos_;
    pos_ += count;
    return true;
  }

  bool u8(std::uint8_t* out) {
    const std::uint8_t* bytes = nullptr;
    if (!take(1, &bytes)) return false;
    *out = bytes[0];
    return true;
  }

  bool u16(std::uint16_t* out) {
    const std::uint8_t* bytes = nullptr;
    if (!take(2, &bytes)) return false;
    *out = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    return true;
  }

  bool u32(std::uint32_t* out) {
    const std::uint8_t* bytes = nullptr;
    if (!take(4, &bytes)) return false;
    *out = 0;
    for (int index = 0; index < 4; ++index) {
      *out |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return true;
  }

  bool i32(std::int32_t* out) {
    std::uint32_t bits = 0;
    if (!u32(&bits)) return false;
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool i64(std::int64_t* out) {
    const std::uint8_t* bytes = nullptr;
    if (!take(8, &bytes)) return false;
    std::uint64_t bits = 0;
    for (int index = 0; index < 8; ++index) {
      bits |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool f64(double* out) {
    std::int64_t bits = 0;
    if (!i64(&bits)) return false;
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool text(std::string* out) {
    std::uint32_t length = 0;
    if (!u32(&length)) return false;
    const std::uint8_t* bytes = nullptr;
    if (!take(length, &bytes)) return false;
    out->assign(reinterpret_cast<const char*>(bytes), length);
    return true;
  }

  bool atEnd() const { return pos_ >= data_.size(); }

 private:
  const std::vector<std::uint8_t>& data_;
  std::size_t pos_ = 0;
};

// 常量池里每一项都存成 repr 后的文本，反汇编只打印它。
std::string reprInt(std::int64_t value) { return std::to_string(value); }

std::string reprFloat(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  std::string text(buffer);
  // 与 Python repr 对齐：整数值的浮点要带 .0，否则 `3.0` 会打成 `3`。
  if (text.find_first_of(".eEni") == std::string::npos) text += ".0";
  return text;
}

// JSON 字符串转义：printInfo 要输出能直接 json.loads 的内容。
std::string jsonEscapeText(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          // 控制字符编成 \u00XX。
          static const char kHex[] = "0123456789abcdef";
          const unsigned code = static_cast<unsigned char>(ch);
          out += "\\u00";
          out.push_back(kHex[(code >> 4) & 0xF]);
          out.push_back(kHex[code & 0xF]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

std::string reprStr(const std::string& value) {  // Python 的 repr 用单引号并转义；这里只需要人看得懂。
  std::string text = "'";
  for (char ch : value) {
    switch (ch) {
      case '\n': text += "\\n"; break;
      case '\r': text += "\\r"; break;
      case '\t': text += "\\t"; break;
      case '\\': text += "\\\\"; break;
      case '\'': text += "\\'"; break;
      default: text.push_back(ch); break;
    }
  }
  text += "'";
  return text;
}

// 每条指令的操作数形状。空表示无操作数。
bool shapesOf(const std::string& name, std::vector<char>* shapes,
              std::string* error) {
  static const char* const kTwoU16[] = {"CALL", "CALL_BUILTIN", "CALL_METHOD",
                                        "CALL_EXT", nullptr};
  static const char* const kU16[] = {
      "CONST", "LOAD_LOCAL", "STORE_LOCAL", "LOAD_GLOBAL", "STORE_GLOBAL",
      "JUMP", "JUMP_IF_FALSE", "JUMP_IF_FALSE_KEEP", "JUMP_IF_TRUE_KEEP",
      "BUILD_LIST", "BUILD_DICT", "ITER_NEXT", "WITH_ENTER", "WITH_EXIT",
      nullptr};
  static const char* const kU8[] = {"BIN", "CMP", "UNARY", "INPLACE", "SLICE",
                                    nullptr};
  static const char* const kNone[] = {
      "NONE", "TRUE", "FALSE", "POP", "DUP", "RETURN", "GET_ITEM", "SET_ITEM",
      "ITER_INIT", "ITER_BREAK", nullptr};

  for (const char* const* it = kTwoU16; *it != nullptr; ++it) {
    if (name == *it) {
      *shapes = {'u', 'b'};  // u16 + u8
      return true;
    }
  }
  for (const char* const* it = kU16; *it != nullptr; ++it) {
    if (name == *it) {
      *shapes = {'u'};
      return true;
    }
  }
  for (const char* const* it = kU8; *it != nullptr; ++it) {
    if (name == *it) {
      *shapes = {'b'};
      return true;
    }
  }
  for (const char* const* it = kNone; *it != nullptr; ++it) {
    if (name == *it) {
      shapes->clear();
      return true;
    }
  }
  *error = "the disassembler has no operand shape for instruction " + name;
  return false;
}

int lineOf(const std::vector<std::pair<std::uint32_t, int>>& lines,
           std::uint32_t offset) {
  int line = 0;
  for (const auto& entry : lines) {
    if (entry.first > offset) break;
    line = entry.second;
  }
  return line;
}

bool decodeBlock(const std::vector<std::uint8_t>& code,
                 const LoadedProgram& program, std::string* error) {
  const char* const* opNames = Tables::opNames();
  const int opCount = Tables::opCount();

  std::size_t pos = 0;
  while (pos < code.size()) {
    const std::size_t offset = pos;
    const int index = code[pos++];
    if (index >= opCount) {
      *error = "opcode " + std::to_string(index) + " at offset " +
               std::to_string(offset) + " is out of range";
      return false;
    }
    const std::string name = opNames[index];
    std::vector<char> shapes;
    if (!shapesOf(name, &shapes, error)) return false;

    std::vector<int> operands;
    for (char shape : shapes) {
      if (pos >= code.size()) {
        *error = "bytecode truncated at offset " + std::to_string(pos);
        return false;
      }
      if (shape == 'b') {
        operands.push_back(code[pos++]);
        continue;
      }
      if (pos + 1 >= code.size()) {
        *error = "bytecode truncated at offset " + std::to_string(pos);
        return false;
      }
      operands.push_back(static_cast<int>(code[pos] | (code[pos + 1] << 8)));
      pos += 2;
    }
    (void)program;
  }
  return true;
}

}  // namespace

bool loadBytecodeFile(const std::string& path, LoadedProgram* program,
                      std::string* error) {
  std::vector<std::uint8_t> data;
  if (!readFileBytes(path, &data)) {
    *error = diag::fileNotFound(path);
    return false;
  }
  if (data.size() < 10 ||
      std::memcmp(data.data(), kBytecodeMagic, sizeof(kBytecodeMagic)) != 0) {
    *error = diag::notBytecode(path);
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(data[8] | (data[9] << 8));
  if (version != kBytecodeVersion) {
    *error = "bytecode version " + std::to_string(version) +
             " does not match the expected version " +
             std::to_string(kBytecodeVersion);
    return false;
  }

  std::vector<std::uint8_t> body(data.begin() + 10, data.end());
  Reader reader(body);

  std::uint16_t globalCount = 0, localCount = 0, constCount = 0;
  std::uint16_t functionCount = 0, builtinCount = 0, importCount = 0;
  std::uint16_t extensionCount = 0, globalNameCount = 0;
  if (!reader.u16(&globalCount) || !reader.u16(&localCount) ||
      !reader.u16(&constCount) || !reader.u16(&functionCount) ||
      !reader.u16(&builtinCount) || !reader.u16(&importCount) ||
      !reader.u16(&extensionCount) || !reader.u16(&globalNameCount)) {
    *error = "bytecode header is truncated";
    return false;
  }
  program->globalCount = globalCount;
  program->localCount = localCount;

  for (std::uint16_t i = 0; i < globalNameCount; ++i) {
    std::string name;
    if (!reader.text(&name)) {
      *error = "bytecode truncated in global names";
      return false;
    }
    program->globalNames.push_back(name);
  }

  for (std::uint16_t i = 0; i < constCount; ++i) {
    std::uint8_t kind = 0;
    if (!reader.u8(&kind)) {
      *error = "bytecode truncated in constants";
      return false;
    }
    switch (static_cast<ConstType>(kind)) {
      case ConstType::kNone:
        program->consts.push_back("None");
        break;
      case ConstType::kBool: {
        std::int64_t value = 0;
        if (!reader.i64(&value)) {
          *error = "bytecode truncated in a bool constant";
          return false;
        }
        program->consts.push_back(value != 0 ? "True" : "False");
        break;
      }
      case ConstType::kInt: {
        std::int64_t value = 0;
        if (!reader.i64(&value)) {
          *error = "bytecode truncated in an int constant";
          return false;
        }
        program->consts.push_back(reprInt(value));
        break;
      }
      case ConstType::kFloat: {
        double value = 0.0;
        if (!reader.f64(&value)) {
          *error = "bytecode truncated in a float constant";
          return false;
        }
        program->consts.push_back(reprFloat(value));
        break;
      }
      case ConstType::kStr: {
        std::string value;
        if (!reader.text(&value)) {
          *error = "bytecode truncated in a string constant";
          return false;
        }
        program->consts.push_back(reprStr(value));
        break;
      }
      default:
        *error = "unknown constant type " + std::to_string(kind);
        return false;
    }
  }

  for (std::uint16_t i = 0; i < functionCount; ++i) {
    std::uint16_t paramCount = 0, localSlots = 0, lineCount = 0, nameLen = 0;
    std::uint32_t codeLen = 0;
    if (!reader.u16(&paramCount) || !reader.u16(&localSlots) ||
        !reader.u32(&codeLen) || !reader.u16(&lineCount) ||
        !reader.u16(&nameLen)) {
      *error = "bytecode truncated in a function header";
      return false;
    }
    LoadedProgram::Block block;
    block.paramCount = paramCount;
    block.localCount = localSlots;

    const std::uint8_t* nameBytes = nullptr;
    if (!reader.take(nameLen, &nameBytes)) {
      *error = "bytecode truncated in a function name";
      return false;
    }
    block.name.assign(reinterpret_cast<const char*>(nameBytes), nameLen);

    const std::uint8_t* codeBytes = nullptr;
    if (!reader.take(codeLen, &codeBytes)) {
      *error = "bytecode truncated in function code";
      return false;
    }
    block.code.assign(codeBytes, codeBytes + codeLen);

    for (std::uint16_t entry = 0; entry < lineCount; ++entry) {
      std::uint32_t offset = 0;
      std::int32_t line = 0;
      if (!reader.u32(&offset) || !reader.i32(&line)) {
        *error = "bytecode truncated in function line table";
        return false;
      }
      block.lines.emplace_back(offset, line);
    }
    program->functions.push_back(std::move(block));
  }

  {
    std::uint32_t codeLen = 0;
    std::uint16_t lineCount = 0;
    if (!reader.u32(&codeLen) || !reader.u16(&lineCount)) {
      *error = "bytecode truncated in the module header";
      return false;
    }
    const std::uint8_t* codeBytes = nullptr;
    if (!reader.take(codeLen, &codeBytes)) {
      *error = "bytecode truncated in module code";
      return false;
    }
    program->moduleCode.assign(codeBytes, codeBytes + codeLen);
    for (std::uint16_t entry = 0; entry < lineCount; ++entry) {
      std::uint32_t offset = 0;
      std::int32_t line = 0;
      if (!reader.u32(&offset) || !reader.i32(&line)) {
        *error = "bytecode truncated in the module line table";
        return false;
      }
      program->moduleLines.emplace_back(offset, line);
    }
  }

  auto readNames = [&reader, error](std::uint16_t count,
                                    std::vector<std::string>* out) {
    for (std::uint16_t i = 0; i < count; ++i) {
      std::string name;
      if (!reader.text(&name)) {
        *error = "bytecode truncated in the name tables";
        return false;
      }
      out->push_back(name);
    }
    return true;
  };
  if (!readNames(builtinCount, &program->builtins)) return false;
  if (!readNames(importCount, &program->imports)) return false;
  if (!readNames(extensionCount, &program->extensions)) return false;

  // 顺带校验一遍：解码不了的产物不该被当成「能跑」。
  if (!decodeBlock(program->moduleCode, *program, error)) return false;
  for (const LoadedProgram::Block& block : program->functions) {
    if (!decodeBlock(block.code, *program, error)) return false;
  }
  return true;
}

void printInfo(const LoadedProgram& program) {
  // 纯 JSON：没有提示文字，测试可以 json.loads 直接读。
  std::printf("{");
  std::printf("\"globalCount\": %d, ", program.globalCount);
  std::printf("\"localCount\": %d, ", program.localCount);

  std::printf("\"globals\": [");
  for (std::size_t i = 0; i < program.globalNames.size(); ++i) {
    std::printf("%s\"%s\"", i == 0 ? "" : ", ",
                jsonEscapeText(program.globalNames[i]).c_str());
  }
  std::printf("], ");

  std::printf("\"extensions\": [");
  for (std::size_t i = 0; i < program.extensions.size(); ++i) {
    std::printf("%s\"%s\"", i == 0 ? "" : ", ",
                jsonEscapeText(program.extensions[i]).c_str());
  }
  std::printf("], ");

  std::printf("\"imports\": [");
  for (std::size_t i = 0; i < program.imports.size(); ++i) {
    std::printf("%s\"%s\"", i == 0 ? "" : ", ",
                jsonEscapeText(program.imports[i]).c_str());
  }
  std::printf("], ");

  std::printf("\"builtins\": [");
  for (std::size_t i = 0; i < program.builtins.size(); ++i) {
    std::printf("%s\"%s\"", i == 0 ? "" : ", ",
                jsonEscapeText(program.builtins[i]).c_str());
  }
  std::printf("], ");

  std::printf("\"functions\": [");
  for (std::size_t i = 0; i < program.functions.size(); ++i) {
    const LoadedProgram::Block& block = program.functions[i];
    std::printf("%s{\"name\": \"%s\", \"paramCount\": %d, \"localCount\": %d}",
                i == 0 ? "" : ", ",
                jsonEscapeText(block.name).c_str(), block.paramCount,
                block.localCount);
  }
  std::printf("], ");

  // 模块指令条数（字节数，不是指令数）：测试用它做粗回归。
  std::printf("\"moduleCodeSize\": %d, ",
              static_cast<int>(program.moduleCode.size()));
  std::printf("\"constCount\": %d", static_cast<int>(program.consts.size()));
  std::printf("}\n");
}

void disassemble(const LoadedProgram& program, const std::string& path) {
  std::printf("%s: %d global slots, %d constants, %d functions\n",
              path.c_str(), program.globalCount,
              static_cast<int>(program.consts.size()),
              static_cast<int>(program.functions.size()));
  if (!program.builtins.empty()) {
    std::printf("builtins: ");
    for (std::size_t i = 0; i < program.builtins.size(); ++i) {
      std::printf("%s%s", i == 0 ? "" : ", ", program.builtins[i].c_str());
    }
    std::printf("\n");
  }

  struct View {
    const char* name;
    const std::vector<std::uint8_t>* code;
    const std::vector<std::pair<std::uint32_t, int>>* lines;
    int params;
  };
  std::vector<View> views;
  views.push_back({"<module>", &program.moduleCode, &program.moduleLines, 0});
  for (const LoadedProgram::Block& block : program.functions) {
    views.push_back(
        {block.name.c_str(), &block.code, &block.lines, block.paramCount});
  }

  const char* const* opNames = Tables::opNames();
  for (const View& view : views) {
    std::printf("\n-- %s", view.name);
    if (std::string(view.name) != "<module>") {
      std::printf(" (%d parameters)", view.params);
    }
    std::printf("\n");

    const std::vector<std::uint8_t>& code = *view.code;
    std::size_t pos = 0;
    while (pos < code.size()) {
      const std::size_t offset = pos;
      const std::string name = opNames[code[pos++]];
      std::vector<char> shapes;
      std::string ignored;
      if (!shapesOf(name, &shapes, &ignored)) return;

      std::vector<int> operands;
      for (char shape : shapes) {
        if (shape == 'b') {
          operands.push_back(code[pos++]);
        } else {
          operands.push_back(static_cast<int>(code[pos] | (code[pos + 1] << 8)));
          pos += 2;
        }
      }

      // 把裸数字翻译成人能读的东西。
      std::string text;
      char buffer[128];
      if (name == "CONST" && operands[0] < static_cast<int>(program.consts.size())) {
        text = program.consts[static_cast<std::size_t>(operands[0])];
      } else if ((name == "BIN" || name == "INPLACE") && operands[0] < 12) {
        static const char* const kBinopNames[] = {
            "ADD", "SUB", "MUL", "DIV", "FLOOR_DIV", "MOD", "POW",
            "BIT_AND", "BIT_OR", "BIT_XOR", "SHL", "SHR"};
        text = kBinopNames[operands[0]];
      } else if (name == "CMP" && operands[0] < 8) {
        static const char* const kCmpopNames[] = {
            "EQ", "NE", "LT", "LE", "GT", "GE", "IN", "NOT_IN"};
        text = kCmpopNames[operands[0]];
      } else if (name == "UNARY" && operands[0] < 3) {
        static const char* const kUnopNames[] = {"NEG", "NOT", "INVERT"};
        text = kUnopNames[operands[0]];
      } else if (name == "CALL_METHOD" &&
                 operands[0] < static_cast<int>(program.consts.size())) {
        std::snprintf(buffer, sizeof(buffer), "%s, argc=%d",
                      program.consts[static_cast<std::size_t>(operands[0])].c_str(),
                      operands[1]);
        text = buffer;
      } else if (name == "CALL_EXT") {
        // 扩展号指向扩展表，不是常量池——两者都是 u16，很容易看错。
        if (operands[0] < static_cast<int>(program.extensions.size())) {
          std::snprintf(buffer, sizeof(buffer), "%s, argc=%d",
                        program.extensions[static_cast<std::size_t>(operands[0])]
                            .c_str(),
                        operands[1]);
        } else {
          std::snprintf(buffer, sizeof(buffer), "ext#%d, argc=%d", operands[0],
                        operands[1]);
        }
        text = buffer;
      } else if (name == "CALL_BUILTIN") {
        std::snprintf(buffer, sizeof(buffer), "builtin#%d, argc=%d",
                      operands[0], operands[1]);
        text = buffer;
      } else if (name == "JUMP" || name == "JUMP_IF_FALSE" ||
                 name == "JUMP_IF_FALSE_KEEP" || name == "JUMP_IF_TRUE_KEEP" ||
                 name == "ITER_NEXT") {
        std::snprintf(buffer, sizeof(buffer), "-> %d", operands[0]);
        text = buffer;
      } else if (name == "SLICE") {
        const int flags = operands[0];
        std::string parts;
        if (flags & 1) parts += "start";
        if (flags & 2) parts += parts.empty() ? "stop" : "/stop";
        if (flags & 4) parts += parts.empty() ? "step" : "/step";
        text = parts.empty() ? "all defaults" : parts;
      } else {
        for (std::size_t i = 0; i < operands.size(); ++i) {
          if (i > 0) text += ", ";
          text += std::to_string(operands[i]);
        }
      }

      std::printf("  %4d  L%-4d %-20s %s\n", static_cast<int>(offset),
                  lineOf(*view.lines, static_cast<std::uint32_t>(offset)),
                  name.c_str(), text.c_str());
    }
  }
}

}  // namespace pcc
