// ViewRoot：整棵视图树唯一的宿主，负责把树画到后端并驱动脏区
//
// 与 AiUI 的 ViewRoot 职责一致：唯一的绘制入口、唯一的焦点/捕获状态。
// 区别在于这里不持有平台窗口——绘制目标是一个 Surface 或平台 Canvas，
// 由调用方（窗口后端或离线渲染器）驱动。
//
// 这样「屏幕绘制」与「离线渲染」走同一条代码路径：
//
//   ViewRoot::draw(painter, dirty)   ← 屏幕后端传入窗口画布
//   ViewRoot::draw(painter, dirty)   ← 离线渲染传入内存 Surface
//
// 也就是说，截图/打印/CI 像素测试都不需要真实窗口。
#pragma once

#include "pyui/painter.h"
#include "pyui/types.h"
#include "pyui/view_group.h"

#include <string>
#include <vector>

namespace pyui {

class ViewRoot {
 public:
  ViewRoot() = default;
  ~ViewRoot();

  // 内容根。所有权转移给 ViewRoot。
  void setContentView(std::unique_ptr<ViewGroup> content);
  ViewGroup* contentView() const { return content_.get(); }

  // ── 尺寸与 DPI ──
  void setSize(Size size);
  Size size() const { return size_; }
  void setDpi(int dpi) { dpi_ = dpi > 0 ? dpi : 96; }
  int dpi() const { return dpi_; }
  int px(int designValue) const;

  // ── 主题 ──
  void setTheme(const Theme* theme);

  // ── 布局 ──
  // 按当前尺寸重新测量并摆放整棵树。内容根撑满 ViewRoot。
  void performLayout();
  // 请求重新布局：控件改尺寸/内边距时调用。已挂载时立即重排，
  // 未挂载时什么都不做（构造阶段的 setter 不该触发整树布局）。
  void requestLayout();

  // ── 绘制 ──
  // painter 的原点应已对齐到窗口客户区左上角。dirty 是客户区坐标。
  // 会先清背景，再递归绘制内容。
  void draw(Painter& painter, const Rect& dirty);

  // 完整重绘的便捷入口（离线渲染、截图）
  void drawAll(Painter& painter);

  // ── 脏区 ──
  // 请求重绘；回调在宿主决定何时生效（屏幕后端 → InvalidateRect，
  // 离线渲染 → 立即下次 draw 时整块重画）
  void invalidate();
  void invalidateRect(const Rect& windowRect);
  // 取出并清空累积的脏区。返回 false 表示无脏区。
  bool consumeDirtyRect(Rect& out);
  void clearDirty();

  // 宿主回调：需要重绘时通知（屏幕后端据此 InvalidateRect）
  using InvalidateHandler = std::function<void(const Rect&)>;
  void setInvalidateHandler(InvalidateHandler fn) { onInvalidate_ = std::move(fn); }

  // ── 焦点 ──
  void setFocusedView(View* view);
  View* focusedView() const { return focused_; }
  // 按 Tab 顺序找下一个可聚焦视图（深度优先）
  View* findNextFocusable(View* from, bool forward = true);

  // ── 输入分发 ──
  // 窗口坐标的鼠标事件：内部转成命中视图的局部坐标再投递
  bool dispatchMouseDown(const MouseEvent& e);
  void dispatchMouseUp(const MouseEvent& e);
  void dispatchMouseMove(const MouseEvent& e);
  void dispatchMouseLeave();
  bool dispatchMouseWheel(const MouseEvent& e);
  void dispatchContextMenu(const MouseEvent& e);
  void dispatchDoubleClick(const MouseEvent& e);
  bool dispatchKeyDown(const Key& key, const Modifiers& mods);
  bool dispatchChar(std::uint32_t codepoint);

  // 悬停 / 按下 / 鼠标捕获：全树只有一份状态，由这里维护。
  // 控件不自跟踪 enter/leave —— 那一定会漏（指针移出窗口、被别的窗口抢走、
  // 布局变化后控件移走都不发 leave），于是悬停色永远不消失。
  View* hotView() const { return hot_; }
  View* pressedView() const { return pressed_; }

  // 命中测试：窗口坐标 → 最深的可交互视图
  View* hitTest(Point windowPoint) const;

  // 按窗口角色触发一次点击（窗口坐标）。
  //
  // role 是**布局数据**，控件本身只是普通 Button，它不知道窗口存在。内置
  // 标题栏由 TitleBar 自己接好三个动作，应用自定义布局时没人接——按钮画出来
  // 却点不动。AppWindow 在装配阶段用 bindWindowRoles 给它们挂上 onClick，
  // 这里则是「按坐标找角色并执行」的入口，供不经过按下/抬起配对的场景使用。
  //
  // 返回 true 表示命中的控件带窗口角色、点击已执行。
  bool dispatchWindowRoleClick(Point windowPoint);

  // ── 计时 ──
  // 由宿主周期性调用（60 Hz 或按需），驱动需要动画的控件
  void dispatchTimer();

  // 调试：绘制一次并返回根视图的完整绝对矩形
  Rect contentBounds() const;

 private:
  void attachContent();
  void detachContent();
  // 切换悬停目标，并让新旧两方都重绘。
  void setHotView(View* target);

  std::unique_ptr<ViewGroup> content_;
  Size size_{};
  int dpi_ = 96;
  const Theme* theme_ = nullptr;

  View* focused_ = nullptr;

  // 累积脏区（客户区坐标）。空表示无脏区。
  Rect dirty_{};
  bool hasDirty_ = false;
  // 有布局请求待处理。批量 setter 期间只置位不重排，
  // 下一次 draw 前统一处理一次——否则改 10 个属性就重排 10 遍。
  bool layoutRequested_ = false;
  InvalidateHandler onInvalidate_;

  // 悬停与按下：全树各一份，避免控件各自跟踪时漏掉 leave
  View* hot_ = nullptr;
  View* pressed_ = nullptr;
  // 鼠标按下时命中的视图；用于判断「在同一视图上按下又松开」才是点击
  View* clickCandidate_ = nullptr;
  bool pressInside_ = false;
};

}  // namespace pyui
