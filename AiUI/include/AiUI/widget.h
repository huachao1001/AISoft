// 控件基类
//
// 生命周期：create() 建出平台窗口 → measure() 求尺寸 → layout() 摆放 →
// paint() 自绘 → 事件回调。
//
// 坐标约定：bounds() 是相对父控件客户区的矩形；所有尺寸参数都是
// 96 DPI 设计值，基类用 px() 换算成物理像素。
#pragma once

#include "AiUI/events.h"
#include "AiUI/painter.h"
#include "AiUI/platform.h"
#include "AiUI/types.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AiUI {

class Canvas;

// 子控件在容器内的摆放参数
struct LayoutParams {
  // 尺寸：kWrapContent 按内容，kMatchParent 撑满，其余为 96 DPI 设计值
  int width = kMatchParent;
  int height = kMatchParent;

  // FrameLayout 用的绝对偏移（96 DPI 设计值）
  int x = 0;
  int y = 0;

  int marginLeft = 0;
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;

  // LinearLayout 里按权重分配剩余空间；0 表示不参与拉伸
  float weight = 0.0f;

  Gravity horizontal = Gravity::kStart;
  Gravity vertical = Gravity::kStart;
  static constexpr int kWrapContent = -1;
  static constexpr int kMatchParent = -2;

  static LayoutParams wrap() {
    LayoutParams p;
    p.width = kWrapContent;
    p.height = kWrapContent;
    return p;
  }
  static LayoutParams match() { return LayoutParams{}; }
  static LayoutParams fixed(int w, int h) {
    LayoutParams p;
    p.width = w;
    p.height = h;
    return p;
  }
  // 尺寸可传 kWrapContent / kMatchParent 哨兵值
  LayoutParams withSize(int w, int h) {
    width = w;
    height = h;
    return *this;
  }
  // 绝对定位：FrameLayout 按左上角坐标放置
  static LayoutParams at(int x, int y, int w, int h) {
    LayoutParams p;
    p.x = x;
    p.y = y;
    p.width = w;
    p.height = h;
    return p;
  }
  LayoutParams margin(int all) {
    marginLeft = marginTop = marginRight = marginBottom = all;
    return *this;
  }
  LayoutParams margin(int horizontalMargin, int verticalMargin) {
    marginLeft = marginRight = horizontalMargin;
    marginTop = marginBottom = verticalMargin;
    return *this;
  }
  LayoutParams withWeight(float w) {
    weight = w;
    return *this;
  }
  LayoutParams align(Gravity hGravity, Gravity vGravity) {
    horizontal = hGravity;
    vertical = vGravity;
    return *this;
  }
};

// 控件基类：子类只覆盖自己关心的方法
class Widget {
 public:
  using Handler = std::function<void()>;
  using MouseHandler = std::function<void(const MouseEvent&)>;
  using KeyHandler = std::function<bool(const Key&, const Modifiers&)>;

  Widget() = default;
  virtual ~Widget();

  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  // ── 树结构 ──
  Widget* addChild(Widget* child, const LayoutParams& params = {});
  // 按类型建子控件并加入树；所有权交给父控件
  template <typename T, typename... Args>
  T* add(Args&&... args) {
    auto* child = new T(std::forward<Args>(args)...);
    addChild(child);
    return child;
  }
  // 深度优先找第一个匹配类型的后代
  template <typename T>
  T* find() const {
    for (Widget* child : children_) {
      if (auto* hit = dynamic_cast<T*>(child)) return hit;
      if (auto* hit = child->find<T>()) return hit;
    }
    return nullptr;
  }
  void removeChild(Widget* child);
  void removeAllChildren();
  const std::vector<Widget*>& children() const { return children_; }
  Widget* parent() const { return parent_; }

  const LayoutParams& layoutParams() const { return params_; }
  void setLayoutParams(const LayoutParams& params);

  // ── 生命周期 ──
  // 建出平台窗口；bounds 为 96 DPI 设计值
  NativeHandle create(NativeHandle parentHandle, int id, const Rect& bounds,
                      const std::wstring& className = L"AiUIWidget");
  // 把已建好的控件挂进树，之后由布局接管位置
  void attach();
  void destroy();
  bool isCreated() const { return window_ != nullptr; }

  NativeHandle handle() const;
  NativeHandle parentHandle() const { return parentHandle_; }
  int id() const { return id_; }

  // ── 几何 ──
  const Rect& bounds() const { return bounds_; }
  Rect clientRect() const;
  int dpi() const;
  // 96 DPI 设计值 → 物理像素
  int px(int value) const;
  // 父控件客户区原点下的绝对位置
  Point absolutePosition() const;

  // 求期望尺寸（设计值输入，物理像素返回）
  Size measure(const Size& available);
  // 摆放自己与子树；frame 相对父控件客户区
  void layout(const Rect& frame);
  void setBounds(const Rect& frame);
  void invalidate();

