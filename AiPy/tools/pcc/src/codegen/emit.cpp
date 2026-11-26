// 代码生成的公共底座：发射、槽分配、错误、常量池与下标池。
//
// 这些被 statements/expressions/module 三个文件共用，所以单独一层。
#include "pcc/codegen/codegen.h"

#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

namespace pcc {

CodeGen::CodeGen(std::string filename, std::string moduleName)
    : filename_(std::move(filename)) {
  program_.moduleName = std::move(moduleName);
  module_.name = "<module>";
}

// ── 发射 ──

void CodeGen::emit(const char* op, const Node& node) {
  block_->lines.emplace_back(static_cast<std::uint32_t>(block_->code.size()),
                             node.line > 0 ? node.line : 0);
  const int number = Tables::opByName(op);
  block_->code.push_back(static_cast<std::uint8_t>(number < 0 ? 0 : number));
}

void CodeGen::emitOpNoLine(const char* op) {
  // 行号 0：模块末尾的隐式 return 不对应任何源码行（Python 版传 ast.Module，
  // 它没有 lineno）。
  block_->lines.emplace_back(static_cast<std::uint32_t>(block_->code.size()), 0);
  const int number = Tables::opByName(op);
  block_->code.push_back(static_cast<std::uint8_t>(number < 0 ? 0 : number));
}

void CodeGen::emitU8(int value) {
  block_->code.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void CodeGen::emitU16(int value) {
  block_->code.push_back(static_cast<std::uint8_t>(value & 0xFF));
  block_->code.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void CodeGen::patchU16(std::size_t at, int value) {
  if (at + 1 >= block_->code.size()) return;
  block_->code[at] = static_cast<std::uint8_t>(value & 0xFF);
  block_->code[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

int CodeGen::slotOfIn(Block* block, const std::string& name) {
  for (std::size_t i = 0; i < block->localNames.size(); ++i) {
    if (block->localNames[i] == name) return static_cast<int>(i);
  }
  const int slot = block->localCount;
  block->localNames.push_back(name);
  block->localCount = slot + 1;
  return slot;
}

int CodeGen::slotOf(const std::string& name) { return slotOfIn(block_, name); }

int CodeGen::allocSlot() {
  // 只增不减：退出时把槽还回去会让紧随的另一个 with 复用它，而外层的
  // `__exit__` 还指着那个槽——管理器被换掉，锁就不释放了。
  const int slot = block_->localCount;
  // 匿名槽用一个不可能与源码标识符碰撞的名字标记。
  block_->localNames.push_back("\x01anon");
  block_->localCount = slot + 1;
  return slot;
}

bool CodeGen::fail(const std::string& message, int line, int col) {
  if (failed_) return false;
  failed_ = true;
  error_.message = message;
  error_.line = line;
  error_.col = col;
  return false;
}

bool CodeGen::failAt(const std::string& message, const Node& node) {
  return fail(message, node.line, node.col + 1);
}

// ── 下标池 ──

int CodeGen::extensionIndex(const std::string& qualified) {
  for (std::size_t i = 0; i < extensions_.size(); ++i) {
    if (extensions_[i] == qualified) return static_cast<int>(i);
  }
  extensions_.push_back(qualified);
  return static_cast<int>(extensions_.size()) - 1;
}

int CodeGen::builtinIndex(const std::string& name) {
  for (std::size_t i = 0; i < usedBuiltins_.size(); ++i) {
    if (usedBuiltins_[i] == name) return static_cast<int>(i);
  }
  usedBuiltins_.push_back(name);
  return static_cast<int>(usedBuiltins_.size()) - 1;
}

int CodeGen::constIndexOfInt(long long value) {
  for (std::size_t i = 0; i < consts_.size(); ++i) {
    if (consts_[i].kind == ConstKind::Int && consts_[i].intValue == value) {
      return static_cast<int>(i);
    }
  }
  Constant constant;
  constant.kind = ConstKind::Int;
  constant.intValue = value;
  consts_.push_back(constant);
  return static_cast<int>(consts_.size()) - 1;
}

int CodeGen::constIndexOfFloat(double value) {
  for (std::size_t i = 0; i < consts_.size(); ++i) {
    if (consts_[i].kind == ConstKind::Float && consts_[i].floatValue == value) {
      return static_cast<int>(i);
    }
  }
  Constant constant;
  constant.kind = ConstKind::Float;
  constant.floatValue = value;
  consts_.push_back(constant);
  return static_cast<int>(consts_.size()) - 1;
}

int CodeGen::constIndexOfStr(const std::string& value) {
  for (std::size_t i = 0; i < consts_.size(); ++i) {
    if (consts_[i].kind == ConstKind::Str && consts_[i].strValue == value) {
      return static_cast<int>(i);
    }
  }
  Constant constant;
  constant.kind = ConstKind::Str;
  constant.strValue = value;
  consts_.push_back(constant);
  return static_cast<int>(consts_.size()) - 1;
}

int CodeGen::constIndexOfBytes(const std::string& value) {
  for (std::size_t i = 0; i < consts_.size(); ++i) {
    if (consts_[i].kind == ConstKind::Bytes && consts_[i].strValue == value) {
      return static_cast<int>(i);
    }
  }
  Constant constant;
  constant.kind = ConstKind::Bytes;
  constant.strValue = value;
  consts_.push_back(constant);
  return static_cast<int>(consts_.size()) - 1;
}

int CodeGen::globalSlot(const std::string& name) {
  for (std::size_t i = 0; i < globalOrder_.size(); ++i) {
    if (globalOrder_[i] == name) return static_cast<int>(i);
  }
  globalOrder_.push_back(name);
  globalsSet_.insert(name);
  return static_cast<int>(globalOrder_.size()) - 1;
}

int CodeGen::functionIndex(const std::string& name) const {
  // 被装饰的名字运行期装的是装饰器返回值，不能按名解析成函数号。
  if (decoratedNames_.count(name) != 0) return -1;
  return rawFunctionIndex(name);
}

int CodeGen::rawFunctionIndex(const std::string& name) const {
  for (std::size_t i = 0; i < functionNames_.size(); ++i) {
    if (functionNames_[i] == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace pcc
