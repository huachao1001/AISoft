// 表达式的代码生成，逐段对照 compiler.py 的 expr/slice/call/constant。
#include "pcc/codegen/codegen.h"

#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

#include "aipy/exceptions.h"
#include "aipy/types_names.h"

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
  // 同名变量优先于类型名：`int = 5` 之后再读 `int` 必须是 5，不是
  // `<class 'int'>`（CPython 里赋值遮蔽内建）。
  //
  // 判据分两种：模块级代码按**编译顺序**看已赋值的名字（`int = 5` 写在
  // 后面时前面的 `int` 仍是内建）；函数体里按模块级是否**声明过**——函数
  // 调用发生在模块执行完之后，那时全局名已经存在。
  const bool shadowed =
      isLocal(name) || (block_ == &module_ ? assignedSoFar_.count(name) != 0
                                          : globalsSet_.count(name) != 0);
  if (!shadowed && aipy::isBuiltinTypeName(name)) {
    emit("LOAD_TYPE", node);
    emitU16(constIndexOfStr(name));
    return true;
  }
  if (!shadowed && aipy::isExceptionClassName(name)) {
    // 异常类同样是值：`isinstance(e, ValueError)`、`raise` 之外的场合。
    emit("LOAD_TYPE", node);
    emitU16(constIndexOfStr(name));
    return true;
  }
  if (Tables::isBuiltin(name)) {
    // 内建名被赋值过时它就是一个普通变量，不再是「不能当值」的内建。
    if (shadowed) {
      nameTarget(name, node);
      return true;
    }
    return failAt(diag::builtinAsValue(name), node);
  }
  // 脚本函数出现在值的位置（`g = f`、`f(func)` 的实参）就是取函数值。
  // 编译成一条 MAKE_FUNCTION：函数号进载荷，运行期得到一个 kObject。
  if (const int index = functionIndex(name); index >= 0) {
    emit("MAKE_FUNCTION", node);
    emitU16(index);
    emitU16(0);  // 捕获个数。顶层函数不捕获，名字全在模块级全局槽里。
    return true;
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
      // 其它情况是真正的属性读：接收者求值后 GET_ATTR。
      // 编译期不知道接收者是实例、类还是 pylib 对象，一律按名字查——
      // 与 `a.b` 在 CPython 里「先看对象有没有这个属性」一致。
      if (!expr(*node.children[0])) return false;
      emit("GET_ATTR", node);
      emitU16(constIndexOfStr(node.text));
      return true;
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

    case Kind::Call: {
      const Node& callee = *node.children[0];
      // `super()`：当前方法所属的类 + self。Java 式的无参形式就够用，
      // 两个参数的形式（`super(C, self)`）在脚本里极罕见，暂不支持。
      if (callee.kind == Kind::Name && callee.text == "super" &&
          node.children.size() == 1) {
        if (currentClass_.empty()) {
          return failAt("super() is only allowed inside a class method", node);
        }
        emit("LOAD_LOCAL", node);
        emitU16(0);  // 方法的第一个形参就是 self
        emit("MAKE_SUPER", node);
        emitU16(constIndexOfStr(currentClass_));
        return true;
      }

      // `A(...)`：编译期知道 A 是脚本类（classDef 登记过）就发 INSTANTIATE。
      if (callee.kind == Kind::Name) {
        const auto it = classNames_.find(callee.text);
        if (it != classNames_.end()) {
          const std::size_t total = node.children.size() - 1;
          if (total != node.argsCount) {
            return failAt(diag::keywordArguments(), node);
          }
          nameRead(callee.text, callee);
          for (std::size_t i = 0; i < total; ++i) {
            if (!expr(*node.children[1 + i])) return false;
          }
          emit("INSTANTIATE", node);
          emitU8(static_cast<int>(total));
          return true;
        }
      }
      return call(node);
    }

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

    case Kind::Lambda: {
      // lambda 是一个值：登记隐藏函数，在这里求值它要捕获的外层变量，
      // 再 MAKE_FUNCTION 造出函数值。捕获必须在**这句的位置**求值，
      // 与默认参数同理——否则闭包看到的是定义之后才改的值。
      const int index = registerLambda(node);
      if (index < 0) return false;
      const Block& callee = blocks_[static_cast<std::size_t>(
          functions_[static_cast<std::size_t>(index)].blockIndex)];

      // 默认值在**定义处**求值（CPython 语义），与 def 完全一致：
      // 存进隐藏全局槽，调用时按掩码取。
      if (callee.defaultSlot >= 0) {
        if (!emitLambdaDefaults(node, callee)) return false;
      }

      std::vector<std::string> captures = callee.captures;
      for (const std::string& name : captures) {
        if (!nameRead(name, node)) return false;
      }
      emit("MAKE_FUNCTION", node);
      emitU16(index);
      emitU16(static_cast<int>(captures.size()));
      return true;
    }

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

    case Kind::Await: {
      // `await 协程`：求值协程对象，再让 VM 驱动它跑到底取返回值。
      if (!expr(*node.children[0])) return false;
      emit("AWAIT", node);
      return true;
    }

    case Kind::Yield: {
      // `yield` / `yield x`：求出值（没有操作数就是 None）后发 YIELD。
      // 这一帧会被标记成生成器（见 compileFunctionBody 的 yield 检测）。
      if (node.children.empty()) {
        emit("NONE", node);
      } else if (!expr(*node.children[0])) {
        return false;
      }
      emit("YIELD", node);
      return true;
    }

    case Kind::YieldFrom:
      return failAt("'yield from' is not supported", node);

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

bool CodeGen::callScript(const Node& node, int index) {
  // 脚本函数的签名在编译期完全已知，所以关键字参数在这里**重排**成位置
  // 顺序，VM 不需要认识关键字。`f(b=1, a=2)` 生成与 `f(2, 1)` 相同的代码。
  const Block& calleeBlock =
      blocks_[static_cast<std::size_t>(
          functions_[static_cast<std::size_t>(index)].blockIndex)];
  const std::vector<std::string>& params = calleeBlock.localNames;
  const int paramCount = calleeBlock.paramCount;
  const int defaultCount = calleeBlock.defaultCount;
  const int defaultSlot = calleeBlock.defaultSlot;

  // 形参里有 `*args` / `**kwargs` / 仅关键字参数，或实参里有 `*` / `**`
  // 展开时，实参的个数与名字要到运行期才知道，编译期拍平不了：改发
  // [位置实参列表, 关键字字典]，由 VM 按 CPython 规则绑定。
  bool needsDynamic = calleeBlock.varargSlot >= 0 ||
                      calleeBlock.kwargSlot >= 0 ||
                      calleeBlock.kwonlyStart >= 0;
  for (std::size_t i = 0; i < node.children.size() - 1; ++i) {
    if (node.children[1 + i]->kind == Kind::Starred) needsDynamic = true;
  }
  if (needsDynamic) return callDynamic(node, index);

  const std::size_t supplied = node.children.size() - 1;
  std::vector<const Node*> ordered(static_cast<std::size_t>(paramCount),
                                   nullptr);
  std::size_t positional = 0;
  std::size_t highest = 0;

  for (std::size_t i = 0; i < supplied; ++i) {
    const Node& argument = *node.children[1 + i];
    if (argument.kind == Kind::Keyword) {
      int slot = -1;
      for (int p = 0; p < paramCount; ++p) {
        if (params[static_cast<std::size_t>(p)] == argument.text) {
          slot = p;
          break;
        }
      }
      if (slot < 0) {
        return failAt(diag::unexpectedKeyword(node.children[0]->text,
                                             argument.text),
                      argument);
      }
      if (ordered[static_cast<std::size_t>(slot)] != nullptr) {
        return failAt(diag::duplicateKeyword(argument.text), argument);
      }
      ordered[static_cast<std::size_t>(slot)] = argument.children[0].get();
      highest = std::max(highest, static_cast<std::size_t>(slot) + 1);
      continue;
    }
    if (positional >= static_cast<std::size_t>(paramCount)) {
      return failAt(diag::tooManyArguments(node.children[0]->text), argument);
    }
    if (ordered[positional] != nullptr) {
      return failAt(diag::multipleValues(params[positional]), argument);
    }
    ordered[positional] = &argument;
    ++positional;
    highest = std::max(highest, positional);
  }

  // 中间的洞：有默认值就取默认值补上，否则是少传必需参数。
  for (std::size_t i = 0; i < highest; ++i) {
    if (ordered[i] != nullptr) continue;
    const bool hasDefault =
        i < 64 && (calleeBlock.defaultMask & (std::uint64_t(1) << i)) != 0;
    if (!hasDefault) {
      return failAt(diag::missingArgument(node.children[0]->text, params[i]),
                    node);
    }
  }

  // 按参数顺序求值。求值顺序与 CPython 不同（它按实参书写顺序），但这里
  // 的实参都是无副作用的常见形态；有副作用时差异已写入语言文档。
  for (std::size_t i = 0; i < highest; ++i) {
    if (ordered[i] != nullptr) {
      if (!expr(*ordered[i])) return false;
      continue;
    }
    // 洞有默认值：从隐藏槽按下标取这一个。不能紧凑传参——那会把后面的
    // 值灌进前面缺的那个参数。
    emit("LOAD_GLOBAL", node);
    emitU16(defaultSlot);
    emit("CONST", node);
    emitU16(constIndexOfInt(static_cast<long long>(i)));
    emit("GET_ITEM", node);
  }
  emit("CALL", node);
  emitU16(index);
  emitU8(static_cast<int>(highest));
  return true;
}

bool CodeGen::callDynamic(const Node& node, int index) {
  // 栈形状：[位置实参列表, 关键字字典]。
  //
  // 位置实参按书写顺序拼进一个列表。普通实参用 `BUILD_LIST 1` + `BIN ADD`
  // 追加；`*expr` 先过一遍内建 `list()` 再拼接——展开的对象是任意可迭代
  // 类型，`list + tuple` 在 CPython 里是 TypeError，归一成列表才与
  // 「展开后逐个变成位置实参」的语义一致。
  emit("BUILD_LIST", node);
  emitU16(0);

  for (std::size_t i = 0; i < node.children.size() - 1; ++i) {
    const Node& argument = *node.children[1 + i];
    if (argument.kind == Kind::Keyword) continue;
    if (argument.kind == Kind::Starred) {
      // `**d` 是关键字展开，不是位置展开；它在下半段处理。
      if (argument.flag) continue;
      if (!expr(*argument.children[0])) return false;
      emit("CALL_BUILTIN", argument);
      emitU16(builtinIndex("list"));
      emitU8(1);
      emit("BIN", argument);
      emitU8(0);  // BinOp::kAdd
      continue;
    }
    if (!expr(argument)) return false;
    emit("BUILD_LIST", argument);
    emitU16(1);
    emit("BIN", argument);
    emitU8(0);  // BinOp::kAdd
  }

  // 关键字：`name=value` 直接进字典，`**expr` 合并进来。
  emit("BUILD_DICT", node);
  emitU16(0);
  for (std::size_t i = 0; i < node.children.size() - 1; ++i) {
    const Node& argument = *node.children[1 + i];
    if (argument.kind == Kind::Keyword) {
      emit("DUP", argument);
      emit("CONST", argument);
      emitU16(constIndexOfStr(argument.text));
      if (!expr(*argument.children[0])) return false;
      emit("SET_ITEM", argument);
      continue;
    }
    if (argument.kind != Kind::Starred || !argument.flag) continue;  // `*` 不是 `**`
    // `**expr`：把字典并进关键字字典。栈上是 [关键字字典, 展开值]。
    if (!expr(*argument.children[0])) return false;
    emit("DICT_MERGE", argument);
  }

  emit("CALL_DYN", node);
  emitU16(index);
  return true;
}

bool CodeGen::callValueArgs(const Node& node) {
  // 通用「按值调用」的实参编码：
  //   没有 `*`/`**` → 直接压实参，CALL_VALUE u8
  //   有 `*`/`**`   → 压 [位置列表, 关键字字典]，CALL_VALUE_DYN
  // 调用方负责先把「可调用值」压在栈底。
  bool dynamic = false;
  for (std::size_t i = 0; i < node.children.size() - 1; ++i) {
    if (node.children[1 + i]->kind == Kind::Starred) dynamic = true;
    if (node.children[1 + i]->kind == Kind::Keyword) dynamic = true;
  }

  const std::size_t total = node.children.size() - 1;
  if (!dynamic) {
    for (std::size_t i = 0; i < total; ++i) {
      if (!expr(*node.children[1 + i])) return false;
    }
    emit("CALL_VALUE", node);
    emitU8(static_cast<int>(total));
    return true;
  }

  emit("BUILD_LIST", node);
  emitU16(0);
  for (std::size_t i = 0; i < total; ++i) {
    const Node& argument = *node.children[1 + i];
    if (argument.kind == Kind::Keyword) continue;
    if (argument.kind == Kind::Starred) {
      if (argument.flag) continue;  // `**` 走下半段
      if (!expr(*argument.children[0])) return false;
      emit("CALL_BUILTIN", argument);
      emitU16(builtinIndex("list"));
      emitU8(1);
      emit("BIN", argument);
      emitU8(0);  // BinOp::kAdd
      continue;
    }
    if (!expr(argument)) return false;
    emit("BUILD_LIST", argument);
    emitU16(1);
    emit("BIN", argument);
    emitU8(0);
  }

  emit("BUILD_DICT", node);
  emitU16(0);
  for (std::size_t i = 0; i < total; ++i) {
    const Node& argument = *node.children[1 + i];
    if (argument.kind == Kind::Keyword) {
      emit("DUP", argument);
      emit("CONST", argument);
      emitU16(constIndexOfStr(argument.text));
      if (!expr(*argument.children[0])) return false;
      emit("SET_ITEM", argument);
      continue;
    }
    if (argument.kind != Kind::Starred || !argument.flag) continue;
    if (!expr(*argument.children[0])) return false;
    emit("DICT_MERGE", argument);
  }

  emit("CALL_VALUE_DYN", node);
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
    // 关键字实参：`print(sep="-")`、`sorted(reverse=True)`。内建签名没有
    // 形参名，所以另发 CALL_BUILTIN_KW，由 VM 按 builtins_keywords.cpp
    // 的表把名字换成位置。
    std::size_t keywordCount = 0;
    for (std::size_t i = 0; i < total; ++i) {
      if (node.children[1 + i]->kind == Kind::Keyword) ++keywordCount;
    }
    if (keywordCount == 0) {
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

    // 位置实参先压（保持源码顺序），再按源码顺序压 [键, 值] 对。
    const std::size_t positional = total - keywordCount;
    if (positional != node.argsCount) {
      // `f(a=1, *args)` 这种混合形式暂不支持；位置个数对不上就报错。
      return failAt(diag::keywordArguments(), node);
    }
    for (std::size_t i = 0; i < total; ++i) {
      const Node& argument = *node.children[1 + i];
      if (argument.kind == Kind::Keyword) continue;
      if (!expr(argument)) return false;
    }
    for (std::size_t i = 0; i < total; ++i) {
      const Node& argument = *node.children[1 + i];
      if (argument.kind != Kind::Keyword) continue;
      emit("CONST", argument);
      emitU16(constIndexOfStr(argument.text));
      if (!expr(*argument.children[0])) return false;
    }
    emit("CALL_BUILTIN_KW", node);
    emitU16(builtinIndex(calleeName));
    emitU8(static_cast<int>(positional));
    emitU8(static_cast<int>(keywordCount));
    return true;
  }

  // 脚本内函数：CALL + 函数号。
  if (func.kind == Kind::Name || selfCall) {
    const int index = functionIndex(calleeName);
    if (index >= 0) {
      return callScript(node, index);
    }
  }

  // ── 调用一个变量：`g(1)`，其中 g 是存着函数值的变量 ──
  //
  // 名字既不是内建也不是脚本函数，但它是**已知的变量**（局部或全局槽），
  // 那就只可能是运行期存进去的函数值。编成「先读值、再按值调用」。
  // 放在裸名分支之前：否则会被当成扩展函数，运行期报「没有链接进来」。
  if (func.kind == Kind::Name || selfCall) {
    if (isLocal(calleeName) || globalsSet_.count(calleeName) != 0) {
      nameTarget(calleeName, func);
      return callValueArgs(node);
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

  // 其它任何表达式当调用目标：`f()(1)`、`(lambda x: x)(1)`。
  // 先求值拿到函数值，再按值调用——与「调用变量」同一套指令。
  if (!expr(func)) return false;
  return callValueArgs(node);
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

    case ConstKind::Bytes: {
      emit("CONST", node);
      emitU16(constIndexOfBytes(node.text));
      return true;
    }
  }
  return failAt(diag::unsupportedLiteral(node.text), node);
}

}  // namespace pcc
