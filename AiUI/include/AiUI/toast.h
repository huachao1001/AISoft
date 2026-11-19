// Toast：短暂的屏幕提示
//
// 由 ViewRoot 持有，一个窗口一条；任何控件都能发起：
//   showToast(L"已复制 rgba(86, 120, 30, 255)");
//
// 三条设计取舍：
//   - 画在最上层但不吃鼠标：它只是回执，点它、划它都不该吞掉事件。
//   - 同一时刻只有一条：新消息顶掉旧的并重新计时。多条叠着会让「刚才那条
//     说了什么」难以阅读，也占屏幕。
//   - 计时挂在 ViewRoot 唯一的单次定时器上：淡入淡出要逐帧，但 hold 阶段
//     不用重绘，所以 tick() 返回的只是「下一次需要醒来的时间」。
#pragma once

#include "AiUI/painter.h"

#include <cstdint>
#include <string>

namespace AiUI {

class Toast {
 public:
  struct Timing {
    int holdMs = 1400;  // 完全显示多久
    int fadeMs = 160;   // 淡出时长（消失保持原地淡出）
    // 弹出时长：上浮 + 过冲回弹。比 fadeMs 略长，才有「弹出来」的行程感；
    // 淡入透明度仍按 fadeMs 走完，行程还在继续，观感上更活。
    int riseMs = 260;
  };

  // 弹一条提示；已有提示会被顶掉并重新计时
  void show(const std::wstring& text);
  // 立刻收起，不进淡出
  void hide();

  bool isVisible() const { return phase_ != Phase::kHidden; }
  void setTiming(const Timing& timing) { timing_ = timing; }
  const Timing& timing() const { return timing_; }

  // 当前该占的区域（客户区坐标）。弹出阶段会带上上浮偏移，是这一帧的实际位置。
  Rect bounds(const Rect& clientArea, int dpi) const;
  // 刷新用的稳定区域：落位框与上浮到底时的框取并集，覆盖整段行程。
  //
  // 必须用它来标脏，不能用 bounds()：绘制发生在标脏之后十几毫秒，那时
  // 上浮已经又走了一段，按瞬时位置标脏会漏掉被扫过的那一条，留下残影。
  Rect dirtyBounds(const Rect& clientArea, int dpi) const;
  // 画自身到客户区坐标；不透明度按当前时间算，调用方无需传 alpha
  void paint(Painter& painter, const Rect& clientArea, int dpi) const;

  // 推进到 now（到点就切下一阶段）。
  // 返回下一次需要重绘的绝对毫秒；< 0 表示不必再醒。
  int64_t tick(int64_t now);

 private:
  enum class Phase { kHidden, kFadingIn, kHolding, kFadingOut };

  float alphaAt(int64_t now) const;
  // 落位框（不含上浮偏移）
  Rect settledBounds(const Rect& clientArea, int dpi) const;
  // 弹出阶段相对最终位置的竖直偏移（正数表示还在下方）。
  // 淡出时恒为 0：消失保持原地淡出，不再上下动。
  int riseOffsetAt(int64_t now, int dpi) const;

  std::wstring text_;
  Timing timing_;
  Phase phase_ = Phase::kHidden;
  int64_t phaseStartMs_ = 0;
  int64_t phaseEndMs_ = 0;
};

}  // namespace AiUI
