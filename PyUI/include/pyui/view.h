// View：绘制与事件的基本单位
//
// 与 Android 的 View 对应：一块矩形区域、树上一个节点、一次绘制的入口。
// 控件不创建原生子窗口——整棵树共用一个窗口，脏区由 ViewRoot 统一驱动，
// 所以拖动窗口或超出屏幕都不会留下空白。
//
// 坐标约定：bounds() 相对父容器；控件内部一律用自身局部坐标（左上角为原点）。
// 尺寸参数都是 96 DPI 设计值，用 px() 换算成物理像素。
#pragma once

#include "pyui/events.h"
#include "pyui/id.h"
#include "pyui/painter.h"
#include "pyui/theme.h"
#include "pyui/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pyui {

class ViewGroup;
class ViewRoot;

// 窗口角色：由 XML 用 role="..." 声明，平台层据此决定命中行为。
//
// 为什么要有这一层：最大化按钮必须返回 HTMAXBUTTON，系统才会给出贴靠布局的
// 悬停预览；而按钮摆在哪由布局决定，不能写死在核心代码里。没有角色时只能靠
// 约定 id（0x7A06）来认按钮，一旦应用自定义标题栏换了个 id 或挪了位置就失灵。
// 角色把「这个控件在窗口里干什么」变成布局数据，判定只看角色、不看位置和 id。
//
// 对应关系：
//   kCaption       拖动窗口、双击最大化；返回 HTCAPTION
//   kMaximize      返回 HTMAXBUTTON（唯一能触发吸附下拉的角色）
//   kMinimize      点击最小化
//   kClose         点击关闭
//   kTitle         标题文字（标题栏自己找它，用来显示窗口标题）
//   kIcon          应用图标（同上）
enum class WindowRole {
  kNone,
  kCaption,
  kMaximize,
  kMinimize,
  kClose,
  kTitle,
  kIcon,
};

