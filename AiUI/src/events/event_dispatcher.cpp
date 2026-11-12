// 事件分发实现：捕获 → 目标 → 冒泡
#include "AiUI/events/event_dispatcher.h"

#include "AiUI/view.h"
#include "AiUI/view_group.h"

#include <algorithm>

namespace AiUI {

EventDispatcher::EventDispatcher(std::vector<View*> path, MouseEvent event)
    : path_(std::move(path)), event_(event) {
  windowPoint_ = event.position;
  event_.propagation = &state_;
}

MouseEvent EventDispatcher::eventFor(View* view) const {
  // 每层收到自己的局部坐标：窗口坐标一次换算到底
  MouseEvent e = event_;
  e.position = view->windowToLocal(windowPoint_);
  return e;
}

bool EventDispatcher::dispatchMouseDown() {
  // 1. 捕获：祖先从根往下，容器可在此拦下
  for (size_t i = 0; i + 1 < path_.size(); ++i) {
    View* view = path_[i];
    MouseEvent e = eventFor(view);
    if (view->dispatchInterceptMouseDown(e)) {
      // 拦下即接管：内层与目标都收不到，直接交给这个容器
      view->dispatchMouseDown(e);
      handler_ = view;
      state_.markHandled();
      return true;
    }
    if (state_.stopped) return state_.handled;
  }

  // 2. 目标
  if (View* view = target()) {
    const bool consumed = view->dispatchMouseDown(eventFor(view));
    if (state_.stopped) {
      handler_ = view;
      return true;
    }
    if (consumed) {
      handler_ = view;
      return true;
    }
  }

  // 3. 冒泡：从目标的父级往上
  for (size_t i = path_.size(); i-- > 1;) {
    View* view = path_[i - 1];
    if (view->dispatchMouseDown(eventFor(view)) || state_.stopped) {
      handler_ = view;
      return true;
    }
  }
  // 没人消费也要记下目标：按钮这类控件靠 up 才知道按下了
  handler_ = target();
  return state_.handled;
}

// 无返回值的事件（up/move/doubleClick/contextMenu）：
// 消费与否只能靠 stopPropagation 表达，与 Android 的「不发回去就不再上报」一致
bool EventDispatcher::runVoidEvent(bool (View::*intercept)(const MouseEvent&),
                                   void (View::*handler)(const MouseEvent&)) {
  if (intercept) {
    for (size_t i = 0; i + 1 < path_.size(); ++i) {
      View* view = path_[i];
      const MouseEvent e = eventFor(view);
      if ((view->*intercept)(e)) {
        (view->*handler)(e);
        handler_ = view;
        state_.markHandled();
        return true;
      }
      if (state_.stopped) return state_.handled;
    }
  }

  if (View* view = target()) {
    (view->*handler)(eventFor(view));
    handler_ = view;
    if (state_.stopped) return true;
  }

  for (size_t i = path_.size(); i-- > 1;) {
    View* view = path_[i - 1];
    (view->*handler)(eventFor(view));
    if (state_.stopped) {
      handler_ = view;
      return true;
    }
  }
  return state_.handled;
}

bool EventDispatcher::dispatchMouseUp() {
  return runVoidEvent(&View::dispatchInterceptMouseUp, &View::dispatchMouseUp);
}

bool EventDispatcher::dispatchMouseMove() {
  return runVoidEvent(&View::dispatchInterceptMouseMove, &View::dispatchMouseMove);
}

bool EventDispatcher::dispatchDoubleClick() {
  return runVoidEvent(nullptr, &View::dispatchDoubleClick);
}

bool EventDispatcher::dispatchContextMenu() {
  return runVoidEvent(nullptr, &View::dispatchContextMenu);
}

bool EventDispatcher::dispatchMouseWheel() {
  // 滚轮没有捕获阶段：它不像拖动那样需要容器与子控件争抢手势
  if (View* view = target()) {
    if (view->dispatchMouseWheel(eventFor(view))) return true;
    if (state_.stopped) return true;
  }
  for (size_t i = path_.size(); i-- > 1;) {
    View* view = path_[i - 1];
    if (view->dispatchMouseWheel(eventFor(view))) return true;
    if (state_.stopped) return true;
  }
  return state_.handled;
}

std::vector<View*> pathFromRoot(View* target) {
  std::vector<View*> path;
  // parent() 返回 ViewGroup*，上溯时统一按 View* 处理
  for (View* v = target; v; v = static_cast<View*>(v->parent())) path.push_back(v);
  std::reverse(path.begin(), path.end());
  return path;
}

}  // namespace AiUI
