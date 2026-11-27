// 语句的代码生成，逐段对照 compiler.py 的 statement/with/if/assign/for。
//
// ── 发射顺序就是字节码本身 ──
//
// 这个文件里几乎没有「算法」，全是「先发哪条、后发哪条」。改顺序不会报错，
// 只会让产物与 Python 版不一致——所以每段的注释都写清它对应 Python 版的
// 哪几行，方便对照，而不是解释代码在做什么。
#include "pcc/codegen/codegen.h"

#include "aipy/exceptions.h"
#include "pcc/diagnostics.h"

namespace pcc {

bool CodeGen::statement(const Node& node) {
  switch (node.kind) {
    case Kind::Expr: {
      const Node& value = *node.children[0];
      if (value.kind == Kind::YieldFrom) {
        // `yield from x` 作为语句：编译成迭代 + YIELD，yield 的值丢掉。
        if (!expr(value)) return false;
        emit("POP", node);
        return true;
      }
      if (value.kind == Kind::Yield) {
        // `yield x` 作为语句：yield 的值没人要，直接丢掉。
        if (!expr(value)) return false;
        emit("POP", node);
        return true;
      }
      if (value.kind == Kind::Await) {
        // `await f()` 作为语句：驱动协程，丢掉返回值。
        if (!expr(value)) return false;
        emit("POP", node);
        return true;
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
      // `while ... else`：条件变假之后执行 else；`break` 跳过 else。
      // 所以 break 的目标要指向「else 之后」，而条件失败的目标指向 else 开头。
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

      // 条件失败落在这里：先跑 else，再跳过它（break 从上面直接跳到末尾）。
      const int elseStart = static_cast<int>(block_->code.size());
      patchU16(static_cast<std::size_t>(exitFixup), elseStart);
      int skipFixup = -1;
      if (node.orelseCount > 0) {
        const std::size_t orelseStart = 1 + node.bodyCount;
        for (std::size_t i = 0; i < node.orelseCount; ++i) {
          if (!statement(*node.children[orelseStart + i])) return false;
        }
        emit("JUMP", node);
        skipFixup = static_cast<int>(block_->code.size());
        emitU16(0);
      }
      const int exitAt = static_cast<int>(block_->code.size());
      if (skipFixup >= 0) {
        patchU16(static_cast<std::size_t>(skipFixup), exitAt);
      }
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
      // 声明本身不产字节码：名字早已在 registerNestedFunctions 里被登记成
      // 捕获的自由变量（捕获槽就是单元）。
      return true;

    case Kind::Assert:
      return assertStatement(node);

    case Kind::Raise:
      return raiseStatement(node);

    case Kind::With:
      return withStatement(node);

    case Kind::Try:
      return tryStatement(node);

    case Kind::FunctionDef:
    case Kind::AsyncFunctionDef:
      // 顶层 def 的签名已在 registerFunction 登记过，这里只发默认值求值：
      // CPython 在**执行到这一句时**求值默认参数，所以位置必须在这里。
      if (block_ == &module_) {
        if (!emitDefaultValues(node, *block_)) return false;
        if (node.decoratorCount > 0) return applyDecorators(node);
        return true;
      }
      // 嵌套 def：登记早已完成（registerNestedFunctions），这里发
      // 「求值默认值 → 压捕获 → MAKE_FUNCTION → 存进名字」。
      return nestedDefStatement(node);

    case Kind::Match:
      return matchCodegen(node);

    case Kind::Delete:
      return deleteStatement(node);

    case Kind::ClassDef:
      // Activity 已在 expandActivity 里展开成模块级语句，不会走到这里。
      return compileClass(node);

    case Kind::Import:
    case Kind::ImportFrom:
      return importStatement(node);

    default:
      break;
  }
  return failAt(diag::unsupportedStatement(kind_name(node.kind)), node);
}

bool CodeGen::withStatement(const Node& node) {
  // 多条项 `with a, b:` 与嵌套 `with a: with b:` 语义完全一致：
  // 依次进入，退出时反序。按嵌套生成，进入失败时已进入的那几层照常退出
  // ——WITH_ENTER 只在 __enter__ 成功后才压块（见 machine.cpp）。
  std::vector<int> entered;
  std::vector<int> exitFixups;
  for (std::size_t index = 0; index < node.argsCount; ++index) {
    const Node& item = *node.children[index];
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
    // 第二个操作数：`__exit__` 返回真值（吞掉异常）时跳到这里。先占位，
    // 等整个 with 语句发完再回填成「with 之后」的偏移。
    const int exitFixup = static_cast<int>(block_->code.size());
    emitU16(0);

    // `as 名字`：WITH_ENTER 把 __enter__ 的返回值留在栈上，存进名字即可。
    if (optional != nullptr) {
      nameStore(optional->text, node);
    } else {
      emit("POP", node);
    }

    entered.push_back(slot);
    withStack_.push_back(slot);
    exitFixups.push_back(exitFixup);
  }

  for (std::size_t i = 0; i < node.bodyCount; ++i) {
    if (!statement(*node.children[node.argsCount + i])) return false;
  }

  for (std::size_t count = entered.size(); count > 0; --count) {
    withStack_.pop_back();
    emit("WITH_EXIT", node);
    emitU16(entered[count - 1]);
  }

  // 被吞掉的异常从这里继续。嵌套（多条目）时所有层都指向同一个终点：
  // 内层吞掉后外层也已经退出了，直接落到语句之后即可。
  const int after = static_cast<int>(block_->code.size());
  for (int fixup : exitFixups) {
    patchU16(static_cast<std::size_t>(fixup), after);
  }
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
  if (target.kind == Kind::Attribute && !isSelfName(target)) {
    // `a.b = v`：栈上已有值。SET_ATTR 要 [接收者, 值]，先接住值再压接收者。
    const int temp = slotOf("\x01assign_value");
    emit("STORE_LOCAL", node);
    emitU16(temp);
    if (!expr(*target.children[0])) return false;
    emit("LOAD_LOCAL", node);
    emitU16(temp);
    emit("SET_ATTR", node);
    emitU16(constIndexOfStr(target.text));
    return true;
  }

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
    if (target.children[1]->kind == Kind::Slice) {
      // 切片赋值：压 [容器, start?, stop?, step?, 值]，再 SET_SLICE。
      // 位标记与读取时一致（1=start 2=stop 4=step）；值排在最后，
      // 因为 SET_SLICE 先弹值再弹边界。
      const Node& slice = *target.children[1];
      int flags = 0;
      std::size_t child = 0;
      if (slice.sliceFlags & 1) {
        flags |= 1;
        if (!expr(*slice.children[child++])) return false;
      }
      if (slice.sliceFlags & 2) {
        flags |= 2;
        if (!expr(*slice.children[child++])) return false;
      }
      if (slice.sliceFlags & 4) {
        flags |= 4;
        if (!expr(*slice.children[child++])) return false;
      }
      emit("LOAD_LOCAL", node);
      emitU16(temp);
      emit("SET_SLICE", node);
      emitU8(flags);
      return true;
    }
    if (!expr(*target.children[1])) return false;
    emit("LOAD_LOCAL", node);
    emitU16(temp);
    emit("SET_ITEM", node);
    return true;
  }

  if (target.kind == Kind::TupleLit || target.kind == Kind::ListLit) {
    // 元组解包：栈上已有序列。UNPACK 把它拆成 N 个元素（第一个在最上），
    // 随后按**从后往前**的顺序逐个 assignTarget——每个目标都会弹掉一个。
    const std::size_t count = target.children.size();
    emit("UNPACK", target);
    emitU16(static_cast<int>(count));
    for (std::size_t i = count; i > 0; --i) {
      if (!assignTarget(*target.children[i - 1], node)) return false;
    }
    return true;
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

bool CodeGen::matchCodegen(const Node& node) {
  // `match` 降级成 if/elif 链：
  //   match X:              _m = X
  //       case 1:           if _m == 1: ...
  //       case _:           else: ...
  //
  // 主题只求值一次，存进隐藏局部槽——`match f():` 重复求值会让副作用跑多次。
  const Node& subject = *node.children[0];
  const int temp = slotOf("\x01match_subject");
  if (!expr(subject)) return false;
  emit("STORE_LOCAL", node);
  emitU16(temp);

  std::vector<int> endFixups;   // body 跑完 → match 之后
  int nextCaseFixups[64];       // 「本 case 不匹配」→ 下一个 case 起点
  int nextCaseCount = 0;

  const std::size_t caseCount = node.bodyCount;
  for (std::size_t i = 0; i < caseCount; ++i) {
    const Node& clause = *node.children[1 + i];
    const Node& pattern = *clause.children[0];

    // 上一个 case 的「不匹配」落点就是这里。
    const int caseStart = static_cast<int>(block_->code.size());
    for (int k = 0; k < nextCaseCount; ++k) {
      patchU16(static_cast<std::size_t>(nextCaseFixups[k]), caseStart);
    }
    nextCaseCount = 0;

    // 模式是裸名字（非 `_`）时是「捕获模式」：总是命中并把主题绑给这个名字。
    const bool capture =
        pattern.kind == Kind::Name && pattern.text != "_";
    std::string bindName = clause.text;
    if (capture) bindName = pattern.text;
    const bool wildcard =
        (pattern.kind == Kind::Name && pattern.text == "_") || capture;
    // 捕获模式没有任何匹配判据（总是命中），所以先绑定、再判守卫——CPython
    // 里 `case n if n > 50:` 的守卫能看到已绑定的 n。
    if (wildcard && !bindName.empty()) {
      emit("LOAD_LOCAL", clause);
      emitU16(temp);
      nameStore(bindName, clause);
      bindName.clear();
    }

    if (!wildcard) {
      if (pattern.kind == Kind::TupleLit && pattern.flag) {
        // 或模式 → `_m in (a, b, ...)`。先压主题（元素），再建元组
        // （容器）：CMP 取栈底作 `a`、栈顶作 `b`，而 IN 的语义是
        // contains(b, a)。
        emit("LOAD_LOCAL", pattern);
        emitU16(temp);
        for (const auto& item : pattern.children) {
          if (!expr(*item)) return false;
        }
        emit("BUILD_TUPLE", pattern);
        emitU16(static_cast<int>(pattern.children.size()));
        emit("CMP", pattern);
        emitU8(6);   // CmpOp 编号：IN
      } else {
        emit("LOAD_LOCAL", pattern);
        emitU16(temp);
        if (!expr(pattern)) return false;
        emit("CMP", pattern);
        emitU8(0);   // CmpOp 编号：EQ
      }
      emit("JUMP_IF_FALSE", clause);
      nextCaseFixups[nextCaseCount++] = static_cast<int>(block_->code.size());
      emitU16(0);

      // `case P as name:`：先确认模式命中，再绑定，然后才判守卫。
      if (!bindName.empty()) {
        emit("LOAD_LOCAL", clause);
        emitU16(temp);
        nameStore(bindName, clause);
        bindName.clear();
      }
    }

    // 守卫对通配/捕获模式同样要判：`case n if n > 50:` 必须真的看 n。
    if (clause.flag) {
      const Node& guard = *clause.children[1];
      if (!expr(guard)) return false;
      emit("JUMP_IF_FALSE", guard);
      nextCaseFixups[nextCaseCount++] = static_cast<int>(block_->code.size());
      emitU16(0);
    }

    const std::size_t firstBody = 1 + (clause.flag ? 1 : 0);
    for (std::size_t k = 0; k < clause.bodyCount; ++k) {
      if (!statement(*clause.children[firstBody + k])) return false;
    }

    if (!wildcard || i + 1 < caseCount) {
      emit("JUMP", clause);
      endFixups.push_back(static_cast<int>(block_->code.size()));
      emitU16(0);
    }
  }

  const int endAt = static_cast<int>(block_->code.size());
  for (int k = 0; k < nextCaseCount; ++k) {
    patchU16(static_cast<std::size_t>(nextCaseFixups[k]), endAt);
  }
  for (int fixup : endFixups) {
    patchU16(static_cast<std::size_t>(fixup), endAt);
  }
  return true;
}

bool CodeGen::forLoop(const Node& node) {
  const Node& target = *node.children[0];
  const bool tupleTarget =
      target.kind == Kind::TupleLit || target.kind == Kind::ListLit;
  if (target.kind != Kind::Name && !isSelfName(target) && !tupleTarget) {
    return failAt(diag::loopVariableName(), target);
  }

  if (!expr(*node.children[1])) return false;
  emit("ITER_INIT", node);
  const int loopTop = static_cast<int>(block_->code.size());
  emit("ITER_NEXT", node);
  const int exitFixup = static_cast<int>(block_->code.size());
  emitU16(0);
  // 元组目标：ITER_NEXT 压上来的元素本身是序列，交给 assignTarget 拆。
  if (tupleTarget) {
    if (!assignTarget(target, node)) return false;
  } else {
    nameStore(target.text, node);
  }

  loopStack_.push_back(LoopFrame{loopTop, {}, true});
  for (std::size_t i = 0; i < node.bodyCount; ++i) {
    if (!statement(*node.children[2 + i])) return false;
  }
  const LoopFrame frame = loopStack_.back();
  loopStack_.pop_back();

  emit("JUMP", node);
  emitU16(loopTop);

  // 迭代耗尽落在这里：先跑 else，再跳到末尾；break 直接跳到末尾。
  const int elseStart = static_cast<int>(block_->code.size());
  patchU16(static_cast<std::size_t>(exitFixup), elseStart);
  int skipFixup = -1;
  if (node.orelseCount > 0) {
    const std::size_t orelseStart = 2 + node.bodyCount;
    for (std::size_t i = 0; i < node.orelseCount; ++i) {
      if (!statement(*node.children[orelseStart + i])) return false;
    }
    emit("JUMP", node);
    skipFixup = static_cast<int>(block_->code.size());
    emitU16(0);
  }
  const int exitAt = static_cast<int>(block_->code.size());
  if (skipFixup >= 0) {
    patchU16(static_cast<std::size_t>(skipFixup), exitAt);
  }
  for (int fixup : frame.breakFixups) {
    patchU16(static_cast<std::size_t>(fixup), exitAt);
  }
  return true;
}

bool CodeGen::deleteStatement(const Node& node) {
  // 每个目标发一条 DEL_LOCAL / DEL_GLOBAL：u16 槽号 + u16 名字常量号
  // （名字只用于运行期的 NameError 消息）。
  for (const NodePtr& target : node.children) {
    if (target->kind != Kind::Name) {
      return failAt(diag::nameExpected("a variable name"), *target);
    }
    const std::string& name = target->text;
    if (isLocal(name)) {
      emit("DEL_LOCAL", *target);
      emitU16(slotOf(name));
    } else {
      emit("DEL_GLOBAL", *target);
      emitU16(globalSlot(name));
    }
    emitU16(constIndexOfStr(name));
  }
  return true;
}

bool CodeGen::assertStatement(const Node& node) {
  // `assert cond` / `assert cond, msg` 展开成：
  //
  //     <cond>
  //     JUMP_IF_FALSE L_THROW   ← 条件为假时才去抛
  //     JUMP L_END
  //   L_THROW:
  //     <msg>                   ← 消息（没有就不压栈）
  //     RAISE_TYPE "AssertionError", 有消息?
  //   L_END:
  //
  // 走 RAISE_TYPE 而不是先构造异常对象：类型在编译期就确定，与 raise 一致。
  if (node.children.empty()) return failAt(diag::assertUnsupported(), node);

  if (!expr(*node.children[0])) return false;
  emit("JUMP_IF_FALSE", node);
  const int throwFixup = static_cast<int>(block_->code.size());
  emitU16(0);

  emit("JUMP", node);
  const int endFixup = static_cast<int>(block_->code.size());
  emitU16(0);

  patchU16(static_cast<std::size_t>(throwFixup),
           static_cast<int>(block_->code.size()));
  const bool hasMessage = node.children.size() > 1;
  if (hasMessage && !expr(*node.children[1])) return false;
  emit("RAISE_TYPE", node);
  emitU16(constIndexOfStr("AssertionError"));
  emitU8(hasMessage ? 1 : 0);

  patchU16(static_cast<std::size_t>(endFixup),
           static_cast<int>(block_->code.size()));
  return true;
}

bool CodeGen::raiseStatement(const Node& node) {
  // `raise` 单独一行：重抛当前处理的异常。
  if (node.children.empty()) {
    emit("RERAISE", node);
    return true;
  }

  const Node& value = *node.children[0];

  // `raise ValueError` / `raise ValueError("msg")`：类型名在编译期就能确定，
  // 所以发 RAISE_TYPE（类型名进常量池），运行期不必再查「这个名字是不是
  // 异常类」。拼错的名字在这里就报错，而不是等脚本跑到那一行。
  if (value.kind == Kind::Call && value.children.size() > 0 &&
      value.children[0]->kind == Kind::Name) {
    const std::string& callee = value.children[0]->text;
    if (aipy::isExceptionClassName(callee)) {
      // 只接受 0 或 1 个位置参数：CPython 的多种内建异常构造形式里，
      // 这里只实现「一个消息」——`args` 元组属元组语义，等元组落地再说。
      const std::size_t argc = value.argsCount;
      if (argc > 1) return failAt(diag::raiseTooManyArgs(callee), value);
      if (argc == 1) {
        if (!expr(*value.children[1])) return false;
      }
      emit("RAISE_TYPE", node);
      emitU16(constIndexOfStr(callee));
      emitU8(argc == 1 ? 1 : 0);
      return true;
    }
  }

  if (value.kind == Kind::Name && aipy::isExceptionClassName(value.text)) {
    emit("RAISE_TYPE", node);
    emitU16(constIndexOfStr(value.text));
    emitU8(0);
    return true;
  }

  // 其它表达式：求值后按「抛出这个值」处理。VM 会在它不是异常对象时
  // 报 TypeError，与 CPython 一致。
  if (!expr(value)) return false;
  emit("RAISE", node);
  return true;
}

bool CodeGen::tryStatement(const Node& node) {
  // ── 布局 ──
  //
  // try/except 部分整体放在「finally 保护区」里：这样无论是正常结束、
  // 还是 except 没接住而向外传播，控制流都会先经过 finally，再由 finally
  // 之后的 RERAISE 继续往外抛。CPython 的语义正是这个。
  //
  //   [有 finally]
  //     SETUP_EXC L_FIN_ENTRY      ← 外层：finally 的保护区
  //   [try/except/else 整段]
  //   [有 finally]
  //     POP_EXC                    ← 正常穿出 finally 保护区
  //     <finally 体>
  //     JUMP L_END
  //   L_FIN_ENTRY:                 ← 异常进来时栈顶是异常值
  //     <finally 体>
  //     RAISE                      ← 重新抛出（finally 不吞异常）
  //   L_END:
  //
  // 没有 finally 时就只有 try/except/else 那一段。
  const bool hasFinally = node.finalCount > 0;

  int finallyEntryFixup = -1;
  if (hasFinally) {
    emit("SETUP_EXC", node);
    finallyEntryFixup = static_cast<int>(block_->code.size());
    emitU16(0);
  }

  // ── try 体 ──
  //
  // 只有存在 except 子句时才需要保护区：没有 except 而只有 finally 时，
  // 异常应当直接穿到外层（外层那个 finally 保护区就是干这个的），
  // 在这里再包一层只会让 finally 执行两次。
  int handlerFixup = -1;
  if (node.handlerCount > 0) {
    emit("SETUP_EXC", node);
    handlerFixup = static_cast<int>(block_->code.size());
    emitU16(0);
  }

  for (std::size_t i = 0; i < node.bodyCount; ++i) {
    if (!statement(*node.children[i])) return false;
  }

  if (node.handlerCount > 0) emit("POP_EXC", node);

  // ── else ──
  //
  // else 只在 try 体没有异常时执行，且**不**在保护区内（CPython 如此：
  // else 里抛出的异常不会被自己的 except 捕获）。
  const std::size_t handlerStart = node.bodyCount;
  const std::size_t orelseStart = handlerStart + node.handlerCount;
  const std::size_t finalStart = orelseStart + node.orelseCount;

  int elseExitFixup = -1;
  const bool hasHandlers = node.handlerCount > 0;
  if (hasHandlers) {
    // try 体正常结束：跳过所有 handler，去执行 else。
    emit("JUMP", node);
    elseExitFixup = static_cast<int>(block_->code.size());
    emitU16(0);
  }

  if (hasHandlers) {
    // ── handler 入口 ──
    const int handlerAt = static_cast<int>(block_->code.size());
    patchU16(static_cast<std::size_t>(handlerFixup), handlerAt);

    // 每个子句：类型匹配则进入子句体，不匹配则继续下一条 EXC_MATCH；
    // 所有子句都不匹配，就落到末尾的 RERAISE。
    std::vector<int> handlerExits;
    for (std::size_t i = 0; i < node.handlerCount; ++i) {
      const Node& handler = *node.children[handlerStart + i];
      const std::size_t bodyStart = handler.hasType ? 1 : 0;

      if (handler.hasType) {
        const Node& type = *handler.children[0];
        // 异常类型只认内建类名：脚本里没有能继承 BaseException 的 class，
        // 所以编译期就能确定。元组形式展开成多条 EXC_MATCH。
        std::vector<std::string> types;
        if (type.kind == Kind::TupleLit) {
          for (const auto& element : type.children) {
            if (element->kind != Kind::Name ||
                !aipy::isExceptionClassName(element->text)) {
              return failAt(
                  diag::exceptTypeNotException(element->kind == Kind::Name
                                                   ? element->text
                                                   : std::string("<expr>")),
                  *element);
            }
            types.push_back(element->text);
          }
        } else if (type.kind == Kind::Name && aipy::isExceptionClassName(type.text)) {
          types.push_back(type.text);
        } else {
          return failAt(diag::exceptTypeNotException(
                            type.kind == Kind::Name ? type.text
                                                    : std::string("<expr>")),
                        type);
        }

        // 命中目标统一指到子句体起点，四条匹配共享同一个出口。
        const std::size_t matchSites = block_->code.size();
        for (const std::string& name : types) {
          emit("EXC_MATCH", handler);
          emitU16(0);  // 先占位，稍后 patch 成子句体起点
          emitU16(constIndexOfStr(name));
        }
        const int bodyAt = static_cast<int>(block_->code.size());
        // EXC_MATCH 的操作数是 u16 目标 + u16 常量号，各 2 字节，
        // 所以补丁位置是「指令后第一个字节」。
        for (std::size_t k = 0; k < types.size(); ++k) {
          // 每条 5 字节：1 操作码 + 2 目标 + 2 常量号
          patchU16(matchSites + k * 5 + 1, bodyAt);
        }
      }

      // `as e`：把当前异常存进名字。裸 except 不必取。
      if (handler.hasExceptVar) {
        emit("CURRENT_EXC", handler);
        nameStore(handler.text, handler);
      }

      for (std::size_t j = 0; j < handler.handlerBodyCount; ++j) {
        if (!statement(*handler.children[bodyStart + j])) return false;
      }
      emit("CLEAR_EXC", handler);

      emit("JUMP", handler);
      handlerExits.push_back(static_cast<int>(block_->code.size()));
      emitU16(0);
    }

    // 所有子句都不匹配：重抛。
    emit("RERAISE", node);

    const int afterHandlers = static_cast<int>(block_->code.size());
    patchU16(static_cast<std::size_t>(elseExitFixup), afterHandlers);

    // ── else 体 ──
    for (std::size_t i = 0; i < node.orelseCount; ++i) {
      if (!statement(*node.children[orelseStart + i])) return false;
    }

    const int afterElse = static_cast<int>(block_->code.size());
    for (int fixup : handlerExits) {
      patchU16(static_cast<std::size_t>(fixup), afterElse);
    }
  } else {
    // 没有 except 就只可能是「只有 finally」：解析器保证至少一个。
    // try 体已经发完，异常直接由外层 finally 保护区接管。
  }

  // ── finally ──
  if (hasFinally) {
    // 正常路径：穿出外层保护区，执行 finally 体，然后跳过异常入口。
    emit("POP_EXC", node);
    for (std::size_t i = 0; i < node.finalCount; ++i) {
      if (!statement(*node.children[finalStart + i])) return false;
    }
    emit("JUMP", node);
    const int normalFixup = static_cast<int>(block_->code.size());
    emitU16(0);

    // 异常路径：异常值已在栈顶，先执行 finally，再重新抛出。
    patchU16(static_cast<std::size_t>(finallyEntryFixup),
             static_cast<int>(block_->code.size()));
    for (std::size_t i = 0; i < node.finalCount; ++i) {
      if (!statement(*node.children[finalStart + i])) return false;
    }
    emit("RAISE", node);

    patchU16(static_cast<std::size_t>(normalFixup),
             static_cast<int>(block_->code.size()));
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
