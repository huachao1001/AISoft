// 作用域分析：谁是局部量、谁进全局槽、谁是从 `self.x` 展开来的。
//
// ── 这里最容易出的错 ──
//
// `nameStore` 先问 `isLocal`，而局部量要到第一次赋值时才登记——那一刻
// 已经判成全局，于是函数内的临时量变成进程共享的全局槽。单线程下看起来
// 正常（同名变量只被一个函数用），多线程或递归立刻互相踩踏：探针里
// `def fib` 的 left/right/total 全进了 globals，四个线程共享 subtotal
// 就把和算成 3865 而不是 4950。
//
// 所以 registerFunction 必须**先预扫描**函数体里的赋值目标并占好槽。
//
// ── 遍历顺序必须与 ast.walk 相同 ──
//
// Python 版用 `ast.walk` 收集赋值目标，它是**广度优先**（deque + extend）。
// 槽号按首次出现顺序分配，换个遍历顺序就换一套槽号，字节码随之改变。
// 所以这里也写成 BFS，不能「顺手」改成递归。
#include "pcc/codegen/codegen.h"

#include <deque>
#include <string>
#include <vector>

namespace pcc {
namespace {

// ast.walk 的等价物：先进先出，孩子按字段顺序入队。
template <typename Visit>
void walkBreadthFirst(const Node& root, Visit& visit) {
  std::deque<const Node*> queue;
  queue.push_back(&root);
  while (!queue.empty()) {
    const Node* node = queue.front();
    queue.pop_front();
    visit(*node);
    for (const auto& child : node->children) {
      if (child) queue.push_back(child.get());
    }
  }
}

// 一条赋值语句里被赋值的名字（不递归）。
// Python 版在 register/compile 两处各写了一遍同样的三分支。
void assignedTargets(const Node& node, std::vector<const Node*>* out) {
  switch (node.kind) {
    case Kind::Assign:
      for (std::size_t i = 0; i + 1 < node.children.size(); ++i) {
        out->push_back(node.children[i].get());
      }
      break;
    case Kind::AugAssign:
      out->push_back(node.children[0].get());
      break;
    case Kind::AnnAssign:
      if (node.children.size() >= 3) out->push_back(node.children[0].get());
      break;
    default:
      break;
  }
}

}  // namespace

bool CodeGen::isLocal(const std::string& name) const {
  if (block_ == &module_) return false;
  for (const std::string& local : block_->localNames) {
    if (local == name) return true;
  }
  return false;
}

void CodeGen::nameTarget(const std::string& name, const Node& node) {
  if (isLocal(name)) {
    emit("LOAD_LOCAL", node);
    emitU16(slotOf(name));
  } else {
    emit("LOAD_GLOBAL", node);
    emitU16(globalSlot(name));
  }
}

void CodeGen::nameStore(const std::string& name, const Node& node) {
  if (isLocal(name)) {
    emit("STORE_LOCAL", node);
    emitU16(slotOf(name));
  } else {
    emit("STORE_GLOBAL", node);
    emitU16(globalSlot(name));
    // 模块级赋值一旦编译过，后续读同名名字就是读变量而非内建/类型名。
    // 不能用预扫描的 globalsSet_：`int = 5` 写在后面时前面的 `int`
    // 在 CPython 里仍是内建。
    if (block_ == &module_) assignedSoFar_.insert(name);
  }
}

std::vector<std::string> CodeGen::targetNames(const Node& target,
                                              bool selfNames) const {
  std::vector<std::string> names;
  if (target.kind == Kind::Name) {
    names.push_back(target.text);
  } else if (selfNames && target.kind == Kind::Attribute && target.selfAttr) {
    // 方法体里的 `self.x`：Python 版在编译前就改写成了 `x`。
    names.push_back(target.text);
  } else if (target.kind == Kind::TupleLit || target.kind == Kind::ListLit) {
    for (const auto& element : target.children) {
      for (const std::string& name : targetNames(*element, selfNames)) {
        names.push_back(name);
      }
    }
  }
  return names;
}

std::vector<std::string> CodeGen::assignedNames(const Node& node,
                                                bool selfNames) const {
  std::vector<std::string> names;
  std::vector<const Node*> targets;
  assignedTargets(node, &targets);
  for (const Node* target : targets) {
    for (const std::string& name : targetNames(*target, selfNames)) {
      names.push_back(name);
    }
  }
  return names;
}

void CodeGen::collectGlobals(const Node& node, std::set<std::string>* out) {
  auto visit = [out](const Node& current) {
    if (current.kind == Kind::Global) {
      for (const std::string& name : current.names) out->insert(name);
    }
  };
  walkBreadthFirst(node, visit);
}

void CodeGen::collectSelfAssigned(const Node& node, std::set<std::string>* out) {
  auto visit = [out](const Node& current) {
    std::vector<const Node*> targets;
    assignedTargets(current, &targets);
    for (const Node* target : targets) {
      if (target->kind == Kind::Attribute && target->selfAttr) {
        out->insert(target->text);
      }
    }
  };
  walkBreadthFirst(node, visit);
}

void CodeGen::collectAssigned(
    const Node& node, bool selfNames,
    std::vector<std::pair<std::string, const Node*>>* out) const {
  auto visit = [this, out, selfNames](const Node& current) {
    std::vector<const Node*> targets;
    assignedTargets(current, &targets);
    for (const Node* target : targets) {
      for (const std::string& name : targetNames(*target, selfNames)) {
        out->emplace_back(name, target);
      }
    }
  };
  walkBreadthFirst(node, visit);
}

}  // namespace pcc
