// 模块级编译：Activity 展开、函数登记、函数体编译、收尾。
//
// ── 为什么分两遍 ──
//
// Python 允许「先调用后定义」。模块级代码按顺序执行，但函数必须**先全部
// 登记**（签名占位）再编译函数体——否则 `a()` 调用 `b()` 时 b 还没在表里，
// 会被当成「不支持这种调用目标」。
#include "pcc/codegen/codegen.h"

#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

#include <deque>

namespace pcc {

bool CodeGen::compile(const Node& module) {
  // `failed_` 兜底：算子映射这类返回编号的地方没法用返回值传达失败
  // （Python 版是抛异常，任何深度都能立刻中断）。这里统一在收尾处拦一次，
  // 效果等价——反正失败时代码不会被写出。
  if (!compileModule(module) || failed_) return false;

  // 收尾：把内部状态搬进 Program。
  program_.moduleCode = module_.code;
  program_.moduleLines = module_.lines;
  program_.moduleLocalCount = module_.localCount;
  program_.globals = globalOrder_;
  program_.globalCount = static_cast<int>(globalOrder_.size());
  program_.consts = consts_;
  program_.imports = imported_;
  program_.extensions = extensions_;
  program_.builtins = usedBuiltins_;

  for (const Block& block : blocks_) {
    CompiledBlock out;
    out.name = block.name;
    out.code = block.code;
    out.lines = block.lines;
    out.paramCount = block.paramCount;
    out.localCount = block.localCount;
    program_.functions.push_back(std::move(out));
  }
  return true;
}

bool CodeGen::compileModule(const Node& module) {
  // 找出唯一允许的 Activity 类。
  const Node* activity = nullptr;
  for (const auto& child : module.children) {
    if (child->kind == Kind::ClassDef) {
      if (activity != nullptr) return failAt(diag::multipleActivity(), *child);
      activity = child.get();
    }
  }

  // 展开后的模块级语句序列：Activity 的字段与方法在前，类外语句在后
  // （与 Python 版 expand_activity 的拼接顺序一致）。
  program_.hasActivity = activity != nullptr;
  std::vector<const Node*> body;
  if (activity != nullptr) {
    if (!expandActivity(*activity, &body)) return false;
    for (const auto& child : module.children) {
      if (child.get() != activity) body.push_back(child.get());
    }
  } else {
    for (const auto& child : module.children) body.push_back(child.get());
  }

  // 先把所有顶层 def 收进来。
  for (const Node* node : body) {
    if (node->kind == Kind::FunctionDef) {
      if (!registerFunction(*node)) return false;
    }
  }

  // 模块级变量先全部占好槽：函数里读模块级变量要能找到它。
  for (const Node* node : body) {
    for (const std::string& name : assignedNames(*node, false)) {
      globalSlot(name);
    }
  }

  // import 先全部登记：函数体里 `threading.current_thread()` 要靠 imported_
  // 才认得出「这是扩展调用而不是方法调用」。Python 版这里用 ast.walk 扫
  // 整棵树（包括函数体里的 import），所以函数内写 import 也能生效。
  {
    auto visit = [this](const Node& current) {
      if (current.kind == Kind::Import || current.kind == Kind::ImportFrom) {
        importStatement(current);
      }
    };
    std::deque<const Node*> queue;
    queue.push_back(&module);
    while (!queue.empty()) {
      const Node* current = queue.front();
      queue.pop_front();
      visit(*current);
      for (const auto& child : current->children) {
        if (child) queue.push_back(child.get());
      }
    }
  }

  // 函数体最后编译：这时所有函数名与全局槽都已就位。
  for (const Node* node : body) {
    if (node->kind == Kind::FunctionDef) {
      if (!compileFunctionBody(*node)) return false;
    }
  }

  // 模块级语句：按源码顺序执行。
  for (const Node* node : body) {
    if (node->kind == Kind::FunctionDef) continue;
    if (!statement(*node)) return false;
  }

  // 模块级最后一条语句结束 → 隐式 return None。
  // 行号 0：Python 版这里传的是 ast.Module，它没有 lineno。
  emitOpNoLine("NONE");
  emitOpNoLine("RETURN");
  return true;
}

bool CodeGen::expandActivity(const Node& classNode,
                             std::vector<const Node*>* body) {
  bool hasActivity = false;
  for (std::size_t i = 0; i < classNode.argsCount; ++i) {
    const Node& base = *classNode.children[i];
    if (base.kind == Kind::Name && base.text == "Activity") hasActivity = true;
  }
  if (!hasActivity) return failAt(diag::classMustDeriveActivity(), classNode);

  // 装饰器先于类体检查（Python 版也是这个顺序）。
  if (classNode.decoratorCount > 0) {
    return failAt(diag::decoratorUnsupported(), classNode);
  }

  // 类体里的赋值 → 模块级赋值
  std::vector<const Node*> methods;
  for (std::size_t i = 0; i < classNode.bodyCount; ++i) {
    const Node& statementNode = *classNode.children[classNode.argsCount + i];
    switch (statementNode.kind) {
      case Kind::Assign:
      case Kind::AnnAssign:
        body->push_back(&statementNode);
        break;
      case Kind::FunctionDef:
        methods.push_back(&statementNode);
        break;
      case Kind::Pass:
        break;
      default:
        return failAt(diag::activityBodyOnlyFieldsAndMethods(), statementNode);
    }
  }
  // 方法统一放在字段之后（Python 版也是先扫一遍字段再扫一遍方法）。
  // 首参必须是 self：展开后 self 会被去掉，没有 self 的方法展开后参数
  // 会整体错位，症状是「调用时少/多一个实参」。
  for (const Node* method : methods) {
    const Node& args = *method->children[0];
    if (args.params.empty() || args.params[0] != "self") {
      return failAt(diag::methodSelfRequired(method->text), *method);
    }
    body->push_back(method);
  }
  return true;
}

bool CodeGen::registerFunction(const Node& node) {
  if (functionIndex(node.text) >= 0) {
    return failAt(diag::duplicateFunction(node.text), node);
  }
  if (node.decoratorCount > 0) {
    return failAt(diag::decoratorUnsupported(), node);
  }

  const Node& args = *node.children[0];
  if (args.hasVararg || args.hasKwarg || args.hasKwonly) {
    return failAt(diag::variadicParams(), node);
  }
  if (args.hasPosonly) {
    return fail(diag::positionalOnlyParams(), args.posonlyLine,
                args.posonlyCol + 1);
  }
  if (args.defaults > 0) {
    return fail(diag::defaultParams(), args.defaultLine,
                args.defaultCol + 1);
  }

  // 方法（在 Activity 里）去掉第一个 self 参数。
  const bool isMethod = !args.params.empty() && args.params[0] == "self";

  Block block;
  block.name = node.text;
  for (std::size_t i = isMethod ? 1 : 0; i < args.params.size(); ++i) {
    block.localNames.push_back(args.params[i]);
  }
  block.paramCount = static_cast<int>(block.localNames.size());
  block.localCount = block.paramCount;

  FunctionEntry entry;
  entry.node = &node;
  entry.blockIndex = static_cast<int>(blocks_.size());
  entry.isMethod = isMethod;

  // 声明了 `global X` 的名字不算局部——那是 Python 的作用域规则本身。
  std::set<std::string> declaredGlobal;
  collectGlobals(node, &declaredGlobal);

  // `self.x = ...` 展开来的名字也不算：它们是 Activity 的字段（模块级状态）。
  std::set<std::string> selfAssigned;
  collectSelfAssigned(node, &selfAssigned);
  entry.selfAssigned = selfAssigned;

  // 预扫描函数体里「会被赋值」的名字，先占好局部槽。见 scope.cpp 的解释。
  std::vector<std::pair<std::string, const Node*>> assigned;
  collectAssigned(node, true, &assigned);
  for (const auto& item : assigned) {
    const std::string& name = item.first;
    if (declaredGlobal.count(name) != 0) continue;
    if (selfAssigned.count(name) != 0) continue;
    slotOfIn(&block, name);
  }

  functionNames_.push_back(node.text);
  blocks_.push_back(std::move(block));
  functions_.push_back(std::move(entry));
  return true;
}

bool CodeGen::compileFunctionBody(const Node& node) {
  const int index = functionIndex(node.text);
  if (index < 0) return failAt(diag::duplicateFunction(node.text), node);

  FunctionEntry& entry = functions_[static_cast<std::size_t>(index)];
  Block& block = blocks_[static_cast<std::size_t>(entry.blockIndex)];

  std::set<std::string> declaredGlobal;
  collectGlobals(node, &declaredGlobal);

  const std::set<std::string>& selfAssigned = entry.selfAssigned;

  // 参数占前 paramCount 个槽，它们是真正的局部量。
  std::set<std::string> params;
  for (int slot = 0; slot < block.paramCount; ++slot) {
    params.insert(block.localNames[static_cast<std::size_t>(slot)]);
  }

  // 函数里对模块级的赋值必须写 global：否则按 CPython 就是新建局部量，
  // 而那几乎总是笔误。例外是通过 `self.x = ...` 来的名字。
  std::vector<std::pair<std::string, const Node*>> assigned;
  collectAssigned(node, true, &assigned);
  for (const auto& item : assigned) {
    const std::string& name = item.first;
    if (selfAssigned.count(name) != 0) continue;
    // 参数才是真正的局部量；不能因为预扫描登记过就豁免。
    if (globalsSet_.count(name) != 0 && declaredGlobal.count(name) == 0 &&
        params.count(name) == 0) {
      return failAt(diag::missingGlobal(name), *item.second);
    }
  }

  // 进入函数上下文。
  Block* savedBlock = block_;
  std::vector<LoopFrame> savedLoops = loopStack_;
  std::vector<int> savedWiths = withStack_;
  const bool savedInMethod = inMethod_;
  block_ = &block;
  loopStack_.clear();
  withStack_.clear();
  inMethod_ = entry.isMethod;

  bool ok = true;
  for (std::size_t i = 0; i < node.bodyCount && ok; ++i) {
    ok = statement(*node.children[1 + i]);
  }

  if (ok) {
    // 隐式 return None。必须在这里发——下面会把 block_ 恢复成模块块，
    // 之后再 emit 就会落到模块代码里（Python 版用 `block.emit` 显式指定，
    // 这里靠顺序保证同一件事）。
    emit("NONE", node);
    emit("RETURN", node);
  }

  inMethod_ = savedInMethod;
  withStack_ = savedWiths;
  loopStack_ = savedLoops;
  block_ = savedBlock;
  return ok;
}

}  // namespace pcc
