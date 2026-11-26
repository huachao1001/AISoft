// 表达式的代码生成，逐段对照 compiler.py 的 expr/slice/call/constant。
#include "pcc/codegen/codegen.h"

#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

#include <cstdlib>
#include <cstring>

namespace pcc {
namespace {

// 整数字面量：十进制 / 0x / 0o / 0b，允许下划线。
long long parseIntLiteral(const std::string& literal) {
  std::string text;
  text.reserve(literal.size());
  for (char ch : literal) {
    if (ch != '_') text.push_back(ch);
  }
  const bool prefixed =
      text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X' || text[1] == 'o' ||
       text[1] == 'O' || text[1] == 'b' || text[1] == 'B');
  if (!prefixed) return std::strtoll(text.c_str(), nullptr, 10);

  const char marker = static_cast<char>(text[1] | 0x20);  // 转小写
  const int base = marker == 'x' ? 16 : (marker == 'o' ? 8 : 2);
  return static_cast<long long>(
      std::strtoull(text.c_str() + 2, nullptr, base));
}

// 复数的报错文案里要用原始写法（Python 的 %r 是 `1j` 这类）。
std::string literalRepr(const Node& node) {
  if (node.constKind == ConstKind::Str) {
    const char* prefix = node.flag ? "b" : "";
    return std::string(prefix) + "'" + node.text + "'";
  }
  return node.text;
}

// 把 `os` / `os.path` / `os.path.joiner` 这样的纯属性链摊成点分名。
//
// 只接受「根是 Name、往上全是 Attribute」这一种形态：`f().x` 或 `a[0].x`
// 不是模块引用，返回空串让调用方走方法调用那条路。
std::string dottedName(const Node& node) {
  if (node.kind == Kind::Name) return node.text;
  if (node.kind != Kind::Attribute) return std::string();
  if (node.children.empty()) return std::string();
  const std::string base = dottedName(*node.children[0]);
  if (base.empty()) return std::string();
  return base + "." + node.text;
}

}  // namespace

bool CodeGen::nameRead(const std::string& name, const Node& node) {
  // `len` 作为值出现时没有意义（没有一等函数），但 `len(x)` 走 call。
  if (Tables::isBuiltin(name)) {
    return failAt(diag::builtinAsValue(name), node);
  }
  // `g = f` 会编译成「读一个叫 f 的全局变量」，而 f 从没被存进去过。
  if (functionIndex(name) >= 0) {
    return failAt(diag::functionAsValue(name), node);
  }
  nameTarget(name, node);
  return true;
}

