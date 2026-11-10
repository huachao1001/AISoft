// ViewGroup：可容纳子视图的容器
//
// 与 Android 的 ViewGroup 对应：管子树的增删、测量、摆放与绘制。
// 具体怎么摆由子类覆盖 arrangeChildren 决定（LinearLayout / FrameLayout）。
//
// 事件不在这里逐层转发：ViewRoot 命中到最深的视图后直接投递，
// 这样悬停、捕获、焦点只有一份状态，不会重复触发。
#pragma once

#include "AiUI/view.h"

namespace AiUI {

class ViewGroup : public View {
 public:
  ViewGroup() = default;
  ~ViewGroup() override;

  // ── 子树 ──
  View* addView(View* child, const LayoutParams& params = {});
  // 按类型建子视图并加入树；所有权交给父容器
  template <typename T, typename... Args>
  T* add(Args&&... args) {
    auto* child = new T(std::forward<Args>(args)...);
    addView(child);
    return child;
  }
  void removeView(View* child);
  void removeAllViews();
  const std::vector<View*>& children() const { return children_; }
  int childCount() const { return static_cast<int>(children_.size()); }
  View* childAt(int index) const;
  int indexOfChild(View* child) const;
  // 深度优先找第一个匹配类型的后代
  template <typename T>
  T* find() const {
    for (View* child : children_) {
      if (auto* hit = dynamic_cast<T*>(child)) return hit;
      if (auto* group = dynamic_cast<ViewGroup*>(child)) {
        if (auto* hit = group->find<T>()) return hit;
      }
    }
    return nullptr;
  }

  // ── 内边距（96 DPI 设计值）──
  void setPadding(int all);
  void setPadding(int horizontal, int vertical);
  void setPadding(int left, int top, int right, int bottom);
  const Insets& padding() const { return padding_; }
  // 去掉内边距后的内容区（局部坐标）
  Rect contentRect() const;

  // ── View ──
  // 命中测试：点落在哪个最上层后代上；落空时返回容器自身
  View* hitTest(Point localPoint) const;

 protected:
  // 摆放子视图；content 是去掉内边距后的区域（局部坐标）
  virtual void arrangeChildren(const Rect& content);
  void onLayout(const Rect& frame) override;
  void onDrawChildren(Painter& painter, const Rect& dirty) override;

  // 子树挂载/卸载：只由 View 的递归挂载调用
  void attachChildren();
  void detachChildren();

 private:
  Insets padding_{};

  friend class View;
  friend class ViewRoot;
};

}  // namespace AiUI
