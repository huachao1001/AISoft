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
    out.defaultCount = block.defaultCount;
    out.defaultSlot = block.defaultSlot;
    out.defaultMask = block.defaultMask;
    out.varargSlot = block.varargSlot;
    out.kwargSlot = block.kwargSlot;
    out.kwonlyStart = block.kwonlyStart;
    out.captureStart = block.captureStart;
    out.paramNames.assign(block.localNames.begin(),
                          block.localNames.begin() +
                              static_cast<std::ptrdiff_t>(block.paramCount));
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

  // 默认值在**定义处**求值（CPython 语义）：它的求值代码必须出现在
  // def 语句本身的位置，而不是模块级语句之前——否则 `def f(a=n)` 里
  // 的 n 会在 n 赋值之前被求值，拿到 None。实际发射在 statement() 里。

  // 模块级语句：按源码顺序执行。FunctionDef 也要走 statement()——它会在
  // 定义处求值默认参数（CPython 语义），跳过就等于默认值永远是 None。
  for (const Node* node : body) {
    if (!statement(*node)) return false;
  }

  // lambda 的函数体在模块级语句编完之后才编：它们的登记发生在语句内部，
  // 而函数体里引用的名字要等所有全局槽都占好。
  if (!compilePendingLambdas()) return false;

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

bool CodeGen::buildSignature(const Node& node, const std::string& name,
                             bool isMethod, Block* out,
                             std::uint64_t* mask, int* defaultTotal) {
  const Node& args = *node.children[0];
  if (args.hasPosonly) {
    return fail(diag::positionalOnlyParams(), args.posonlyLine,
                args.posonlyCol + 1);
  }

  const std::size_t firstParam = isMethod ? 1 : 0;

  // 槽序：位置参数 → 仅关键字参数 → *args → **kwargs。
  //
  // 前 paramCount 个必须是**具名参数**且连续（位置段 + 仅关键字段），VM 的
  // 绑定逻辑靠这个区间按下标匹配。vararg/kwarg 是具名参数之外的局部量，
  // 只能排在它们后面——这一点与 CPython 的 co_varnames 顺序不同，但脚本
  // 观察不到槽号，只有「名字能读到正确值」这一条约束。
  Block block;
  block.name = name;
  for (std::size_t i = firstParam; i < args.params.size(); ++i) {
    block.localNames.push_back(args.params[i]);
  }
  block.paramCount = static_cast<int>(block.localNames.size());
  block.localCount = block.paramCount;
  if (!args.kwonlyParams.empty()) {
    block.kwonlyStart = block.paramCount;
    for (const std::string& parameter : args.kwonlyParams) {
      block.localNames.push_back(parameter);
      ++block.paramCount;
      ++block.localCount;
    }
  }
  if (args.hasVararg) {
    block.varargSlot = block.localCount++;
    block.localNames.push_back(args.varargName);
  }
  if (args.hasKwarg) {
    block.kwargSlot = block.localCount++;
    block.localNames.push_back(args.kwargName);
  }

  // 默认值：每个具名参数一位，位在参数下标上。这样 `def f(*, a=1, b)`
  // 这种默认值不连续的情况也能精确表达；列表本身与 paramCount 等长，
  // 没有默认值的位置留占位（None 是合法默认值，不能拿它表示「没有」，
  // 真正的判据是掩码位）。
  std::uint64_t bits = 0;
  int total = 0;
  for (std::size_t i = firstParam; i < args.paramDefaults.size(); ++i) {
    if (!args.paramDefaults[i]) continue;
    const std::size_t slot = i - firstParam;
    if (slot < 64) bits |= (std::uint64_t(1) << slot);
    ++total;
  }
  for (std::size_t i = 0; i < args.kwonlyDefaults.size(); ++i) {
    if (!args.kwonlyDefaults[i]) continue;
    const std::size_t slot = static_cast<std::size_t>(block.kwonlyStart) + i;
    if (slot < 64) bits |= (std::uint64_t(1) << slot);
    ++total;
  }
  block.defaultMask = bits;
  block.defaultCount = total;
  *out = std::move(block);
  *mask = bits;
  *defaultTotal = total;
  return true;
}