bool CodeGen::expr(const Node& node) {
  switch (node.kind) {
    case Kind::Constant:
      return constant(node);

    case Kind::Name:
      return nameRead(node.text, node);

    case Kind::Attribute: {
      // 方法体里的 `self.x` 读成模块级名字 x。
      if (isSelfName(node)) return nameRead(node.text, node);
      return failAt(diag::attributeAccess(), node);
    }

    case Kind::BinOp: {
      if (!expr(*node.children[0])) return false;
      if (!expr(*node.children[1])) return false;
      emit("BIN", node);
      emitU8(binopOf(node.ops[0], node));
      return true;
    }

    case Kind::UnaryOp: {
      if (!expr(*node.children[0])) return false;
      emit("UNARY", node);
      emitU8(unopOf(node.ops[0], node));
      return true;
    }

    case Kind::BoolOp: {
      // and/or 返回操作数本身，且短路。每条 KEEP 跳转后面必须紧跟 POP：
      // 短路命中时值要留在栈上当结果，没命中时要弹掉再求下一个操作数。
      // 少了 POP，`a and b` 的两个操作数会同时留在栈上：
      // `print("x: " + str(a and b))` 会输出 TrueTrue。
      const bool isAnd = node.ops[0] == Kind::And;
      const char* keep = isAnd ? "JUMP_IF_FALSE_KEEP" : "JUMP_IF_TRUE_KEEP";
      std::vector<int> fixups;
      for (std::size_t i = 0; i + 1 < node.children.size(); ++i) {
        if (!expr(*node.children[i])) return false;
        emit(keep, node);
        fixups.push_back(static_cast<int>(block_->code.size()));
        emitU16(0);
        emit("POP", node);
      }
      if (!expr(*node.children.back())) return false;
      const int end = static_cast<int>(block_->code.size());
      for (int fixup : fixups) {
        patchU16(static_cast<std::size_t>(fixup), end);
      }
      return true;
    }

    case Kind::Compare: {
      // 链式比较 a < b < c：中间项只求值一次。
      if (node.ops.size() == 1) {
        if (!expr(*node.children[0])) return false;
        if (!expr(*node.children[1])) return false;
        emit("CMP", node);
        emitU8(cmpopOf(node.ops[0], node));
        return true;
      }

      const int leftSlot = slotOf("\x01chained_left");
      const int rightSlot = slotOf("\x01chained_right");
      if (!expr(*node.children[0])) return false;
      emit("STORE_LOCAL", node);
      emitU16(leftSlot);

      std::vector<int> falseFixups;
      for (std::size_t i = 0; i < node.ops.size(); ++i) {
        if (!expr(*node.children[i + 1])) return false;
        emit("STORE_LOCAL", node);
        emitU16(rightSlot);
        emit("LOAD_LOCAL", node);
        emitU16(leftSlot);
        emit("LOAD_LOCAL", node);
        emitU16(rightSlot);
        emit("CMP", node);
        emitU8(cmpopOf(node.ops[i], node));

        // 最后一个比较的结果就是整个表达式的结果。
        if (i + 1 == node.ops.size()) break;

        // 中间结果只用来决定是否继续；不继续时整个表达式是 False。
        emit("JUMP_IF_FALSE", node);
        falseFixups.push_back(static_cast<int>(block_->code.size()));
        emitU16(0);
        // 继续：右值成为下一轮的左值。
        emit("LOAD_LOCAL", node);
        emitU16(rightSlot);
        emit("STORE_LOCAL", node);
        emitU16(leftSlot);
      }

      emit("JUMP", node);
      const int endFixup = static_cast<int>(block_->code.size());
      emitU16(0);

      const int falseAt = static_cast<int>(block_->code.size());
      for (int fixup : falseFixups) {
        patchU16(static_cast<std::size_t>(fixup), falseAt);
      }
      emit("FALSE", node);

      const int endAt = static_cast<int>(block_->code.size());
      patchU16(static_cast<std::size_t>(endFixup), endAt);
      return true;
    }

    case Kind::IfExp: {
      // x if c else y：只求值一支。
      if (!expr(*node.children[0])) return false;
      emit("JUMP_IF_FALSE", node);
      const int elseFixup = static_cast<int>(block_->code.size());
      emitU16(0);
      if (!expr(*node.children[1])) return false;
      emit("JUMP", node);
      const int endFixup = static_cast<int>(block_->code.size());
      emitU16(0);
      const int elseAt = static_cast<int>(block_->code.size());
      patchU16(static_cast<std::size_t>(elseFixup), elseAt);
      if (!expr(*node.children[2])) return false;
      const int endAt = static_cast<int>(block_->code.size());
      patchU16(static_cast<std::size_t>(endFixup), endAt);
      return true;
    }

    case Kind::Call:
      return call(node);

    case Kind::Subscript: {
      if (!expr(*node.children[0])) return false;
      const Node& slice = *node.children[1];
      if (slice.kind == Kind::Slice) return sliceExpr(slice, node);
      if (!expr(slice)) return false;
      emit("GET_ITEM", node);
      return true;
    }

    case Kind::ListLit: {
      for (const auto& element : node.children) {
        if (!expr(*element)) return false;
      }
      emit("BUILD_LIST", node);
      emitU16(static_cast<int>(node.children.size()));
      return true;
    }

    case Kind::DictLit: {
      // 字典展开 `**d`：Python 版检查的是「有没有键为 None」。
      // 解析期用 flag 标记，所以这里按 keyCount 与子节点数是否对得上判断。
      if (node.children.size() != node.keyCount * 2) {
        return failAt(diag::dictSpreadUnsupported(), node);
      }
      for (const auto& child : node.children) {
        if (!expr(*child)) return false;
      }
      emit("BUILD_DICT", node);
      emitU16(static_cast<int>(node.keyCount));
      return true;
    }

    case Kind::JoinedStr: {
      // f-string 展开成 `"a" + str(x) + "b"`，用 ADD 链即可。
      //
      // 表达式段必须显式 str()：`f"{1}"` 得到 "1"，而 `"a" + 1` 在 CPython
      // 是 TypeError。CPython 的 f-string 内部同样对每段调 format()。
      if (node.children.empty()) {
        emit("CONST", node);
        emitU16(constIndexOfStr(""));
        return true;
      }
      bool emitted = false;
      for (const NodePtr& part : node.children) {
        if (part->kind == Kind::Constant) {
          emit("CONST", *part);
          emitU16(constIndexOfStr(part->text));
        } else {
          if (!expr(*part)) return false;
          emit("CALL_BUILTIN", *part);
          emitU16(builtinIndex("str"));
          emitU8(1);
        }
        if (emitted) {
          emit("BIN", *part);
          emitU8(binopOf(Kind::Add, *part));
        }
        emitted = true;
      }
      return true;
    }

    case Kind::TupleLit: {
      for (const auto& element : node.children) {
        if (!expr(*element)) return false;
      }
      emit("BUILD_TUPLE", node);
      emitU16(static_cast<int>(node.children.size()));
      return true;
    }

    case Kind::Slice:
      // 切片只能出现在下标里；单独出现是 `x[0:1] = v` 这类被拒的写法。
      return failAt(diag::unsupportedExpression("Slice"), node);

    case Kind::Lambda:
      return failAt(diag::unsupportedExpression("Lambda"), node);

    case Kind::NamedExpr:
      return failAt(diag::unsupportedExpression("NamedExpr"), node);

    case Kind::SetLit: {
      for (const auto& element : node.children) {
        if (!expr(*element)) return false;
      }
      emit("BUILD_SET", node);
      emitU16(static_cast<int>(node.children.size()));
      return true;
    }

    case Kind::ListComp:
      return failAt(diag::unsupportedExpression("ListComp"), node);

    case Kind::SetComp:
      return failAt(diag::unsupportedExpression("SetComp"), node);

    case Kind::DictComp:
      return failAt(diag::unsupportedExpression("DictComp"), node);

    case Kind::GeneratorExp:
      return failAt(diag::unsupportedExpression("GeneratorExp"), node);

    case Kind::Starred:
      return failAt(diag::unsupportedExpression("Starred"), node);

    case Kind::Await:
      return failAt(diag::unsupportedExpression("Await"), node);

    case Kind::Yield:
      return failAt(diag::unsupportedExpression("Yield"), node);

    case Kind::YieldFrom:
      return failAt(diag::unsupportedExpression("YieldFrom"), node);

    default:
      break;
  }
  return failAt(diag::unsupportedExpression(kind_name(node.kind)), node);
}

