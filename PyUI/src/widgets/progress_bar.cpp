#include "pyui/widgets/progress_bar.h"

#include "pyui/text_renderer.h"

#include <algorithm>
#include <cstdio>

namespace pyui {

void ProgressBar::setProgress(float value) {
  const float clamped = std::max(0.0f, std::min(1.0f, value));
  if (clamped == progress_) return;
  progress_ = clamped;
  // 给了确定进度就退出不确定态：两者同时为真时画哪一条都自相矛盾
  indeterminate_ = false;
  invalidate();
}

void ProgressBar::setIndeterminate(bool indeterminate) {
  if (indeterminate_ == indeterminate) return;
  indeterminate_ = indeterminate;
  if (indeterminate) phase_ = 0.0f;
  invalidate();
}

Size ProgressBar::onMeasure(const Size& available) {
  const Font font = uiFont(9.0f, false, false, dpi());
  const int track = px(thickness_);
  // 有文字时整块要比轨道高一行，否则百分比会压在轨道上
  const bool hasText = showPercent_ || !label_.empty();
  const int height = hasText ? std::max(track, textLineHeight(font)) : track;
  int width = available.width > 0 ? available.width : px(160);
  if (available.height > 0) {
    return Size{width, std::min(height, available.height)};
  }
  return Size{width, height};
}

void ProgressBar::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const Font font = uiFont(9.0f, false, false, dpi());
  const bool hasText = showPercent_ || !label_.empty();

  // 有文字时文字与轨道左右分列：把百分比压在轨道中央会让填充色与
  // 文字抢位置，深色主题下尤其读不清。
  const int trackHeight = px(thickness_);
  const int lineHeight = textLineHeight(font);
  const int labelWidth = hasText ? std::min(px(56), std::max(0, rect.width() / 2)) : 0;
  const int gap = hasText ? px(6) : 0;

  const int top = rect.top + (rect.height() - trackHeight) / 2;
  const Rect track{rect.left, top, rect.right - labelWidth - gap, top + trackHeight};
  if (track.width() <= 0 || track.height() <= 0) return;

  const int radius = trackHeight / 2;
  painter.fillRoundRect(track, t.trackBg, radius);

  if (indeterminate_) {
    // 不确定态画一段匀速前进的短条，而不是来回扫的亮条：
    // 前者每帧只需一次圆角填充，视觉上同样表达「正在进行、进度未知」。
    const int segment = std::max(px(24), std::min(track.width(), track.width() / 4));
    const int travel = std::max(0, track.width() - segment);
    const int start = track.left + static_cast<int>(static_cast<float>(travel) * phase_);
    const Rect bar{start, track.top, start + segment, track.bottom};
    // 越界部分先夹到轨道内再画，圆角才不会在右边界外露出一截
    const Rect visible = bar.intersect(track);
    if (!visible.isEmpty()) painter.fillRoundRect(visible, t.trackFill, radius);
  } else {
    const int filled = static_cast<int>(static_cast<float>(track.width()) * progress_ + 0.5f);
    if (filled > 0) {
      // 填充用强调色到 accentDown 的垂直渐变：纯色填充在长条上显得很平
      painter.drawGradientRoundRect(
          Rect{track.left, track.top, track.left + filled, track.bottom},
          blendColor(t.accent, makeColor(0xff, 0xff, 0xff), 0.18f), t.accentDown, radius);
    }
  }

  if (!hasText) return;

  std::string text = label_;
  if (text.empty()) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%d%%", static_cast<int>(progress_ * 100.0f + 0.5f));
    text = buffer;
  }

  const Rect textRect{rect.right - labelWidth, rect.top + (rect.height() - lineHeight) / 2,
                      rect.right, rect.top + (rect.height() + lineHeight) / 2};
  painter.drawText(textRect, text, t.textMuted, font, TextAlign::kRight);
}

void ProgressBar::onTimer() {
  if (!indeterminate_) return;
  // 一秒走完一个循环：太快像故障闪烁，太慢会让人以为卡死了
  phase_ += 1.0f / 60.0f;
  if (phase_ > 1.0f) phase_ -= 1.0f;
  invalidate();
}

}  // namespace pyui
