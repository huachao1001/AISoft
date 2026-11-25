// 语句的代码生成，逐段对照 compiler.py 的 statement/with/if/assign/for。
//
// ── 发射顺序就是字节码本身 ──
//
// 这个文件里几乎没有「算法」，全是「先发哪条、后发哪条」。改顺序不会报错，
// 只会让产物与 Python 版不一致——所以每段的注释都写清它对应 Python 版的
// 哪几行，方便对照，而不是解释代码在做什么。
#include "pcc/codegen/codegen.h"

#include "pcc/diagnostics.h"

namespace pcc {

bool CodeGen::statement(const Node& node) {
  switch (node.kind) {
    case Kind::Expr: {
      const Node& value = *node.children[0];
      if (value.kind == Kind::Yield || value.kind == Kind::YieldFrom) {
        return failAt(diag::yieldUnsupported(), value);
      }
      if (value.kind == Kind::Await) {
        return failAt(diag::awaitUnsupported(), value);
      }
      // 只有调用语句会产生副作用；`1 + 2` 这样的表达式语句整条丢弃。
      if (value.kind == Kind::Call) {
        if (!expr(value)) return false;
        emit("POP", node);
      }
      return true;
    }

    case Kind::Pass:
      return true;

    case Kind::Assign: {
      // 链式赋值 a = b = 1：值只求一次，再逐个存。
      const std::size_t valueIndex = node.children.size() - 1;
      if (!expr(*node.children[valueIndex])) return false;
      for (std::size_t i = 0; i < valueIndex; ++i) {
        if (i + 1 < valueIndex) emit("DUP", node);
        if (!assignTarget(*node.children[i], node)) return false;
      }
      return true;
    }

    case Kind::AnnAssign:
      // 纯注解 `x: int` 不产生代码。
      if (node.children.size() < 3) return true;
      if (!expr(*node.children[2])) return false;
      return assignTarget(*node.children[0], node);

    case Kind::AugAssign:
      return augmented(node);

    case Kind::Return: {
      if (node.children.empty()) {
        emit("NONE", node);
      } else {
        if (!expr(*node.children[0])) return false;
      }
      // 返回值已在栈顶，但 __exit__ 会压/弹一层——先挪到临时槽，退出所有
      // with，再取回来。否则 WITH_EXIT 弹掉的正是返回值。
      if (!withStack_.empty()) {
        const int temp = allocSlot();
        emit("STORE_LOCAL", node);
        emitU16(temp);
        emitWithExits(node);
        emit("LOAD_LOCAL", node);
        emitU16(temp);
      }
      emit("RETURN", node);
      return true;
    }

    case Kind::If:
      return ifChain(node);

    case Kind::While: {
      if (node.orelseCount > 0) return failAt(diag::whileElse(), node);
      const int loopStart = static_cast<int>(block_->code.size());
      if (!expr(*node.children[0])) return false;
      emit("JUMP_IF_FALSE", node);
      const int exitFixup = static_cast<int>(block_->code.size());
      emitU16(0);
      loopStack_.push_back(LoopFrame{loopStart, {}, false});
      for (std::size_t i = 0; i < node.bodyCount; ++i) {
        if (!statement(*node.children[1 + i])) return false;
      }
      const LoopFrame frame = loopStack_.back();
      loopStack_.pop_back();
      emit("JUMP", node);
      emitU16(loopStart);
      const int exitAt = static_cast<int>(block_->code.size());
      patchU16(static_cast<std::size_t>(exitFixup), exitAt);
      for (int fixup : frame.breakFixups) {
        patchU16(static_cast<std::size_t>(fixup), exitAt);
      }
      return true;
    }

    case Kind::For:
      return forLoop(node);

    case Kind::Break: {
      if (loopStack_.empty()) return failAt(diag::breakOutsideLoop(), node);
      const bool isFor = loopStack_.back().isFor;
      // 先退出 with（由内向外），再弹循环状态。
      emitWithExits(node);
      if (isFor) emit("ITER_BREAK", node);
      emit("JUMP", node);
      const int fixup = static_cast<int>(block_->code.size());
      emitU16(0);
      loopStack_.back().breakFixups.push_back(fixup);
      return true;
    }

    case Kind::Continue: {
      if (loopStack_.empty()) return failAt(diag::continueOutsideLoop(), node);
      const int target = loopStack_.back().continueTarget;
      emitWithExits(node);
      emit("JUMP", node);
      emitU16(target);
      return true;
    }

    case Kind::Global:
      return true;  // 槽在模块级已经分配好

    case Kind::Nonlocal:
      return failAt(diag::nonlocalUnsupported(), node);

    case Kind::Assert:
      return failAt(diag::assertUnsupported(), node);

    case Kind::Raise:
      return failAt(diag::raiseUnsupported(), node);

    case Kind::With:
      return withStatement(node);

    case Kind::Try:
      return failAt(diag::tryUnsupported(), node);

    case Kind::FunctionDef:
      return failAt(diag::nestedFunction(node.text), node);

    case Kind::Delete:
      return failAt(diag::deleteUnsupported(), node);

    case Kind::Import:
    case Kind::ImportFrom:
      return importStatement(node);

    default:
      break;
  }
  return failAt(diag::unsupportedStatement(kind_name(node.kind)), node);
}

bool CodeGen::withStatement(const Node& node) {
  if (node.argsCount != 1) return failAt(diag::withMultipleItems(), node);

  const Node& item = *node.children[0];
  const Node& context = *item.children[0];
  const Node* optional =
      item.children.size() > 1 ? item.children[1].get() : nullptr;
  if (optional != nullptr && optional->kind != Kind::Name &&
      !isSelfName(*optional)) {
    return failAt(diag::withAsNameOnly(), *optional);
  }

  // 管理器表达式先求值，WITH_ENTER 直接弹它。
  if (!expr(context)) return false;
  const int slot = allocSlot();
  emit("WITH_ENTER", node);
  emitU16(slot);

  // `as 名字`：WITH_ENTER 把 __enter__ 的返回值留在栈上，存进名字即可。
  if (optional != nullptr) {
    nameStore(optional->text, node);
  } else {
    emit("POP", node);
  }

  withStack_.push_back(slot);
  for (std::size_t i = 0; i < node.bodyCount; ++i) {
    if (!statement(*node.children[node.argsCount + i])) return false;
  }
  withStack_.pop_back();

  emit("WITH_EXIT", node);
  emitU16(slot);
  return true;
}

void CodeGen::emitWithExits(const Node& node) {
  // 由内向外：最内层的 with 先释放。
  for (auto it = withStack_.rbegin(); it != withStack_.rend(); ++it) {
    emit("WITH_EXIT", node);
    emitU16(*it);
  }
}

bool CodeGen::ifChain(const Node& node) {
  std::vector<int> exitFixups;
  const Node* current = &node;

  for (;;) {
    if (!expr(*current->children[0])) return false;
    emit("JUMP_IF_FALSE", *current);
    const int elseFixup = static_cast<int>(block_->code.size());
    emitU16(0);

    for (std::size_t i = 0; i < current->bodyCount; ++i) {
      if (!statement(*current->children[1 + i])) return false;
    }

    // 分支结束跳到整体末尾（除非它就是最后一段且没有 else）。
    if (current->orelseCount > 0) {
      emit("JUMP", *current);
      exitFixups.push_back(static_cast<int>(block_->code.size()));
      emitU16(0);
    }
    const int nextAt = static_cast<int>(block_->code.size());
    patchU16(static_cast<std::size_t>(elseFixup), nextAt);

    // elif 在 AST 里是「orelse 里只有一条 If」。
    const std::size_t orelseStart = 1 + current->bodyCount;
    if (current->orelseCount == 1 &&
        current->children[orelseStart]->kind == Kind::If) {
      current = current->children[orelseStart].get();
      continue;
    }
    for (std::size_t i = 0; i < current->orelseCount; ++i) {
      if (!statement(*current->children[orelseStart + i])) return false;
    }
    break;
  }

  const int exitAt = static_cast<int>(block_->code.size());
  for (int fixup : exitFixups) {
    patchU16(static_cast<std::size_t>(fixup), exitAt);
  }
  return true;
}

bool CodeGen::assignTarget(const Node& target, const Node& node) {
  if (target.kind == Kind::Name || isSelfName(target)) {
    const std::string& name = target.text;
    // `f = ...` 覆盖一个脚本函数：读名字时才会检查「函数不能当值」，
    // 写不会——允许它会把函数静默替换成普通变量。
    if (functionIndex(name) >= 0) {
      return failAt(diag::reassignFunction(name), target);
    }
    nameStore(name, node);
    return true;
  }

  if (target.kind == Kind::Subscript) {
    // 栈上已有值；下标赋值要 [容器, 下标, 值] 的顺序，先用临时槽接住。
    const int temp = slotOf("\x01assign_value");
    emit("STORE_LOCAL", node);
    emitU16(temp);
    if (!expr(*target.children[0])) return false;
    if (!expr(*target.children[1])) return false;
    emit("LOAD_LOCAL", node);
    emitU16(temp);
    emit("SET_ITEM", node);
    return true;
  }

  if (target.kind == Kind::TupleLit || target.kind == Kind::ListLit) {
    return failAt(diag::tupleUnpackAssign(), target);
  }
  return failAt(diag::badAssignTarget(), target);
}

bool CodeGen::augmented(const Node& node) {
  const Node& target = *node.children[0];
  if (target.kind == Kind::Name || isSelfName(target)) {
    const std::string name = target.text;
    nameTarget(name, node);
    if (!expr(*node.children[1])) return false;
    emit("BIN", node);
    emitU8(binopOf(node.ops[0], node));
    nameStore(name, node);
    return true;
  }

  if (target.kind == Kind::Subscript) {
    if (!expr(*target.children[0])) return false;
    if (!expr(*target.children[1])) return false;
    if (!expr(*node.children[1])) return false;
    emit("INPLACE", node);
    emitU8(binopOf(node.ops[0], node));
    return true;
  }

  return failAt(diag::unsupportedAugTarget(), target);
}

bool CodeGen::forLoop(const Node& node) {
  if (node.orelseCount > 0) return failAt(diag::forElse(), node);

  const Node& target = *node.children[0];
  if (target.kind != Kind::Name && !isSelfName(target)) {
    return failAt(diag::loopVariableName(), target);
  }

  if (!expr(*node.children[1])) return false;
  emit("ITER_INIT", node);
  const int loopTop = static_cast<int>(block_->code.size());
  emit("ITER_NEXT", node);
  const int exitFixup = static_cast<int>(block_->code.size());
  emitU16(0);
  nameStore(target.text, node);

  loopStack_.push_back(LoopFrame{loopTop, {}, true});
  for (std::size_t i = 0; i < node.bodyCount; ++i) {
    if (!statement(*node.children[2 + i])) return false;
  }
  const LoopFrame frame = loopStack_.back();
  loopStack_.pop_back();

  emit("JUMP", node);
  emitU16(loopTop);
  const int exitAt = static_cast<int>(block_->code.size());
  patchU16(static_cast<std::size_t>(exitFixup), exitAt);
  for (int fixup : frame.breakFixups) {
    patchU16(static_cast<std::size_t>(fixup), exitAt);
  }
  return true;
}

bool CodeGen::importStatement(const Node& node) {
  // 依赖图是构建期的事；这里只记「这个模块被用了」。
  // 幂等：compileModule 预扫一遍，模块级执行时又会走到这里。
  auto add = [this](const std::string& name) {
    if (name.empty()) return;
    for (const std::string& existing : imported_) {
      if (existing == name) return;
    }
    imported_.push_back(name);
  };

  if (node.kind == Kind::Import) {
    for (const auto& alias : node.children) add(alias->text);
  } else {
    add(node.text);
  }
  return true;
}

}  // namespace pcc