bool CodeGen::sliceExpr(const Node& node, const Node& parent) {
  // 容器已在栈底，按 start/stop/step 的顺序依次压入存在的操作数。
  // 位标记 1=start 2=stop 4=step，与 SLICE 指令一致。
  int flags = 0;
  std::size_t child = 0;
  if (node.sliceFlags & 1) {
    flags |= 1;
    if (!expr(*node.children[child++])) return false;
  }
  if (node.sliceFlags & 2) {
    flags |= 2;
    if (!expr(*node.children[child++])) return false;
  }
  if (node.sliceFlags & 4) {
    flags |= 4;
    if (!expr(*node.children[child++])) return false;
  }
  emit("SLICE", parent);
  emitU8(flags);
  return true;
}

bool CodeGen::call(const Node& node) {
  const Node& func = *node.children[0];
  const std::size_t total = node.children.size() - 1;

  // 方法体里的 `self.g()` 与 `g()` 是同一件事：Python 版在编译前把
  // `self.g` 改写成了 `g`，于是它走「脚本内函数」那条分支。不这样处理
  // 会退化成 CALL_METHOD（按名字查表），而脚本函数从来不在方法表里。
  const bool selfCall = isSelfName(func);
  const std::string calleeName = func.text;

  // 内建的判断必须先于脚本函数（与 Python 版同序）。
  if ((func.kind == Kind::Name || selfCall) &&
      Tables::isBuiltin(calleeName)) {
    if (total != node.argsCount) {
      return failAt(diag::keywordArguments(), node);
    }
    for (std::size_t i = 0; i < total; ++i) {
      if (!expr(*node.children[1 + i])) return false;
    }
    emit("CALL_BUILTIN", node);
    emitU16(builtinIndex(calleeName));
    emitU8(static_cast<int>(total));
    return true;
  }

  // 脚本内函数：CALL + 函数号。
  if (func.kind == Kind::Name || selfCall) {
    const int index = functionIndex(calleeName);
    if (index >= 0) {
      if (total != node.argsCount) {
        return failAt(diag::keywordArguments(), node);
      }
      for (std::size_t i = 0; i < total; ++i) {
        if (!expr(*node.children[1 + i])) return false;
      }
      emit("CALL", node);
      emitU16(index);
      emitU8(static_cast<int>(total));
      return true;
    }
  }

  // ── 裸名调用：可能是库注册的内建（`open`）──
  //
  // 走到这里说明这个名字既不是语言内建、也不是脚本内函数。它可能是一个
  // 库用 AIPY_PYLIB_BUILTIN 注册的短名（`open` → `fs.open`）。
  //
  // 这里**不能**判定「函数不存在」并拒绝：编译器按设计不认识具体能力，
  // 而库是可插拔的——同一个脚本在不同产物里可以链不同的库。所以编成
  // 按名字的扩展调用，由链接期决定它能不能解析；没链接进来时，报错落在
  // 运行期那一行，措辞与「扩展函数 text.upper 没有链接进来」一致。
  //
  // 代价是名字打错从编译期推迟到运行期。这个代价换来的是「新增裸名不用
  // 改编译器」，那正是「核心 VM 只认语法」这条约束的要求。
  if (func.kind == Kind::Name || selfCall) {
    if (total != node.argsCount) {
      return failAt(diag::keywordArguments(), node);
    }
    for (std::size_t i = 0; i < total; ++i) {
      if (!expr(*node.children[1 + i])) return false;
    }
    emit("CALL_EXT", node);
    emitU16(extensionIndex(calleeName));
    emitU8(static_cast<int>(total));
    return true;
  }

  // 扩展：模块.函数(...) / 方法调用。
  if (func.kind == Kind::Attribute) {
    if (total != node.argsCount) {
      return failAt(diag::keywordArguments(), node);
    }
    const Node& receiver = *func.children[0];

    // 接收者是扩展模块名时走 CALL_EXT；否则是方法调用。
    //
    // 模块名可以是点分的（`os.path.exists`）：这里取接收者的完整属性链
    // 名，再判断它是不是 `import` 过的模块。判据有两条：
    //   1. 精确匹配（`import os.path`）
    //   2. 点分边界上的前缀匹配（`import os` 之后写 `os.path.exists`）
    // 第 2 条是 CPython 的真实行为：`import os` 之后 `os.path` 就可用，
    // 不需要单独 import 子模块。
    const std::string receiverName = dottedName(receiver);
    if (!receiverName.empty()) {
      bool isModule = false;
      for (const std::string& module : imported_) {
        if (module == receiverName) {
          isModule = true;
          break;
        }
        // 前缀必须在点分边界上：`o` 不能匹配 `os.path`。
        if (receiverName.size() > module.size() &&
            receiverName.compare(0, module.size(), module) == 0 &&
            receiverName[module.size()] == '.') {
          isModule = true;
          break;
        }
      }
      if (isModule) {
        for (std::size_t i = 0; i < total; ++i) {
          if (!expr(*node.children[1 + i])) return false;
        }
        const std::string qualified = receiverName + "." + func.text;
        emit("CALL_EXT", node);
        emitU16(extensionIndex(qualified));
        emitU8(static_cast<int>(total));
        return true;
      }
    }

    // 压栈顺序：接收者在下、参数在上。
    if (!expr(receiver)) return false;
    for (std::size_t i = 0; i < total; ++i) {
      if (!expr(*node.children[1 + i])) return false;
    }
    emit("CALL_METHOD", node);
    emitU16(constIndexOfStr(func.text));
    emitU8(static_cast<int>(total));
    return true;
  }

  return failAt(diag::unsupportedCallTarget(), node);
}