bool CodeGen::registerFunction(const Node& node) {
  if (rawFunctionIndex(node.text) >= 0) {
    return failAt(diag::duplicateFunction(node.text), node);
  }
  // 带装饰器的名字运行期装的是装饰器返回值，按名调用要走 CALL_VALUE。
  if (node.decoratorCount > 0) decoratedNames_.insert(node.text);

  // 方法（在 Activity 里）去掉第一个 self 参数。
  const Node& args = *node.children[0];
  const bool isMethod = !args.params.empty() && args.params[0] == "self";

  Block block;
  std::uint64_t mask = 0;
  int defaultTotal = 0;
  if (!buildSignature(node, node.text, isMethod, &block, &mask,
                      &defaultTotal)) {
    return false;
  }
  if (defaultTotal > 0) {
    // 默认值在定义处求值后存进这个隐藏全局槽。名字带前缀，避免与脚本变量
    // 撞名；它同时出现在 globalNames 里，热加载会把值一并搬过去。
    block.defaultSlot = globalSlot(std::string("\x01", 1) + "defaults:" + node.text);
  }

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

int CodeGen::registerLambda(const Node& node) {
  // lambda 没有名字。函数表里需要一个唯一的名字只用于报错，用 <lambda>
  // 加序号；脚本看不到它。
  const std::string name =
      "<lambda>" + std::to_string(lambdaCounter_++);

  Block block;
  std::uint64_t mask = 0;
  int defaultTotal = 0;
  if (!buildSignature(node, name, false, &block, &mask, &defaultTotal)) {
    return -1;
  }
  if (defaultTotal > 0) {
    block.defaultSlot = globalSlot(std::string("\x01", 1) + "defaults:" + name);
  }

  // lambda 的体是一个表达式，里面的名字都是**捕获**：它们要么来自外层
  // 函数（闭包），要么来自模块级。这里没法静态分出哪一种，所以统一按
  // 捕获处理——调用时由 VM 把外层局部值填进捕获槽。
  //
  // 捕获顺序按首次出现排，去重。
  std::vector<std::string> captures;
  collectLambdaCaptures(*node.children[1], block.localNames, &captures);
  block.captures = captures;
  if (!captures.empty()) {
    // 捕获槽紧跟在参数区之后，按 captures 的顺序排列。VM 靠 captureStart
    // 把 MAKE_FUNCTION 压栈的值填进这些槽。
    block.captureStart = block.localCount;
    for (const std::string& captured : captures) {
      slotOfIn(&block, captured);
    }
  }

  FunctionEntry entry;
  entry.node = &node;
  entry.blockIndex = static_cast<int>(blocks_.size());
  entry.isMethod = false;
  functionNames_.push_back(name);
  blocks_.push_back(std::move(block));
  functions_.push_back(std::move(entry));
  pendingLambdas_.push_back(static_cast<int>(functions_.size()) - 1);
  return static_cast<int>(functions_.size()) - 1;
}

void CodeGen::collectLambdaCaptures(
    const Node& body, const std::vector<std::string>& params,
    std::vector<std::string>* out) const {
  // lambda 体里出现的每个名字：不是自己的形参、不是内建、不是脚本函数，
  // 就需要从外层捕获。用后序遍历保证嵌套表达式的顺序稳定。
  std::deque<const Node*> queue;
  queue.push_back(&body);
  while (!queue.empty()) {
    const Node* current = queue.front();
    queue.pop_front();
    if (current->kind == Kind::Name) {
      const std::string& name = current->text;
      if (Tables::isBuiltin(name)) continue;
      if (functionIndex(name) >= 0) continue;
      bool isParam = false;
      for (const std::string& parameter : params) {
        if (parameter == name) {
          isParam = true;
          break;
        }
      }
      if (isParam) continue;
      bool seen = false;
      for (const std::string& existing : *out) {
        if (existing == name) {
          seen = true;
          break;
        }
      }
      if (!seen) out->push_back(name);
      continue;
    }
    for (const auto& child : current->children) {
      if (child) queue.push_back(child.get());
    }
  }
}

bool CodeGen::compilePendingLambdas() {
  for (const int index : pendingLambdas_) {
    FunctionEntry& entry = functions_[static_cast<std::size_t>(index)];
    Block& block = blocks_[static_cast<std::size_t>(entry.blockIndex)];
    const Node& node = *entry.node;

    Block* savedBlock = block_;
    std::vector<LoopFrame> savedLoops = loopStack_;
    std::vector<int> savedWiths = withStack_;
    const bool savedInMethod = inMethod_;
    block_ = &block;
    loopStack_.clear();
    withStack_.clear();
    inMethod_ = false;

    bool ok = true;
    if (node.children.size() > 1) ok = expr(*node.children[1]);
    if (ok) {
      emit("RETURN", node);
    }

    inMethod_ = savedInMethod;
    withStack_ = savedWiths;
    loopStack_ = savedLoops;
    block_ = savedBlock;
    if (!ok) return false;
  }
  return true;
}

bool CodeGen::emitDefaultValues(const Node& node, Block& block) {
  (void)block;
  // CPython 在**执行到 def 这一句时**求值默认参数。所以这段代码必须发在
  // def 语句自己的位置，而不能提到模块级之前。
  const Node& args = *node.children[0];
  const int index = rawFunctionIndex(node.text);
  if (index < 0) return failAt(diag::nestedFunction(node.text), node);
  const Block& target =
      blocks_[static_cast<std::size_t>(
          functions_[static_cast<std::size_t>(index)].blockIndex)];
  if (target.defaultSlot < 0) return true;

  const bool isMethod = !args.params.empty() && args.params[0] == "self";
  const std::size_t firstParam = isMethod ? 1 : 0;

  // 列表与 paramCount 等长，按参数下标直接放默认值。没有默认值的位置只是
  // 占位（None）——函数表里的 defaultMask 告诉 VM 哪些位置是真的，占位值
  // 永远不会被读到。用等长列表而不是「紧凑尾部」，是为了让仅关键字参数
  // `def f(*, a=1, b)` 这种不连续的默认值也能按下标找到。
  for (int i = 0; i < target.paramCount; ++i) emit("NONE", node);
  emit("BUILD_LIST", node);
  emitU16(target.paramCount);

  // 默认值列表与 paramCount 等长，下标就是去掉 self 之后的参数下标。
  // `paramDefaults` 与 args.params 平行，所以有默认值的位置参数在去掉
  // self 之后的下标是 i - firstParam。
  for (std::size_t i = firstParam; i < args.paramDefaults.size(); ++i) {
    if (!args.paramDefaults[i]) continue;
    emit("DUP", node);
    emit("CONST", node);
    emitU16(constIndexOfInt(static_cast<long long>(i - firstParam)));
    if (!expr(*args.paramDefaults[i])) return false;
    emit("SET_ITEM", node);
  }
  for (std::size_t i = 0; i < args.kwonlyDefaults.size(); ++i) {
    if (!args.kwonlyDefaults[i]) continue;
    emit("DUP", node);
    emit("CONST", node);
    emitU16(constIndexOfInt(static_cast<long long>(target.kwonlyStart) +
                            static_cast<long long>(i)));
    if (!expr(*args.kwonlyDefaults[i])) return false;
    emit("SET_ITEM", node);
  }
  emit("STORE_GLOBAL", node);
  emitU16(target.defaultSlot);
  return true;
}

bool CodeGen::emitLambdaDefaults(const Node& node, const Block& callee) {
  // 与 emitDefaultValues 同构，区别只是节点形状：lambda 的参数在
  // children[0]、体在 children[1]，没有 bodyCount。
  const Node& args = *node.children[0];
  for (int i = 0; i < callee.paramCount; ++i) emit("NONE", node);
  emit("BUILD_LIST", node);
  emitU16(callee.paramCount);

  const int base = static_cast<int>(args.params.size()) -
                   static_cast<int>(args.paramDefaults.size());
  for (std::size_t i = 0; i < args.paramDefaults.size(); ++i) {
    if (!args.paramDefaults[i]) continue;
    emit("DUP", node);
    emit("CONST", node);
    emitU16(constIndexOfInt(static_cast<long long>(base) +
                            static_cast<long long>(i)));
    if (!expr(*args.paramDefaults[i])) return false;
    emit("SET_ITEM", node);
  }
  for (std::size_t i = 0; i < args.kwonlyDefaults.size(); ++i) {
    if (!args.kwonlyDefaults[i]) continue;
    emit("DUP", node);
    emit("CONST", node);
    emitU16(constIndexOfInt(static_cast<long long>(callee.kwonlyStart) +
                            static_cast<long long>(i)));
    if (!expr(*args.kwonlyDefaults[i])) return false;
    emit("SET_ITEM", node);
  }
  emit("STORE_GLOBAL", node);
  emitU16(callee.defaultSlot);
  return true;
}

bool CodeGen::applyDecorators(const Node& node) {
  // `@a @b def f` 等价于 `f = a(b(f))`：最靠近 def 的装饰器先应用。
  //
  // 每轮都要拿到「当前值」，所以先造出函数值放进一个隐藏全局槽，然后
  // 从最内层装饰器开始逐层调用，结果写回同一个槽。槽名带前缀，避免与
  // 脚本变量撞名；它也进 globalNames，热加载会一并搬。
  const int funcIndex = rawFunctionIndex(node.text);
  if (funcIndex < 0) return failAt(diag::nestedFunction(node.text), node);
  const int slot = globalSlot(std::string("\x01", 1) + "decorated:" + node.text);

  emit("MAKE_FUNCTION", node);
  emitU16(funcIndex);
  emitU16(0);
  emit("STORE_GLOBAL", node);
  emitU16(slot);

  // 逆序：decorators[0] 是最外层（最靠近 @ 的第一行），最后应用。
  for (std::size_t i = node.decorators.size(); i > 0; --i) {
    const Node& decorator = *node.decorators[i - 1];
    if (!expr(decorator)) return false;      // 装饰器可调用值
    emit("LOAD_GLOBAL", node);
    emitU16(slot);                            // 当前值作实参
    emit("CALL_VALUE", node);
    emitU8(1);
    emit("STORE_GLOBAL", node);
    emitU16(slot);
  }

  // 最终值落进脚本里的函数名。
  emit("LOAD_GLOBAL", node);
  emitU16(slot);
  nameStore(node.text, node);
  return true;
}

bool CodeGen::compileFunctionBody(const Node& node) {
  const int index = rawFunctionIndex(node.text);
  if (index < 0) return failAt(diag::duplicateFunction(node.text), node);

  FunctionEntry& entry = functions_[static_cast<std::size_t>(index)];
  Block& block = blocks_[static_cast<std::size_t>(entry.blockIndex)];

  std::set<std::string> declaredGlobal;
  collectGlobals(node, &declaredGlobal);

  const std::set<std::string>& selfAssigned = entry.selfAssigned;

  // 形参占前 paramCount 个槽（位置参数 + 仅关键字参数）；*args / **kwargs
  // 紧跟在后面。它们都是调用时绑定的局部量，不能因为预扫描登记过就豁免
  // 「漏写 global」的检查——但**其它**局部量也不该豁免，所以这里只收形参。
  std::set<std::string> params;
  for (int slot = 0; slot < block.paramCount; ++slot) {
    params.insert(block.localNames[static_cast<std::size_t>(slot)]);
  }
  if (block.varargSlot >= 0) {
    params.insert(block.localNames[static_cast<std::size_t>(block.varargSlot)]);
  }
  if (block.kwargSlot >= 0) {
    params.insert(block.localNames[static_cast<std::size_t>(block.kwargSlot)]);
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
