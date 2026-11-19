// SidePanel / PanelToggle 实现
#include "AiUI/widgets/side_panel.h"

#include "AiUI/platform.h"

#include <algorithm>
#include <cmath>

namespace AiUI {

namespace {

// 动画按帧推进；与 photo_view 保持一致
constexpr int kFrameMs = 16;

// 缓入缓出：两端收住，中段快。侧栏折叠是「一次性动作」，
// 不适合滚轮那套跟随模型（那套是给连续输入用的）。
float easeInOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

// ───────────────────────── PanelToggle ─────────────────────────

void PanelToggle::setStyle(const PanelToggleStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

void PanelToggle::setCollapsed(bool collapsed) {
  if (collapsed_ == collapsed) return;
  collapsed_ = collapsed;
  // 只换箭头方向：它是「下一步会怎样」的提示
  invalidate();
}

// 有状态源时以它为准：箭头永远反映侧栏真实状态，不必要求每条改动路径
// （点击、快捷键、自动化脚本）都记得手动同步一次。
bool PanelToggle::isCollapsed() const {
  return collapsedSource_ ? collapsedSource_() : collapsed_;
}

Rect PanelToggle::iconRect() const {
  const int d = dpi();
  const Rect rc = clientRect();
  const int side = (std::max)(1, (std::min)(style_.iconSize * d / 96, (std::min)(rc.width(), rc.height())));
  return Rect{(rc.width() - side) / 2, (rc.height() - side) / 2, (rc.width() + side) / 2,
              (rc.height() + side) / 2};
}

Size PanelToggle::onMeasure(const Size& available) {
  (void)available;
  // 方形按钮：图标四周留同样多的空白，放在状态栏里不至于比文字高
  const int d = dpi();
  const int side = style_.iconSize * d / 96 + 14 * d / 96;
  return Size{side, side};
}

void PanelToggle::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Theme& t = theme();

  if (style_.topDivider) {
    const Color line = style_.divider != kNoColor ? style_.divider : t.line;
    painter.drawHLine(rect.left, rect.right, rect.top, line);
  }

  if (hot_ || pressed_) {
    const Color bg = pressed_ && style_.downBackground != kNoColor
                         ? style_.downBackground
                         : (style_.hoverBackground != kNoColor ? style_.hoverBackground : t.btnHoverBg);
    painter.fillRoundRect(rect.inset(2 * d / 96, 2 * d / 96, 2 * d / 96, 2 * d / 96), bg, 5 * d / 96);
  }

  Color ink = style_.icon != kNoColor ? style_.icon : t.iconMuted;
  if (!isEnabled()) ink = style_.iconDisabled != kNoColor ? style_.iconDisabled : t.textFaint;

  // 展开态指左（收起），收起态指右（展开）：kChevron 本身是「右」，
  // 所以展开时转 180°。绕图标中心转，否则它会跑到原点那一侧去。
  const Rect icon = iconRect();
  painter.save();
  if (!isCollapsed()) {
    painter.translate(icon.center().x, icon.center().y);
    painter.rotate(180.0f);
    painter.drawIcon(IconId::kChevron,
                     Rect{-icon.width() / 2, -icon.height() / 2, icon.width() / 2, icon.height() / 2},
                     ink);
  } else {
    painter.drawIcon(IconId::kChevron, icon, ink);
  }
  painter.restore();
}

bool PanelToggle::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft || !isEnabled()) return false;
  pressed_ = true;
  invalidate();
  return true;
}

void PanelToggle::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!pressed_) return;
  pressed_ = false;
  invalidate();
  if (onToggle_) onToggle_();
}

void PanelToggle::onMouseMove(const MouseEvent& e) {
  const bool hot = clientRect().contains(e.position);
  if (hot == hot_) return;
  hot_ = hot;
  setCursor(hot && isEnabled() ? CursorShape::kHand : CursorShape::kArrow);
  invalidate();
}

void PanelToggle::onMouseLeave() {
  if (!hot_ && !pressed_) return;
  hot_ = false;
  pressed_ = false;
  invalidate();
}

Color PanelToggle::backgroundColor() const { return kNoColor; }