bool CodeGen::constant(const Node& node) {
  switch (node.constKind) {
    case ConstKind::None:
      // intValue == -1 是 Ellipsis 的标记（None 用 0）。
      if (node.intValue == -1) {
        return failAt(diag::unsupportedLiteral("Ellipsis"), node);
      }
      emit("NONE", node);
      return true;

    case ConstKind::Bool:
      emit(node.intValue != 0 ? "TRUE" : "FALSE", node);
      return true;

    case ConstKind::Int:
      emit("CONST", node);
      emitU16(constIndexOfInt(parseIntLiteral(node.text)));
      return true;

    case ConstKind::Float: {
      if (node.flag) {
        // 复数：Python 的 %r 是 `1j` 这类。
        return failAt(diag::unsupportedLiteral(literalRepr(node)), node);
      }
      emit("CONST", node);
      emitU16(constIndexOfFloat(std::strtod(node.text.c_str(), nullptr)));
      return true;
    }

    case ConstKind::Str: {
      if (node.flag) {
        return failAt(diag::unsupportedLiteral(literalRepr(node)), node);
      }
      emit("CONST", node);
      emitU16(constIndexOfStr(node.text));
      return true;
    }
  }
  return failAt(diag::unsupportedLiteral(node.text), node);
}

}  // namespace pcc
