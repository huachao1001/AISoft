// SidePanel / PanelHeader 实现
#include "AiUI/widgets/side_panel.h"

#include "AiUI/font.h"
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

// ───────────────────────── PanelHeader ─────────────────────────

void PanelHeader::setStyle(const PanelHeaderStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

void PanelHeader::setCollapsed(bool collapsed) {
  if (collapsed_ == collapsed) return;
  collapsed_ = collapsed;
  // 只换按钮方向：它是「下一步会怎样」的提示
  invalidate();
}

// 排法跟「实际宽度」走，而不是跟目标态走：动画过程中宽度一路收窄，
// 横排放不下时自然切成居中摆放，看着是被挤出去的，不会突然跳一下。
bool PanelHeader::isNarrow() const {
  const int d = dpi();
  return clientRect().width() < style_.collapsedWidth * d / 96 * 2;
}

Rect PanelHeader::localToggleRect() const {
  const int d = dpi();
  const Rect rc = clientRect();
  // 固定的小方块按钮；窄边上先保证不越界
  const int side = (std::max)(1, (std::min)(22 * d / 96, rc.width() - 4 * d / 96));
  if (isNarrow()) {
    // 窄边就是个方块：按钮居中，四周留白对称
    const int x = (rc.width() - side) / 2;
    const int y = (rc.height() - side) / 2;
    return Rect{x, y, x + side, y + side};
  }
  const int right = rc.right - style_.padRight * d / 96;
  const int centerY = rc.center().y;
  return Rect{right - side, centerY - side / 2, right, centerY + side / 2};
}

Rect PanelHeader::toggleRect() const { return localToggleRect(); }

Size PanelHeader::onMeasure(const Size& available) {
  // 宽度由 SidePanel 独占下发（它自己摆子视图），这里只报自然高度
  return Size{available.width, style_.height * dpi() / 96};
}

void PanelHeader::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Theme& t = theme();
  const Color iconColor = t.iconMuted;

  // 分隔线压在朝内容那一侧：顶栏在底部，线就在它上面
  if (!isNarrow()) {
    painter.drawHLine(rect.left, rect.right, rect.top, t.line);
  }

  const Rect button = localToggleRect();
  if (hot_ || pressed_) {
    const Color hoverBg = style_.buttonHover != kNoColor ? style_.buttonHover : t.btnHoverBg;
    painter.fillRoundRect(button, hoverBg, 4 * d / 96);
  }

  // 展开态按钮指向左（收起），折叠态指向右（展开）：
  // kChevron 本身是「右」，所以展开时转 180°。绕按钮中心转，
  // 否则图标会跑到原点那一侧去。
  const Rect iconRect = button.inset(5 * d / 96, 5 * d / 96, 5 * d / 96, 5 * d / 96);
  painter.save();
  if (!collapsed_) {
    painter.translate(button.center().x, button.center().y);
    painter.rotate(180.0f);
    painter.drawIcon(IconId::kChevron,
                     Rect{-iconRect.width() / 2, -iconRect.height() / 2, iconRect.width() / 2,
                          iconRect.height() / 2},
                     iconColor);
  } else {
    painter.drawIcon(IconId::kChevron, iconRect, iconColor);
  }
  painter.restore();
}

bool PanelHeader::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  if (!localToggleRect().contains(e.position)) return false;
  pressed_ = true;
  invalidate();
  return true;
}

void PanelHeader::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!pressed_) return;
  pressed_ = false;
  invalidate();
  if (onToggle_) onToggle_();
}

void PanelHeader::onMouseMove(const MouseEvent& e) {
  const bool hot = localToggleRect().contains(e.position);
  if (hot == hot_) return;
  hot_ = hot;
  setCursor(hot ? CursorShape::kHand : CursorShape::kArrow);
  invalidate();
}

void PanelHeader::onMouseLeave() {
  if (!hot_ && !pressed_) return;
  hot_ = false;
  pressed_ = false;
  invalidate();
}

Color PanelHeader::backgroundColor() const {
  if (style_.background != kNoColor) return style_.background;
  return theme().headerBg;
}

// ───────────────────────── SidePanel ─────────────────────────

SidePanel::~SidePanel() {
  // 子视图归 ViewGroup 所有，这里只断开引用
  header_ = nullptr;
  body_ = nullptr;
}

void SidePanel::ensureHeader() {
  if (header_) return;
  header_ = new PanelHeader();
  header_->setId(makeId("panelHeader"));
  header_->setOnToggle([this] { setCollapsed(!collapsed_); });
  // 顶栏先加：折叠动画里它是唯一还看得见的部件
  addView(header_, LayoutParams{}.withSize(LayoutParams::kMatchParent, LayoutParams::kWrapContent));
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

int SidePanel::targetWidth() const {
  if (!collapsed_) return expandedWidth_;
  const PanelHeaderStyle hs = header_ ? header_->style() : PanelHeaderStyle{};
  return hs.collapsedWidth;
}

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
    if (header_) header_->setCollapsed(collapsed);
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
    if (header_) header_->setCollapsed(collapsed);
    if (body_) body_->show(!collapsed);
    if (onCollapsedChanged_) onCollapsedChanged_();
    return;
  }
  // 只先换按钮方向（它是「下一步会怎样」的提示），标题排法留给宽度自己决定：
  // 宽度一路收窄，横排放不下时自然转竖排，看着是被挤出去而不是跳一下。
  if (header_) header_->setCollapsed(collapsed);
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
    // 收完之后才换顶栏形态、藏内容：中途换会让画面先闪一下
    if (header_) header_->setCollapsed(collapsed_);
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
  const int d = dpi();
  if (header_) {
    // 顶栏贴在底部。高度不超过宽度：收成窄边后它就是底下一个方块，
    // 宽度一路收窄时高度跟着收敛，不会在动画收尾那一刻突然变矮。
    const int designH = header_->style().height * d / 96;
    const int h = (std::min)({designH, frame.width(), frame.height()});
    header_->layout(Rect{0, frame.height() - h, frame.width(), frame.height()});
  }
  if (!body_) return;

  const int bottom = header_ ? header_->bounds().top : frame.height();
  // 动画期间内容保持展开宽度：窄下来的是「窗口」，不是里面的卡片，
  // 看起来才是被推出去，而不是被压扁。折叠完成后它本来就隐藏了。
  const int bodyWidth =
      animating_ ? (std::max)(frame.width(), expandedWidth_ * d / 96) : frame.width();
  body_->layout(Rect{0, 0, bodyWidth, (std::max)(0, bottom)});
}

}  // namespace AiUI