// ───────────────────────── SidePanel ─────────────────────────

SidePanel::~SidePanel() {
  // 子视图归 ViewGroup 所有，这里只断开引用
  body_ = nullptr;
}

void SidePanel::setContent(View* body) {
  if (body_ == body) return;
  // ViewGroup::removeView 会连带销毁，换内容前必须确认不再引用
  if (body_) removeView(body_);
  body_ = body;
  if (!body_) return;
  addView(body_, LayoutParams{}.withSize(LayoutParams::kMatchParent, LayoutParams::kMatchParent));
}

void SidePanel::setExpandedWidth(int width) {
  if (expandedWidth_ == width) return;
  expandedWidth_ = width;
  if (!collapsed_ && !animating_) applyWidth(width);
}

int SidePanel::targetWidth() const { return collapsed_ ? 0 : expandedWidth_; }

void SidePanel::setCollapsed(bool collapsed, bool animated) {
  if (collapsed_ == collapsed && !animating_) return;
  // 已经在往这个方向收/放：再点一次不该重启缓动，否则会先减速再加速，像顿了一下
  collapsed_ = collapsed;
  const int target = targetWidth();
  if (animating_ && animTo_ == target) return;

  // 没有窗口时没有逐帧定时器，直接落到终态
  if (!animated || !isCreated()) {
    animating_ = false;
    lastAppliedWidth_ = target;
    applyWidth(target);
    if (body_) body_->show(!collapsed);
    if (onCollapsedChanged_) onCollapsedChanged_();
    return;
  }

  // 展开时先把内容放出来，动画过程中才有东西可露
  if (body_ && !collapsed_) body_->show(true);

  const int from = layoutParams().width >= 0 ? layoutParams().width : expandedWidth_;
  animFrom_ = from;
  animTo_ = target;
  if (animFrom_ == animTo_) {
    animating_ = false;
    if (body_) body_->show(!collapsed);
    if (onCollapsedChanged_) onCollapsedChanged_();
    return;
  }
  animating_ = true;
  animStartMs_ = platform()->monotonicMillis();
  lastAppliedWidth_ = animFrom_;
  postDelayed(kFrameMs);
}

void SidePanel::onDelayed() {
  if (!animating_) return;

  const int64_t elapsed = platform()->monotonicMillis() - animStartMs_;
  const float raw = animDurationMs_ > 0
                        ? static_cast<float>(elapsed) / static_cast<float>(animDurationMs_)
                        : 1.0f;
  const bool done = raw >= 1.0f;
  const float t = easeInOut(done ? 1.0f : raw);
  // 在「设计值」上插值：布局参数存的就是设计值，不经物理像素来回换算，
  // 不会因为 150% 屏的取整在收尾时差一两像素收不住。
  const int design =
      animFrom_ + static_cast<int>(std::lround(static_cast<float>(animTo_ - animFrom_) * t));

  if (design != lastAppliedWidth_) {
    applyWidth(design);
    lastAppliedWidth_ = design;
  }

  if (done) {
    animating_ = false;
    applyWidth(animTo_);
    // 收完之后才藏内容：中途藏会让画面先空一块
    if (body_) body_->show(!collapsed_);
    if (onCollapsedChanged_) onCollapsedChanged_();
    return;
  }
  postDelayed(kFrameMs);
}

void SidePanel::applyWidth(int designWidth) {
  LayoutParams lp = layoutParams();
  if (lp.width == designWidth) return;
  lp.width = designWidth;
  setLayoutParams(lp);
}

Size SidePanel::onMeasure(const Size& available) { return available; }

void SidePanel::onLayout(const Rect& frame) {
  if (!body_) return;
  const int d = dpi();
  // 动画期间内容保持展开宽度：窄下来的是「窗口」，不是里面的卡片，
  // 看起来才是被推出去，而不是被压扁。收起完成后它本来就隐藏了。
  const int bodyWidth =
      animating_ ? (std::max)(frame.width(), expandedWidth_ * d / 96) : frame.width();
  body_->layout(Rect{0, 0, bodyWidth, frame.height()});
}

}  // namespace AiUI
