// ProgressBar：确定进度与不确定（跑马）两种
//
// 不确定态不画「来回扫的亮条」而画一段匀速前进的短条：前者在 60fps 下每帧
// 要重算整条轨道的抗锯齿，后者只需要一次圆角填充，且视觉上同样能表达
// 「正在进行、进度未知」。性能与观感都更好。
#pragma once

#include "pyui/painter.h"
#include "pyui/view.h"

#include <string>

namespace pyui {

class ProgressBar : public View {
 public:
  ProgressBar() = default;

  // 0..1。设置后自动退出不确定态。
  void setProgress(float value);
  float progress() const { return progress_; }

  // 不确定态：进度未知，画循环前进的短条
  void setIndeterminate(bool indeterminate);
  bool isIndeterminate() const { return indeterminate_; }

  // 条上文字（如 "45%"）；空则不画
  void setLabel(std::string label) { label_ = std::move(label); }
  const std::string& label() const { return label_; }
  // 是否自动显示百分比文字
  void setShowPercent(bool show) { showPercent_ = show; }

  void setTrackThickness(int designPixels) { thickness_ = designPixels; }
  int trackThickness() const { return thickness_; }

  const char* className() const override { return "ProgressBar"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onTimer() override;

 private:
  float progress_ = 0.0f;
  bool indeterminate_ = false;
  bool showPercent_ = false;
  std::string label_;
  int thickness_ = kSizeTrackH;
  // 不确定态的相位（0..1），由 onTimer 推进
  float phase_ = 0.0f;
};

}  // namespace pyui
