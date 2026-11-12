// 事件分发：Android 风格的「捕获 → 目标 → 冒泡」三段式
//
// 三段与 Android 的 touch 分发一一对应：
//   1. 捕获（capture）：从根往目标下沉，每层先给一次拦截机会。
//      容器可以在这里抢下手势，例如滚动容器优先于内部控件。
//   2. 目标（target）：命中视图自己处理。
//   3. 冒泡（bubble）：从目标的父级逐层向上，直到根。
//
// 任一层调用 MouseEvent::stopPropagation() 之后，后续层都不再收到事件；
// onMouseDown / onMouseWheel 返回 true 同样视为消费并停止冒泡。
// 这与 Android「处理了就不再向上」的语义一致。
//
// 命中测试与坐标换算仍由 ViewGroup / ViewRoot 负责，这里只排列接收顺序，
// 因此可以脱离窗口单独测试。
#pragma once

#include "AiUI/events.h"

#include <vector>

namespace AiUI {

class View;

class EventDispatcher {
 public:
  // path 为「根 → … → 目标」，末位必须是目标自身
  EventDispatcher(std::vector<View*> path, MouseEvent event);

  // 按捕获/目标/冒泡的顺序派发；返回事件是否被消费
  bool dispatchMouseDown();
  bool dispatchMouseUp();
  bool dispatchMouseMove();
  bool dispatchMouseWheel();
  bool dispatchDoubleClick();
  bool dispatchContextMenu();

  bool isHandled() const { return state_.handled; }
  bool isStopped() const { return state_.stopped; }
  View* target() const { return path_.empty() ? nullptr : path_.back(); }
  // 真正消费事件的那一层：可能被容器在捕获阶段拦下，不等于 target()。
  // ViewRoot 用它决定后续 up/move 该发给谁。
  View* handler() const { return handler_; }

 private:
  // up/move/doubleClick/contextMenu 没有返回值，消费只能由 stopPropagation 表达
  bool runVoidEvent(bool (View::*intercept)(const MouseEvent&),
                    void (View::*handler)(const MouseEvent&));
  // 把窗口坐标换算成该层的局部坐标
  MouseEvent eventFor(View* view) const;

  std::vector<View*> path_;
  MouseEvent event_;
  Point windowPoint_{};
  EventState state_{};
  View* handler_ = nullptr;
};

// 从目标的祖先链收集「根 → 目标」路径
std::vector<View*> pathFromRoot(View* target);

}  // namespace AiUI
