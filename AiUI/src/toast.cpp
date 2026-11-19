// Toast 实现：状态由时间解析算出，绘制不依赖任何控件
#include "AiUI/toast.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

#include <algorithm>
#include <cmath>

namespace AiUI {
namespace {

// 尺寸按 96 DPI 设计，绘制时按实际 DPI 放大
constexpr int kTextPx = 15;
constexpr int kPadX = 16;
constexpr int kPadY = 9;
// 与窗口底边留出的距离：贴边会显得挤
constexpr int kBottomGap = 48;
constexpr int kSideMargin = 24;
// 底色比缩放浮层略实：它是一句需要读清楚的话
constexpr std::uint8_t kBgAlpha = 0xcc;
// 圆角半径按高度的比例取：四角柔和，不是胶囊。
// 高度 33px 时约 7px，一眼能看出圆角；取半高会变成胶囊，短提示像个按钮。
constexpr int kCornerPercent = 22;  // 百分比：0.22 * 高度
// 淡入淡出的帧间隔
constexpr int kFrameMs = 16;
// 弹出时的上浮距离（96 DPI 下的像素）：从下方弹起，带一点回弹
constexpr int kRiseDistance = 14;

float easeInOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

// 弹出曲线：起步快、末尾带过冲再落回。
// 用 back-out 的经典形式：t=0 时 0，t=1 时恰好 1，中途超过 1。
float easeBackOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  const float c1 = 1.70158f;
  const float c3 = c1 + 1.0f;
  const float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

int px(int designPx, int dpi) { return (std::max)(1, designPx * (std::max)(96, dpi) / 96); }

}  // namespace

void Toast::show(const std::wstring& text) {
  text_ = text;
  phase_ = Phase::kFadingIn;
  phaseStartMs_ = platform()->monotonicMillis();
  // 透明度走完 fadeMs 后不再变，但上浮可能还在继续，所以淡入阶段要覆盖两者
  // 中更长的那个，否则上浮会被提前截断、在落位前突然跳到终点。
  phaseEndMs_ = phaseStartMs_ + (std::max)((std::max)(1, timing_.fadeMs),
                                          (std::max)(1, timing_.riseMs));
}

void Toast::hide() {
  text_.clear();
  phase_ = Phase::kHidden;
  phaseStartMs_ = 0;
  phaseEndMs_ = 0;
}

float Toast::alphaAt(int64_t now) const {
  switch (phase_) {
    case Phase::kHidden:
      return 0.0f;
    case Phase::kHolding:
      return 1.0f;
    case Phase::kFadingIn: {
      const float span = static_cast<float>((std::max)(1, timing_.fadeMs));
      return easeInOut(static_cast<float>(now - phaseStartMs_) / span);
    }
    case Phase::kFadingOut: {
      const float span = static_cast<float>((std::max)(1, timing_.fadeMs));
      return 1.0f - easeInOut(static_cast<float>(now - phaseStartMs_) / span);
    }
  }
  return 0.0f;
}

int64_t Toast::tick(int64_t now) {
  if (phase_ == Phase::kHidden) return -1;
  if (now < phaseEndMs_) {
    // 淡入淡出要逐帧；完全显示期间没有任何变化，睡到阶段结束即可
    if (phase_ == Phase::kHolding) return phaseEndMs_;
    return now + kFrameMs;
  }

  switch (phase_) {
    case Phase::kFadingIn:
      phase_ = Phase::kHolding;
      phaseStartMs_ = now;
      phaseEndMs_ = now + (std::max)(0, timing_.holdMs);
      // holdMs 为 0 时直接进入淡出，别在这一帧就把状态锁住
      if (timing_.holdMs <= 0) return tick(now);
      return phaseEndMs_;
    case Phase::kHolding:
      phase_ = Phase::kFadingOut;
      phaseStartMs_ = now;
      phaseEndMs_ = now + (std::max)(1, timing_.fadeMs);
      return now + kFrameMs;
    case Phase::kFadingOut:
      hide();
      return -1;
    case Phase::kHidden:
      return -1;
  }
  return -1;
}

int Toast::riseOffsetAt(int64_t now, int dpi) const {
  // 只有淡入阶段才做上浮：消失保持原地淡出，免得「往下掉」像被收走
  if (phase_ != Phase::kFadingIn) return 0;
  const float span = static_cast<float>((std::max)(1, timing_.riseMs));
  const float progress = easeBackOut(static_cast<float>(now - phaseStartMs_) / span);
  // easeBackOut 会略微超过 1（过冲）：换算成「从下方 distance 处弹到 0 再略微越过」
  const int distance = px(kRiseDistance, dpi);
  const float traveled = progress - 1.0f;  // -1 → 0，过冲时 > 0
  // traveled 为 -1 时在下方 distance 处，为 0 时落位，>0 时略微向上越过
  return static_cast<int>(std::lround(-traveled * static_cast<float>(distance)));
}

// 落位框：不含上浮偏移的最终位置
Rect Toast::bounds(const Rect& clientArea, int dpi) const {
  const Rect settled = settledBounds(clientArea, dpi);
  if (settled.isEmpty()) return Rect{};
  // 弹出阶段整块从下方滑上来，这里叠加这一帧的偏移：绘制按它落笔
  const int rise = riseOffsetAt(platform()->monotonicMillis(), dpi);
  return settled.moved(0, rise);
}

Rect Toast::settledBounds(const Rect& clientArea, int dpi) const {
  if (phase_ == Phase::kHidden || text_.empty()) return Rect{};

  const int textPx = px(kTextPx, dpi);
  const Font& font = fonts.forPixelSize(textPx);
  if (!font.isValid()) return Rect{};
  // 用真实文本宽度：中文与 ASCII 混排时按字数估会明显偏窄
  const int textWidth = platform()->measureText(text_, font);

  const int padX = px(kPadX, dpi);
  const int padY = px(kPadY, dpi);
  const int gap = px(kBottomGap, dpi);
  const int margin = px(kSideMargin, dpi);

  const int boxH = textPx + padY * 2;
  const int maxW = (std::max)(0, clientArea.width() - margin * 2);
  const int boxW = (std::min)(textWidth + padX * 2, maxW);
  if (boxW <= 0 || boxH <= 0) return Rect{};

  const int left = clientArea.center().x - boxW / 2;
  const int bottom = clientArea.bottom - gap;
  return Rect{left, bottom - boxH, left + boxW, bottom};
}

Rect Toast::dirtyBounds(const Rect& clientArea, int dpi) const {
  const Rect settled = settledBounds(clientArea, dpi);
  if (settled.isEmpty()) return Rect{};
  // 上浮行程整段都算进脏区：起点在落位位置下方 rise 处，过冲时会短暂高于
  // 落位位置。绘制比标脏晚十几毫秒，按瞬时位置标脏必漏边，所以两头都留。
  const int rise = px(kRiseDistance, dpi);
  return settled.inflated(0, rise);
}

void Toast::paint(Painter& painter, const Rect& clientArea, int dpi) const {
  const Rect box = bounds(clientArea, dpi);
  if (box.isEmpty()) return;

  const float alpha = alphaAt(platform()->monotonicMillis());
  if (alpha <= 0.0f) return;

  const std::uint8_t bgAlpha =
      static_cast<std::uint8_t>(static_cast<float>(kBgAlpha) * alpha + 0.5f);
  const std::uint8_t fgAlpha = static_cast<std::uint8_t>(255.0f * alpha + 0.5f);
  // 圆角按高度的两成多取：四角柔和但不变成胶囊
  const int radius = (std::max)(1, box.height() * kCornerPercent / 100);
  painter.fillRoundRect(box, makeColor(0, 0, 0, bgAlpha), radius);

  const Font& font = fonts.forPixelSize(px(kTextPx, dpi));
  if (!font.isValid()) return;
  painter.drawText(box, text_, makeColor(0xff, 0xff, 0xff, fgAlpha), font, TextAlign::kCenter);
}

}  // namespace AiUI
