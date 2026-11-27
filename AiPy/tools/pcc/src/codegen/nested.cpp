// 嵌套函数（`def` 套 `def`）与 `nonlocal`。
//
// ── 为什么用「单元」而不是值拷贝 ──
//
// 直接捕获值（lambda 现在就是这样）只够只读场景。`nonlocal` 要求内层的写
// 能被外层看见，递归要求函数值能引用自己——两者都要求「同一个可变槽」。
// 所以被内层引用到的局部量改存成单元素列表 `[值]`：内层捕获这个**列表的
// 引用**，读写都走 `[0]`。列表已经有共享载荷语义，不必给 Value 加新类型。
//
// ── 什么时候建单元 ──
//
// 父函数里，一个局部名只要有任何一层内层函数读到它、或用 `nonlocal`
// 声明它，就要建单元。判定只看「内层读到了没有」，不做更细的逃逸分析：
// 值的捕获是最容易写对、也最容易对应的形态。
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "pcc/codegen/codegen.h"
#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

namespace pcc {
namespace {

bool isFn(const Node& node) {
  return node.kind == Kind::FunctionDef || node.kind == Kind::AsyncFunctionDef;
}

// 函数体里声明的 `nonlocal` 名字（不钻进更内层的函数）。
void collectNonlocalsOf(const Node& fn, std::set<std::string>* out) {
  std::deque<const Node*> queue;
  for (std::size_t i = 0; i < fn.bodyCount; ++i) {
    if (1 + i < fn.children.size() && fn.children[1 + i]) {
      queue.push_back(fn.children[1 + i].get());
    }
  }
  while (!queue.empty()) {
    const Node* current = queue.front();
    queue.pop_front();
    if (current->kind == Kind::Nonlocal) {
      for (const std::string& name : current->names) out->insert(name);
    }
    if (isFn(*current)) continue;
    for (const auto& child : current->children) {
      if (child) queue.push_back(child.get());
    }
  }
}

// 读到的名字（不钻进内层函数体与 lambda 体；但 `def` 的默认值与装饰器
// 在外层求值，要算进来）。
void collectReads(const Node& node, std::vector<std::string>* out) {
  std::deque<const Node*> queue;
  queue.push_back(&node);
  while (!queue.empty()) {
    const Node* current = queue.front();
    queue.pop_front();
    if (current->kind == Kind::Name) {
      bool seen = false;
      for (const std::string& existing : *out) {
        if (existing == current->text) seen = true;
      }
      if (!seen) out->push_back(current->text);
      continue;
    }
    if (current->kind == Kind::Lambda) continue;
    if (isFn(*current)) {
      if (!current->children.empty()) {
        const Node& args = *current->children[0];
        for (const auto& item : args.paramDefaults) {
          if (item) queue.push_back(item.get());
        }
        for (const auto& item : args.kwonlyDefaults) {
          if (item) queue.push_back(item.get());
        }
      }
      for (const auto& item : current->decorators) {
        if (item) queue.push_back(item.get());
      }
      continue;
    }
    for (const auto& child : current->children) {
      if (child) queue.push_back(child.get());
    }
  }
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
  for (const std::string& item : names) {
    if (item == name) return true;
  }
  return false;
}

}  // namespace

bool CodeGen::loadsCell(const std::string& name) const {
  if (block_ == &module_) return false;
  return contains(block_->cellNames, name) ||
         contains(block_->captureNames, name);
}

int CodeGen::cellSlot(const std::string& name) { return slotOf(name); }

void CodeGen::collectFreeNames(const Node& body, const Block& self,
                               const Block& parent,
                               std::vector<std::string>* out) const {
  // `nonlocal` 声明的名字一定是自由变量。
  std::set<std::string> nonlocals;
  collectNonlocalsOf(body, &nonlocals);
  for (const std::string& name : nonlocals) {
    if (contains(parent.localNames, name) && !contains(*out, name)) {
      out->push_back(name);
    }
  }

  std::vector<std::string> reads;
  for (std::size_t i = 0; i < body.bodyCount; ++i) {
    if (1 + i < body.children.size() && body.children[1 + i]) {
      collectReads(*body.children[1 + i], &reads);
    }
  }
  for (const std::string& name : reads) {
    if (Tables::isBuiltin(name)) continue;
    if (functionIndex(name) >= 0) continue;
    // 自己的形参/局部量遮蔽外层同名。
    if (contains(self.localNames, name)) continue;
    // 只有父作用域的局部量才谈得上「捕获」。
    if (!contains(parent.localNames, name)) continue;
    if (!contains(*out, name)) out->push_back(name);
  }
}

bool CodeGen::registerNestedFunctions(const Node& fnNode,
                                      const std::string& qualified,
                                      Block* parent) {
  for (std::size_t i = 0; i < fnNode.bodyCount; ++i) {
    const Node& child = *fnNode.children[1 + i];
    if (!isFn(child)) continue;

    const std::string name = qualified + "." + child.text;
    if (rawFunctionIndex(name) >= 0) {
      return failAt(diag::duplicateFunction(name), child);
    }

    Block block;
    std::uint64_t mask = 0;
    int defaultTotal = 0;
    if (!buildSignature(child, name, false, &block, &mask, &defaultTotal)) {
      return false;
    }
    if (defaultTotal > 0) {
      block.defaultSlot =
          globalSlot(std::string("\x01", 1) + "defaults:" + name);
    }
    if (child.decoratorCount > 0) decoratedNames_.insert(name);

    // 自由变量：先按签名登记好形参，再算捕获，这样形参遮蔽外层同名。
    std::vector<std::string> freeNames;
    collectFreeNames(child, block, *parent, &freeNames);
    block.captureNames = freeNames;
    if (!freeNames.empty()) {
      block.captureStart = block.localCount;
      for (const std::string& captured : freeNames) {
        slotOfIn(&block, captured);
      }
    }

    // 父函数里为这些名字建单元。
    for (const std::string& captured : freeNames) {
      if (!contains(parent->cellNames, captured)) {
        parent->cellNames.push_back(captured);
      }
    }

    FunctionEntry entry;
    entry.node = &child;
    entry.blockIndex = static_cast<int>(blocks_.size());
    entry.isMethod = false;

    std::set<std::string> declaredGlobal;
    collectGlobals(child, &declaredGlobal);
    // `nonlocal x` 的 x 是**外层**的变量，不是本函数的局部量。预扫描把它
    // 登记成局部会让它多占一个槽，且与捕获槽重名。
    std::set<std::string> declaredNonlocal;
    collectNonlocalsOf(child, &declaredNonlocal);
    std::vector<std::pair<std::string, const Node*>> assigned;
    collectAssigned(child, true, &assigned);
    for (const auto& item : assigned) {
      if (declaredGlobal.count(item.first) != 0) continue;
      if (declaredNonlocal.count(item.first) != 0) continue;
      slotOfIn(&block, item.first);
    }

    functionNames_.push_back(name);
    blocks_.push_back(std::move(block));
    functions_.push_back(std::move(entry));
    nestedFunctionNames_.push_back(name);

    if (!registerNestedFunctions(child, name, &blocks_.back())) return false;
  }
  return true;
}

bool CodeGen::emitFunctionValue(int index, const Node& node) {
  const Block& callee = blocks_[static_cast<std::size_t>(
      functions_[static_cast<std::size_t>(index)].blockIndex)];
  // 先压捕获值：被内层引用的是父函数的单元槽，取 cell 本身（不是 [0]）。
  for (const std::string& captured : callee.captureNames) {
    if (block_ != &module_ && (contains(block_->cellNames, captured) ||
                               contains(block_->captureNames, captured))) {
      emit("LOAD_LOCAL", node);
      emitU16(slotOf(captured));
    } else {
      nameRead(captured, node);
      // 值捕获：外面不是单元（比如从模块级捕获的常量），包一层单元素
      // 列表保持一致——内层按 cell 读，`[0]` 就是那个值。
      emit("BUILD_LIST", node);
      emitU16(1);
    }
  }
  emit("MAKE_FUNCTION", node);
  emitU16(index);
  emitU16(static_cast<int>(callee.captureNames.size()));
  return true;
}

bool CodeGen::nestedDefStatement(const Node& node) {
  // 函数表里的名字是 `<外层>.<本名>`；从当前块名推出来。
  const std::string qualified = block_->name + "." + node.text;
  const int index = rawFunctionIndex(qualified);
  if (index < 0) return failAt(diag::nestedFunction(node.text), node);

  const Block& callee = blocks_[static_cast<std::size_t>(
      functions_[static_cast<std::size_t>(index)].blockIndex)];

  // 默认值在**定义处**求值（CPython 语义），与顶层 def 完全一致：
  // 存进这个函数的隐藏全局槽 `\x01defaults:<名字>`。
  if (callee.defaultSlot >= 0) {
    if (!emitDefaultValuesFor(node, index)) return false;
  }

  if (!emitFunctionValue(index, node)) return false;
  nameStore(node.text, node);

  if (node.decoratorCount > 0) {
    // 装饰器就地应用：栈上是函数值，压装饰器再按值调用。
    for (std::size_t i = node.decoratorCount; i > 0; --i) {
      if (!expr(*node.decorators[i - 1])) return false;
      // [函数值, 装饰器] → 需要 [装饰器, 函数值]
      const int valueSlot = allocSlot();
      const int decoSlot = allocSlot();
      emit("STORE_LOCAL", node);
      emitU16(decoSlot);
      emit("STORE_LOCAL", node);
      emitU16(valueSlot);
      emit("LOAD_LOCAL", node);
      emitU16(decoSlot);
      emit("LOAD_LOCAL", node);
      emitU16(valueSlot);
      emit("CALL_VALUE", node);
      emitU8(1);
    }
    nameStore(node.text, node);
  }
  return true;
}

}  // namespace pcc