  // ── 外观与状态 ──
  void show(bool visible = true);
  bool isVisible() const { return visible_; }
  void setEnabled(bool enabled);
  bool isEnabled() const { return enabled_; }
  void setFocus();
  void invalidateParent();
  // 需要竖向滚动条时打开；由控件自己调 setScrollInfo 维护范围
  void setScrollable(bool scrollable) { scrollable_ = scrollable; }
  bool isScrollable() const { return scrollable_; }

  // ── 事件：支持回调与派生覆盖两种风格 ──
  void setOnClick(Handler fn) { onClick_ = std::move(fn); }
  void setOnMouseDown(MouseHandler fn) { onMouseDownFn_ = std::move(fn); }
  void setOnMouseUp(MouseHandler fn) { onMouseUpFn_ = std::move(fn); }
  void setOnMouseMove(MouseHandler fn) { onMouseMoveFn_ = std::move(fn); }
  void setOnMouseLeave(Handler fn) { onMouseLeaveFn_ = std::move(fn); }
  void setOnKeyDown(KeyHandler fn) { onKeyDownFn_ = std::move(fn); }

  // 平台层回调入口（由 PlatformWindow 转发，业务代码不要直接调）
  void dispatchPaint(Canvas& canvas);
  void dispatchResize(int width, int height);
  void dispatchMouseDown(const MouseEvent& e);
  void dispatchMouseUp(const MouseEvent& e);
  void dispatchMouseMove(const MouseEvent& e);
  void dispatchMouseLeave();
  void dispatchMouseWheel(const MouseEvent& e);
  void dispatchContextMenu(const MouseEvent& e);
  void dispatchDoubleClick(const MouseEvent& e);
  bool dispatchKeyDown(const Key& key, const Modifiers& mods);
  bool dispatchChar(wchar_t ch);
  void dispatchFocusChanged(bool focused);
  void dispatchScroll(const ScrollEvent& e);
  void dispatchTimer();
  void dispatchCompositionStart();
  void dispatchCompositionUpdate(const std::wstring& text);
  void dispatchCompositionEnd();
  // 触发点击回调；派生类在确认点击后调用
  void dispatchClick();

 protected:
  // ── 派生类钩子 ──
  virtual void onCreated() {}
  virtual void onPaint(Painter& painter, const Rect& rect) {
    (void)painter;
    (void)rect;
  }
  // 内容期望尺寸；available 为父给出的可用尺寸（物理像素）
  virtual Size onMeasure(const Size& available);
  // 容器摆放子控件；默认按 LayoutParams 绝对定位 + 对齐
  virtual void arrangeChildren(const Rect& content);
  virtual void onLayout(const Rect&) {}
  virtual void onMouseDown(const MouseEvent&) {}
  virtual void onMouseUp(const MouseEvent&) {}
  virtual void onMouseMove(const MouseEvent&) {}
  virtual void onMouseLeave() {}
  virtual void onMouseWheel(const MouseEvent&) {}
  virtual void onContextMenu(const MouseEvent&) {}
  virtual void onDoubleClick(const MouseEvent&) {}
  virtual bool onKeyDown(const Key&, const Modifiers&) { return false; }
  virtual bool onChar(wchar_t) { return false; }
  virtual void onFocusChanged(bool) {}
  virtual void onScroll(const ScrollEvent&) {}
  virtual void onTimer() {}
  // IME 组合：开始 / 更新组合串 / 结束
  virtual void onCompositionStart() {}
  virtual void onCompositionUpdate(const std::wstring&) {}
  virtual void onCompositionEnd() {}

  // 默认背景色；不想铺底就返回 kNoColor
  virtual Color backgroundColor() const;
  // 自身或最近上级的底色：圆角外露出的四角用它填
  Color resolvedBackground() const;

  void captureMouse();
  void releaseMouse();
  void setCursor(CursorShape shape);

  PlatformWindow* window() const { return window_; }
  const Rect& lastFrame() const { return bounds_; }

  // 子控件的期望尺寸：把设计值换算成物理像素
  Size childMeasureSize(Widget* child, const Size& available) const;

  std::wstring className_ = L"AiUIWidget";

 private:
  void paintInto(Canvas& canvas);

  std::unique_ptr<WindowHost> host_;
  PlatformWindow* window_ = nullptr;
  NativeHandle parentHandle_ = nullptr;
  int id_ = 0;
  Widget* parent_ = nullptr;
  std::vector<Widget*> children_;
  LayoutParams params_{};

  Rect bounds_{};
  Size measured_{};
  bool visible_ = true;
  bool enabled_ = true;
  bool scrollable_ = false;

  Handler onClick_;
  MouseHandler onMouseDownFn_;
  MouseHandler onMouseUpFn_;
  MouseHandler onMouseMoveFn_;
  Handler onMouseLeaveFn_;
  KeyHandler onKeyDownFn_;
};

}  // namespace AiUI
