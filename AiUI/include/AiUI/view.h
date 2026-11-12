// View：绘制与事件的基本单位
//
// 与 Android 的 View 对应：一块矩形区域、树上的一个节点、一次绘制的入口。
// 控件不再各自创建原生子窗口——整棵树由 ViewRoot 画进同一个窗口位图，
// 脏区也由它统一驱动，所以拖动窗口或超出屏幕都不会留下空白。
//
// 坐标约定：bounds() 是相对父容器的矩形；控件内部一律用自身局部坐标
// （左上角为原点），绘制与事件到达时都已是局部坐标。尺寸参数都是
// 96 DPI 设计值，用 px() 换算成物理像素。
#pragma once

#include "AiUI/events.h"
#include "AiUI/id.h"
#include "AiUI/painter.h"
#include "AiUI/platform.h"
#include "AiUI/types.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace AiUI {

class ViewRoot;
class ViewGroup;

// 子视图在容器内的摆放参数
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
  // 只给偏移，尺寸另设；FrameLayout 用来做相对内边距的位移
  LayoutParams offset(int dx, int dy) {
    x = dx;
    y = dy;
    return *this;
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

// 视图基类。叶子控件直接继承它，容器继承 ViewGroup。
class View {
 public:
  using Handler = std::function<void()>;
  using MouseHandler = std::function<void(const MouseEvent&)>;
  using KeyHandler = std::function<bool(const Key&, const Modifiers&)>;

  View();
  virtual ~View();

  View(const View&) = delete;
  View& operator=(const View&) = delete;

  // ── 布局参数 ──
  const LayoutParams& layoutParams() const { return params_; }
  void setLayoutParams(const LayoutParams& params);

  // ── 标识 ──
  // 默认 0 表示没有 id；只有设了 id 的视图才参与 findViewById 匹配
  void setId(int id) { id_ = id; }
  int id() const { return id_; }
  // 按 id 找后代（含自身）；找不到返回 nullptr
  virtual View* findViewById(int id);
  // 按字符串 id 找后代；字符串在编译期折成整数，与小写的 makeId 对齐
  View* findView(const char* id) { return findViewById(makeId(id)); }

  // 按字符串 id 找指定类型的后代；类型不符时返回 nullptr
  template <typename T>
  T* findView(const char* id) {
    return dynamic_cast<T*>(findViewById(makeId(id)));
  }

  // ── 生命周期 ──
  // 挂到宿主窗口；只有根视图建窗口，子视图随树递归挂载
  bool create(NativeHandle parent, const Rect& bounds = Rect{});
  // 带窗口参数：顶层窗口、自绘边框等由调用方决定
  bool create(NativeHandle parent, const WindowParams& params, const Rect& bounds = Rect{});
  bool isCreated() const { return created_; }
  // 根视图对应的原生窗口句柄；子视图返回 nullptr
  NativeHandle handle() const;

  // 摆放自己与子树；frame 相对父容器
  void layout(const Rect& frame);
  void setBounds(const Rect& frame) { layout(frame); }

  // ── 几何 ──
  const Rect& bounds() const { return bounds_; }
  // 局部矩形，左上角恒为原点
  Rect clientRect() const;
  // 相对窗口客户区的绝对矩形
  Rect absoluteBounds() const;
  Point absolutePosition() const;
  Rect localToWindow(const Rect& localRect) const;
  Point localToWindow(Point localPoint) const;
  // 窗口坐标 → 本视图局部坐标
  Point windowToLocal(Point windowPoint) const;
  int dpi() const;
  // 96 DPI 设计值 → 物理像素
  int px(int value) const;

  ViewRoot* root() const { return root_; }
  ViewGroup* parent() const { return parent_; }

  // 求期望尺寸（设计值输入，物理像素返回）
  Size measure(const Size& available);

  // ── 刷新 ──
  // 整个视图重绘
  void invalidate();
  // 只重绘一块局部区域：悬停、选中这类变化用
  void invalidateRect(const Rect& localRect);
  // 标记需要重新测量与摆放
  void requestLayout();

  // ── 外观与状态 ──
  void show(bool visible = true);
  bool isVisible() const { return visible_; }
  // 显式底色；传 kNoColor 表示不铺底，让父容器透上来
  void setBackgroundColor(Color color);
  // 拖窗区域：开启后在该视图上按住即可拖动整个窗口，与系统标题栏手感一致。
  // 双击时切换最大化（系统标题栏的默认行为）。
  void setDragWindow(bool enabled) { dragWindow_ = enabled; }
  bool isDragWindow() const { return dragWindow_; }
  virtual void setEnabled(bool enabled);
  bool isEnabled() const { return enabled_; }
  void setFocus();
  bool isFocused() const;
  void setFocusable(bool focusable) { focusable_ = focusable; }
  bool isFocusable() const { return focusable_; }

  // ── 事件：支持回调与派生覆盖两种风格 ──
  void setOnClick(Handler fn) { onClick_ = std::move(fn); }
  void setOnMouseDown(MouseHandler fn) { onMouseDownFn_ = std::move(fn); }
  void setOnMouseUp(MouseHandler fn) { onMouseUpFn_ = std::move(fn); }
  void setOnMouseMove(MouseHandler fn) { onMouseMoveFn_ = std::move(fn); }
  void setOnMouseLeave(Handler fn) { onMouseLeaveFn_ = std::move(fn); }
  void setOnKeyDown(KeyHandler fn) { onKeyDownFn_ = std::move(fn); }

  // ── 父容器与 ViewRoot 调用入口，业务代码不要直接用 ──
  // 绘制自身；painter 已平移到本视图局部坐标，dirty 也是局部坐标
  virtual void draw(Painter& painter, const Rect& dirty);
  virtual bool dispatchMouseDown(const MouseEvent& e);
  virtual void dispatchMouseUp(const MouseEvent& e);
  virtual void dispatchMouseMove(const MouseEvent& e);
  virtual void dispatchMouseLeave();
  virtual bool dispatchMouseWheel(const MouseEvent& e);
  virtual void dispatchContextMenu(const MouseEvent& e);
  virtual void dispatchDoubleClick(const MouseEvent& e);
  virtual bool dispatchKeyDown(const Key& key, const Modifiers& mods);
  virtual bool dispatchChar(wchar_t ch);
  virtual void dispatchFocusChanged(bool focused);
  // 窗口整体失活/恢复。与焦点转移不同：逻辑焦点不变、不触发失焦回调，
  // 只用来收起光标这类「没有前台就不该出现」的装饰。
  virtual void dispatchWindowActivated(bool active);
  virtual void dispatchTimer();
  // 一次性延时到期
  virtual void dispatchDelayed();
  // 触发点击回调；派生类在确认点击后调用
  void dispatchClick();

 protected:
  // ── 派生类钩子 ──
  virtual void onCreated() {}
  // 局部坐标绘制；rect 为 clientRect()
  virtual void onPaint(Painter& painter, const Rect& rect) {
    (void)painter;
    (void)rect;
  }
  // 子视图之后绘制，用于滚动条这类浮层
  virtual void onDrawForeground(Painter& painter, const Rect& rect) {
    (void)painter;
    (void)rect;
  }
  // 内容期望尺寸；available 为父给出的可用尺寸（物理像素）
  virtual Size onMeasure(const Size& available);
  // 尺寸确定后调用；容器在这里摆放子视图
  virtual void onLayout(const Rect& frame) { (void)frame; }
  // 挂载阶段：子视图递归挂载
  virtual void onAttach() {}
  virtual void onDetach() {}
  // 容器绘制子视图的入口；叶子控件不覆盖
  virtual void onDrawChildren(Painter& painter, const Rect& dirty) {
    (void)painter;
    (void)dirty;
  }
  // 返回 true 表示事件已消费，不再往上冒泡
  virtual bool onMouseDown(const MouseEvent&) { return false; }
  virtual void onMouseUp(const MouseEvent&) {}
  virtual void onMouseMove(const MouseEvent&) {}
  virtual void onMouseLeave() {}
  virtual bool onMouseWheel(const MouseEvent&) { return false; }
  virtual void onContextMenu(const MouseEvent&) {}
  virtual void onDoubleClick(const MouseEvent&) {}
  virtual bool onKeyDown(const Key&, const Modifiers&) { return false; }
  virtual bool onChar(wchar_t) { return false; }
  virtual void onFocusChanged(bool) {}
  // 窗口整体失活/恢复，逻辑焦点保持不变
  virtual void onWindowActivated(bool) {}
  virtual void onTimer() {}
  // 一次性延时到期；postDelayed 时登记的目标收到
  virtual void onDelayed() {}

  // 非客户区命中：windowPoint 是窗口坐标。标题栏这类需要系统拖动/缩放的
  // 区域返回 kCaption 或边框方向，其余返回 kClient。
  virtual HitTarget onHitTest(const Point& windowPoint) {
    (void)windowPoint;
    // 开启 dragWindow 的视图直接当标题栏：拖动、贴边吸附交给系统
    return dragWindow_ ? HitTarget::kCaption : HitTarget::kClient;
  }

  // 默认背景色；不想铺底就返回 kNoColor
  virtual Color backgroundColor() const;
  // 自身或最近上级的底色：圆角外露出的四角用它填
  Color resolvedBackground() const;

  void captureMouse();
  void releaseMouse();
  // 请求鼠标指针形状；悬停或捕获时由 ViewRoot 应用
  void setCursor(CursorShape shape);
  CursorShape cursorShape() const { return cursorShape_; }
  // 输入光标闪烁：整棵树共用一个定时器
  void startBlinkTimer();
  void stopBlinkTimer();
  // 一次性延时：ms 毫秒后回调 onDelayed；再次调用会重新计时，0 表示取消
  void postDelayed(int ms);
  // 让输入法候选框跟随光标（局部坐标）
  void updateImeCaret(int localX, int localY);

  // 子视图的期望尺寸：把设计值换算成物理像素
  Size childMeasureSize(View* child, const Size& available) const;

  // 子树；只有 ViewGroup 会填充
  std::vector<View*> children_;

  friend class ViewGroup;
  friend class ViewRoot;

 private:
  // 递归挂载/卸载子树
  void attachTree();
  void detachTree();

  ViewRoot* root_ = nullptr;
  std::unique_ptr<ViewRoot> ownedRoot_;
  ViewGroup* parent_ = nullptr;
  LayoutParams params_{};

  Rect bounds_{};
  Size measured_{};
  int id_ = 0;
  Color explicitBackground_ = kNoColor;
  bool created_ = false;
  bool visible_ = true;
  bool enabled_ = true;
  bool focusable_ = false;
  bool dragWindow_ = false;
  CursorShape cursorShape_ = CursorShape::kArrow;

  Handler onClick_;
  MouseHandler onMouseDownFn_;
  MouseHandler onMouseUpFn_;
  MouseHandler onMouseMoveFn_;
  Handler onMouseLeaveFn_;
  KeyHandler onKeyDownFn_;
};

}  // namespace AiUI