// 窗口角色 → 名字与名字 → 角色。生成器与运行期共用一份，避免两边写两套字符串。
const char* windowRoleName(WindowRole role);
bool parseWindowRole(const std::string& text, WindowRole* out);

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

  // 期望尺寸上限（96 DPI 设计值）；0 表示不限。
  // 用在「内容想多宽就多宽、但不能把兄弟视图挤出去」的控件上：
  // 标题按文字宽度自然排，超上限时由控件自己省略。
  int maxWidth = 0;
  int maxHeight = 0;

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
  static LayoutParams at(int x, int y, int w, int h) {
    LayoutParams p;
    p.x = x;
    p.y = y;
    p.width = w;
    p.height = h;
    return p;
  }
  LayoutParams withSize(int w, int h) {
    width = w;
    height = h;
    return *this;
  }
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
  LayoutParams margin(int left, int top, int right, int bottom) {
    marginLeft = left;
    marginTop = top;
    marginRight = right;
    marginBottom = bottom;
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
  LayoutParams withMax(int w, int h) {
    maxWidth = w;
    maxHeight = h;
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
  // 按字符串设 id：编译期折成整数，findView 按同一算法匹配
  void setId(const char* id) { id_ = makeId(id); }
  int id() const { return id_; }
  // 窗口角色：XML 属性 role 写入，平台层判定命中的唯一依据。
  // 默认 kNone，表示这个控件不参与标题栏/缩放带的特殊命中。
  void setWindowRole(WindowRole role) { windowRole_ = role; }
  WindowRole windowRole() const { return windowRole_; }
  virtual View* findViewById(int id);
  View* findView(const char* id) { return findViewById(makeId(id)); }
  // 按整数 id 查找。内置控件（标题栏）沿用系统常量（0x7A00–0x7A08），
  // 那些值在 AiUI 与 Win32 里是同一套契约，不该被哈希成别的数。
  View* findView(int id) { return findViewById(id); }

  template <typename T>
  T* findView(const char* id) {
    return dynamic_cast<T*>(findViewById(makeId(id)));
  }

  template <typename T>
  T* findView(int id) {
    return dynamic_cast<T*>(findViewById(id));
  }

  // ── 生命周期 ──
  bool isAttached() const { return attached_; }
  bool isCreated() const { return created_; }

  // ── 几何 ──
  void layout(const Rect& frame);
  void setBounds(const Rect& frame) { layout(frame); }
  const Rect& bounds() const { return bounds_; }
  Rect clientRect() const { return Rect{0, 0, bounds_.width(), bounds_.height()}; }
  // 相对窗口客户区的绝对矩形
  Rect absoluteBounds() const;
  Point absolutePosition() const;
  Point windowToLocal(Point windowPoint) const;

  ViewRoot* root() const { return root_; }
  ViewGroup* parent() const { return parent_; }
  void setRoot(ViewRoot* root) { root_ = root; }

  int dpi() const;
  // 96 DPI 设计值 → 物理像素
  int px(int value) const;

  // 求期望尺寸（96 DPI 设计值输入，物理像素返回）
  Size measure(const Size& available);
  // 最近一次测量的结果
  const Size& measuredSize() const { return measured_; }

  // ── 刷新 ──
  void invalidate();
  void invalidateRect(const Rect& localRect);
  void requestLayout();

  // ── 主题 ──
  // 设本子树使用的调色板。主题沿父链继承，根上设一次即覆盖整棵树。
  //
  // 做成虚函数只为一种情况：控件把主题颜色**缓存进了自身状态**（TitleBar 把
  // 标题色写进子 TextView），换肤时必须重取一次。纯绘制控件在 onPaint 里现取
  // 主题，不需要覆盖它。
  virtual void setTheme(const Theme* theme);
  const Theme* ownTheme() const { return theme_; }
  const Theme& theme() const;

  // ── 外观与状态 ──
  void show(bool visible = true);
  // 与 show 等价，供属性生成代码直接调用
  void setVisible(bool visible) { show(visible); }
  bool isVisible() const { return visible_; }
  // 显式底色；kNoColor 表示不铺底，让父容器透上来
  void setBackgroundColor(Color color);
  Color backgroundColor() const { return explicitBackground_; }
  // 按主题字段铺底：传 &Theme::dialogBg 之类的成员指针，换主题时自动重取
  void setBackgroundRole(Color Theme::*role);
  virtual void setEnabled(bool enabled);
  bool isEnabled() const { return enabled_; }
  // 当前是否可用（自身与所有祖先都启用）
  bool isEffectivelyEnabled() const;
  void setFocus();
  bool isFocused() const;
  void setFocusable(bool focusable) { focusable_ = focusable; }
  bool isFocusable() const { return focusable_; }

  // 不参与命中的装饰视图（标题文字、应用图标、纯背景条）。
  //
  // 默认命中测试会把点投给最上层的后代，于是标题栏的文字会吃掉拖动与双击：
  // 用户按在标题上看不到窗口移动，双击文字也不会最大化。装饰视图自身没有
  // 交互语义，标成透明后事件落到父容器，父容器才拿得到「空白区」语义。
  // 容器的子视图仍然可以命中——这一位只影响「本视图自己」。
  void setMouseTransparent(bool transparent) { mouseTransparent_ = transparent; }
  bool isMouseTransparent() const { return mouseTransparent_; }

  // 悬停与按下由 ViewRoot 在分发时维护：视图不必自己跟踪 enter/leave，
  // 也不会因为指针移出后没收到 leave 而永远停在悬停态。
  bool isHot() const { return hot_; }
  bool isPressed() const { return pressed_; }

  // 悬停时显示的鼠标指针形状。可点击控件默认返回手形。
  virtual CursorShape cursorShape() const { return CursorShape::kArrow; }

  // ── 事件：支持回调与派生覆盖两种风格 ──
  void setOnClick(Handler fn) { onClick_ = std::move(fn); }
  // 是否已经挂过点击回调。窗口角色动作只在「没人接手」时补上，
  // 否则会覆盖应用自己写的 onClick。
  bool hasOnClick() const { return static_cast<bool>(onClick_); }
  void setOnMouseDown(MouseHandler fn) { onMouseDownFn_ = std::move(fn); }
  void setOnMouseUp(MouseHandler fn) { onMouseUpFn_ = std::move(fn); }
  void setOnMouseMove(MouseHandler fn) { onMouseMoveFn_ = std::move(fn); }
  void setOnMouseLeave(Handler fn) { onMouseLeaveFn_ = std::move(fn); }
  void setOnKeyDown(KeyHandler fn) { onKeyDownFn_ = std::move(fn); }
  // 右键与双击也提供回调挂点：控件本身对它们没有默认语义（不像点击会改状态），
  // 所以只要一个纯回调就够，不必逼用户为了接一个事件去继承控件。
  void setOnContextMenu(MouseHandler fn) { onContextMenuFn_ = std::move(fn); }
  void setOnDoubleClick(MouseHandler fn) { onDoubleClickFn_ = std::move(fn); }

  // ── 布局 ──
  // 布局里的标签名，用于错误信息与调试（如 "LinearLayout"）。
  // 不是布局解析的入口——XML 在构建期已编译成 setter 调用。
  virtual const char* className() const { return "View"; }

  // ── 父容器与 ViewRoot 调用入口，业务代码不要直接用 ──
  virtual void draw(Painter& painter, const Rect& dirty);
  virtual bool dispatchMouseDown(const MouseEvent& e);
  virtual void dispatchMouseUp(const MouseEvent& e);
  virtual void dispatchMouseMove(const MouseEvent& e);
  virtual void dispatchMouseLeave();
  virtual bool dispatchMouseWheel(const MouseEvent& e);
  virtual void dispatchContextMenu(const MouseEvent& e);
  virtual void dispatchDoubleClick(const MouseEvent& e);
  virtual bool dispatchKeyDown(const Key& key, const Modifiers& mods);
  virtual bool dispatchChar(std::uint32_t codepoint);
  virtual void dispatchFocusChanged(bool focused);
  virtual void dispatchTimer();
  // 点击激活。基类实现只是调用 onClick 回调；复选/单选/按钮等会先改状态
  // 再回调，因此这里留成虚函数，而不是让每个控件各自在鼠标释放里重复一遍。
  virtual void dispatchClick();

  // 挂载/卸载子树（由 ViewRoot 递归驱动）
  void attachTree(ViewRoot* root);
  void detachTree();

  // 子树；只有 ViewGroup 会填充
  std::vector<View*> children_;

  friend class ViewGroup;
  friend class ViewRoot;

 protected:
  // ── 派生类钩子 ──
  virtual void onAttached() {}
  virtual void onDetached() {}
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
  virtual void onLayout(const Rect& frame) { (void)frame; }
  virtual void onDrawChildren(Painter& painter, const Rect& dirty) {
    (void)painter;
    (void)dirty;
  }
  virtual bool onMouseDown(const MouseEvent&) { return false; }
  virtual void onMouseUp(const MouseEvent&) {}
  virtual void onMouseMove(const MouseEvent&) {}
  virtual void onMouseLeave() {}
  virtual bool onMouseWheel(const MouseEvent&) { return false; }
  virtual void onContextMenu(const MouseEvent&) {}
  virtual void onDoubleClick(const MouseEvent&) {}
  virtual bool onKeyDown(const Key&, const Modifiers&) { return false; }
  virtual bool onChar(std::uint32_t) { return false; }
  virtual void onFocusChanged(bool) {}
  virtual void onTimer() {}

  // 默认背景色；不想铺底就返回 kNoColor
  virtual Color resolveBackground() const;

  // 清掉悬停/按下态。只该在「控件变得不可交互」时调用（禁用、隐藏、
  // 从树上摘下）；常态下这两个状态由 ViewRoot 统一维护，控件不要自己改。
  void clearVisualState();

  // 子视图的期望尺寸：把设计值换算成物理像素
  Size childMeasureSize(View* child, const Size& available) const;

  // 非空时底色取自当前主题的该字段，优先于 explicitBackground_
  Color Theme::*backgroundRole_ = nullptr;

 private:
  static void invalidateSubtree(View* view);

  ViewRoot* root_ = nullptr;
  ViewGroup* parent_ = nullptr;
  LayoutParams params_{};

  Rect bounds_{};
  Size measured_{};
  int id_ = 0;
  WindowRole windowRole_ = WindowRole::kNone;
  Color explicitBackground_ = kNoColor;
  const Theme* theme_ = nullptr;

  bool attached_ = false;
  bool created_ = false;
  bool visible_ = true;
  bool enabled_ = true;
  bool focusable_ = false;
  bool mouseTransparent_ = false;
  bool hot_ = false;
  bool pressed_ = false;

  Handler onClick_;
  MouseHandler onMouseDownFn_;
  MouseHandler onMouseUpFn_;
  MouseHandler onMouseMoveFn_;
  Handler onMouseLeaveFn_;
  KeyHandler onKeyDownFn_;
  MouseHandler onContextMenuFn_;
  MouseHandler onDoubleClickFn_;
};

}  // namespace pyui
