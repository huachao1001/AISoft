// ViewRoot：整棵视图树唯一的窗口与绘制管理者
//
// 与 Android 的 ViewRootImpl 对应：
//   - 只有它创建原生窗口，所有 View 画进同一张位图；
//   - invalidate / invalidateRect 交给系统更新区，WM_PAINT 里只重画那块；
//   - 负责 measure/layout 的时机，把窗口事件翻译成视图局部坐标；
//   - 统一维护焦点、鼠标捕获、悬停与光标。
//
// 一个窗口一个 ViewRoot；子树不持有任何原生句柄。
#pragma once

#include "AiUI/platform.h"
#include "AiUI/view.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace AiUI {

class ViewRoot : public WindowHost {
 public:
  ViewRoot();
  ~ViewRoot() override;

  ViewRoot(const ViewRoot&) = delete;
  ViewRoot& operator=(const ViewRoot&) = delete;

  // 挂到宿主窗口；bounds 为窗口客户区矩形
  bool attach(NativeHandle parent, const Rect& bounds = Rect{});
  // 带窗口参数（顶层/边框/最小尺寸）的挂载；Window 组件用这个
  bool attach(NativeHandle parent, const WindowParams& params, const Rect& bounds = Rect{});
  bool isAttached() const { return window_ != nullptr; }
  NativeHandle handle() const;
  // 底层平台窗口；自绘标题栏需要它做拖动与缩放
  PlatformWindow* platformWindow() const { return window_; }
  // 销毁平台窗口并断开引用；之后可重新 attach
  void destroyWindow();

  // 关闭请求；返回 false 阻止关闭
  using CloseHandler = std::function<bool()>;
  void setCloseHandler(CloseHandler fn) { onClose_ = std::move(fn); }
  // Esc 是否关闭窗口；主窗口通常关闭它，对话框保留
  void setEscapeCloses(bool closes) { escapeCloses_ = closes; }
  // 平台窗口收到关闭请求时调用（Esc、Alt+F4、关闭按钮都走这里）
  void requestClose();
  // 最大化状态变化通知；自绘标题栏据此切换按钮图标
  void setMaximizeStateHandler(std::function<void(bool)> fn) { onMaximizeState_ = std::move(fn); }

  // 设置根视图；不接管所有权，调用方负责生命周期
  void setContentView(View* view);
  View* contentView() const { return content_; }

  // 窗口尺寸变化后重新测量与摆放
  void layout(const Rect& bounds);
  void requestLayout();
  int dpi() const;
  Rect clientRect() const;

  // ── 刷新入口 ──
  void invalidate();
  // 窗口坐标矩形；内部与客户区求交
  void invalidateRect(const Rect& windowRect);

  // ── 焦点与光标 ──
  View* focused() const { return focused_; }
  void requestFocus(View* view);
  void clearFocus();
  // 光标闪烁定时器；整棵树共用一个
  void setBlinkTimer(bool on);
  // 一次性延时：ms 毫秒后通知 view（0 表示取消该 view 的等待）
  void postDelayed(View* view, int ms);
  // 输入法候选框跟随光标（窗口坐标）
  void setImeCaret(int x, int y);

  // ── 鼠标捕获 ──
  void capture(View* view);
  void releaseCapture(View* view);
  // 依据目标视图的 cursorShape 设置系统光标
  void applyCursor(View* view);

  void setBackgroundColor(Color color);

  // ── WindowHost ──
  void onExpose(const Rect& update) override;
  void onResize(int width, int height) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onMouseWheel(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  void onContextMenu(const MouseEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  bool onChar(wchar_t ch) override;
  void onFocusChanged(bool focused) override;
  void onTimer() override;
  void onDelayTimer() override;
  HitTarget onHitTest(const Point& position) override;
  bool onCloseRequest() override;

 private:
  // 找到窗口坐标下最上层的可命中视图
  View* hitTest(Point windowPoint) const;
  // 把窗口坐标事件转成目标视图的局部坐标
  MouseEvent localEvent(View* target, const MouseEvent& e) const;
  // 按客户区重新测量与摆放根视图
  void performLayout(const Rect& area);
  // 切换悬停目标：离开的发 leave，进入的发 move
  void setHovered(View* target, const MouseEvent* e);
  // 把焦点转给某视图或它内部第一个可聚焦的后代
  View* findFocusable(View* view) const;
  // view 是否位于 ancestor 子树内（含自身）
  bool isDescendantOf(const View* view, const View* ancestor) const;
  // 按最早到期的任务重装唯一的单次定时器
  void scheduleNextDelayed();

  PlatformWindow* window_ = nullptr;
  View* content_ = nullptr;
  View* focused_ = nullptr;
  View* hovered_ = nullptr;
  // 鼠标捕获（拖动滚动条、列宽等）与本次按下的目标，两者都可能跨控件收到 up
  View* capture_ = nullptr;
  View* pressed_ = nullptr;
  // 延时任务按到期顺序排列；树里可能有多个视图各自等待
  struct DelayedTask {
    View* view;
    int64_t deadline;  // 毫秒计时器，绝对时间
  };
  std::vector<DelayedTask> delayed_;

  Color background_ = kNoColor;
  bool layoutPending_ = false;
  CloseHandler onClose_;
  std::function<void(bool)> onMaximizeState_;
  bool lastMaximized_ = false;
  bool escapeCloses_ = false;
};

}  // namespace AiUI
