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

void PanelHeader::setText(const std::wstring& text) {
  if (text_ == text) return;
  text_ = text;
  // 折叠时是逐字竖排，字变了高度就变，必须重画
  invalidate();
}

void PanelHeader::setHint(const std::wstring& hint) { setTooltip(hint); }

void PanelHeader::setStyle(const PanelHeaderStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

void PanelHeader::setCollapsed(bool collapsed) {
  if (collapsed_ == collapsed) return;
  collapsed_ = collapsed;
  // 按钮换位置与方向；标题排法不在这里定，交给宽度自己决定
  requestLayout();
  invalidate();
}

// 排法跟「实际宽度」走，而不是跟目标态走：动画过程中宽度一路收窄，
// 横排放不下时自然转竖排，看着是被挤出去的，不会突然跳一下。
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
    const int x = (rc.width() - side) / 2;
    const int y = 4 * d / 96;
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
  const Color textColor = style_.text != kNoColor ? style_.text : t.textHead;
  const Color iconColor = t.iconMuted;

  // 还有横排空间时底边压一条分隔线，跟下面的缩略图区分开
  if (!isNarrow()) {
    painter.drawHLine(rect.left, rect.right, rect.bottom - 1, t.line);
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

  const Font& font = fonts.uiSmall;

  if (isNarrow()) {
    // 竖排：窄边上横着放不下目录名，逐字往下至少还能认出是哪个目录
    int y = button.bottom + style_.titleGap * d / 96;
    const int step = font.pixelHeight() + d * 2 / 96;
    for (wchar_t ch : text_) {
      if (y + step > rect.bottom) break;
      const Rect line{rect.left, y, rect.right, y + step};
      painter.drawText(line, std::wstring(1, ch), textColor, font, TextAlign::kCenter);
      y += step;
    }
    return;
  }

  const Rect titleRect{rect.left + style_.padLeft * d / 96, rect.top,
                       button.left - style_.titleGap * d / 96, rect.bottom};
  if (titleRect.width() > 0) {
    // 目录名可能很长：尾部省略，完整路径交给悬停提示
    painter.drawTextEllipsis(titleRect, text_, textColor, font);
  }
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
  const bool collapsedNow = collapsed_ && !animating_;
  if (header_) {
    // 展开时只占顶上一条；折叠收尾后就是整条窄边，竖排文字要一直排到底
    const int h = collapsedNow ? frame.height() : header_->style().height * d / 96;
    header_->layout(Rect{0, 0, frame.width(), (std::min)(h, frame.height())});
  }
  if (!body_) return;

  const int top = header_ ? header_->bounds().bottom : 0;
  // 动画期间内容保持展开宽度：窄下来的是「窗口」，不是里面的卡片，
  // 看起来才是被推出去，而不是被压扁。折叠完成后它本来就隐藏了。
  const int bodyWidth =
      animating_ ? (std::max)(frame.width(), expandedWidth_ * d / 96) : frame.width();
  body_->layout(Rect{0, top, bodyWidth, frame.height()});
}

}  // namespace AiUI
