// Slider：轨道 + 滑块，支持步进与键盘调整
//
// 数值一律用 double 并在内部按 step 量化：float 在 0..1 上做 100 步会累积
// 误差，用户把滑块拖到最右却显示 99% 就是这么来的。
#pragma once

#include "pyui/painter.h"
#include "pyui/view.h"

#include <functional>

namespace pyui {

class Slider : public View {
 public:
  Slider() { setFocusable(true); }

  void setRange(double minimum, double maximum);
  double minimum() const { return minimum_; }
  double maximum() const { return maximum_; }

  // 单端修改：XML 里 min 与 max 是两条独立属性，生成代码逐条调用。
  // 两者都走 setRange 的重载而不是各自赋值，是为了共用「夹住旧值 + 量化」
  // 这一套收尾逻辑——分开写必然有一处漏掉。
  void setMinimum(double minimum) { setRange(minimum, maximum_); }
  void setMaximum(double maximum) { setRange(minimum_, maximum); }

  // 值会被夹到 [minimum, maximum] 并按 step 量化
  void setValue(double value);
  double value() const { return value_; }

  // 步进；0 表示连续
  void setStep(double step) { step_ = step > 0.0 ? step : 0.0; }
  double step() const { return step_; }

  void setEnabled(bool enabled) override;

  using ChangeHandler = std::function<void(double)>;
  // 拖动过程中持续回调；松手时再回调一次（值相同则不重复）
  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }
  // 只在意最终结果时用这个：拖动结束、键盘调整后各回调一次
  void setOnCommit(ChangeHandler fn) { onCommit_ = std::move(fn); }

  CursorShape cursorShape() const override { return CursorShape::kHand; }

  const char* className() const override { return "Slider"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;

 private:
  // 可拖动范围（去掉滑块半径的两端），物理像素
  Rect trackRect(const Rect& rect) const;
  int thumbRadius() const;
  // 值 → 滑块中心 x（相对本视图左缘）
  int valueToX(double value, const Rect& rect) const;
  // x → 值
  double xToValue(int x, const Rect& rect) const;
  // 从鼠标位置更新值；返回是否变化
  bool updateFromPointer(int x, const Rect& rect);
  void quantize();
  void notifyChanged();
  void notifyCommitted();

  double minimum_ = 0.0;
  double maximum_ = 1.0;
  double value_ = 0.0;
  double step_ = 0.0;

  bool dragging_ = false;

  ChangeHandler onChange_;
  ChangeHandler onCommit_;
};

}  // namespace pyui
