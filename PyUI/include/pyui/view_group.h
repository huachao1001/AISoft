// ViewGroup：可容纳子视图的容器
//
// 管子树的增删、测量、摆放与绘制。具体怎么摆由子类覆盖 arrangeChildren
// 决定（LinearLayout / FrameLayout）。
//
// 事件不在这里逐层转发：ViewRoot 命中到最深的视图后直接投递，这样悬停、
// 捕获、焦点只有一份状态，不会重复触发。
#pragma once

#include "pyui/view.h"

namespace pyui {

class ViewGroup : public View {
 public:
  ViewGroup() = default;
  ~ViewGroup() override;

  // ── 子树 ──
  View* addView(View* child, const LayoutParams& params = {});
  void removeView(View* child);
  void removeAllViews();
  const std::vector<View*>& children() const { return children_; }
  int childCount() const { return static_cast<int>(children_.size()); }
  View* childAt(int index) const;
  int indexOfChild(View* child) const;

  template <typename T, typename... Args>
  T* add(Args&&... args) {
    auto* child = new T(std::forward<Args>(args)...);
    addView(child);
    return child;
  }

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
  // 命中测试：点落在哪个最上层后代上；落空时返回容器自身。
  //
  // 虚函数：容器有时要把「落在本层空白处」翻译成别的语义（标题栏要把
  // band 的空白回落到自身，否则拖动窗口与双击最大化收不到事件）。
  virtual View* hitTest(Point localPoint) const;
  View* findViewById(int id) override;

 protected:
  // 摆放子视图；content 是去掉内边距后的区域（局部坐标，物理像素）
  virtual void arrangeChildren(const Rect& content);
  virtual void onChildAdded(View* child) { (void)child; }
  virtual void onChildRemoved(View* child) { (void)child; }
  void onLayout(const Rect& frame) override;
  void onDrawChildren(Painter& painter, const Rect& dirty) override;

  Insets padding_{};

 private:
  friend class View;
  friend class ViewRoot;
};

}  // namespace pyui
