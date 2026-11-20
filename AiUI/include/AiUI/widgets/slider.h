// 滑块：连续数值的拖动调节
//
// 裁剪比例、JPEG 质量、水印透明度这类参数要一边拖一边看效果，输入框做不到
// 「拖到合适就停」。数值刻意用 0..1 的比例而不是任意区间：调用方自己乘一个
// 量程，控件就不必再管步长、吸附这些分支。
#pragma once

#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <functional>

namespace AiUI {

// 96 DPI 设计值
struct SliderSpec {
  int height = 28;  // 控件总高（含滑块）
  int trackHeight = 4;
  int trackRadius = 2;
  int thumbRadius = 7;
  Color track = kNoColor;  // 未选中段，默认走主题 lineSoft
  Color fill = kNoColor;   // 已选中段，默认走主题 link
  Color thumb = kNoColor;  // 滑块，默认走主题 link
};

// 拖动中要实时出图，所以 change 回调每次移动都发；松开另有钩子，
// 方便调用方在那时做「提交结果」这类一次性的活。
class Slider : public View {
 public:
  using ChangeHandler = std::function<void(float)>;

  void setSpec(const SliderSpec& spec);

  float value() const { return value_; }
  // 超出范围直接夹住，不报错：调用方从配置里读回来的值可能已经越界
  void setValue(float value, bool notify = false);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }
  void setOnRelease(ChangeHandler fn) { onRelease_ = std::move(fn); }

  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

 protected:
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& modifiers) override;

 private:
  // 滑块圆心的可取范围要减去半径，否则拖到两端时滑块会半个出界
  int thumbCenterX(const Rect& rect) const;
  void valueFromX(int x, const Rect& rect);
  void applyValue(float value, bool notify);

  SliderSpec spec_;
  float value_ = 0.0f;
  bool dragging_ = false;
  bool hovered_ = false;

  ChangeHandler onChange_;
  ChangeHandler onRelease_;
};

}  // namespace AiUI
