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

namespace {

// 侧栏折叠图标：四条圆头横杠 + 一个实心三角。
//
// 造型照「缩进」来：三角形那一侧是侧栏将要占的位置，中间两条杠让开它。
// 展开时三角在左、两条短杠缩到右边，读作「把这栏收进左边」；收起时整体
// 左右镜像，读作「再放出来」。
//
// 不用字体字形拼：字形受字体版本与墨迹框影响，笔画粗细和间距都不可控，
// 小尺寸下很容易糊成一堆。这里按固定比例画，任何尺寸都长一样。
//
// 全程用浮点坐标：抗锯齿只在边界不落在像素格上时才发生。整数坐标画出来
// 的横杠正好对齐像素行，端头是剃刀般的直角，斜边还会切出台阶——和旁边
// 那批浮点轮廓的字形图标摆一起就明显不是一路货。
void drawSidebarGlyph(Painter& painter, const Rect& box, bool collapsed, Color ink) {
  const float w = static_cast<float>(box.width());
  const float h = static_cast<float>(box.height());
  if (w <= 0.0f || h <= 0.0f) return;

  // 笔画粗细：状态栏里图标约 30 设备像素，字形图标实测约 2px。
  // 浮点多边形自带抗锯齿，实心宽度略收一点（2px + 半透明边）就够，
  // 再粗就会比旁边的单线图标更重。
  const float thick = (std::max)(1.5f, w * 0.052f);
  const float halfT = thick * 0.5f;

  // 千分比坐标；收起态整体左右镜像
  auto px = [&](float ratio) {
    const float x0 = box.left + w * ratio * 0.001f;
    return collapsed ? box.left + w - (x0 - box.left) : x0;
  };
  auto py = [&](float ratio) { return box.top + h * ratio * 0.001f; };

  struct Bar {
    float centerY;
    float from;
    float to;
  };
  // 四条横杠等距：先定间距，再以图标中线为轴往前推第一条。
  // 以前是手写四个数，结果第一档间距比另两档窄一截，整组还整体偏下。
  // 现在只改 barGap 一个数，疏密就整体跟着变，不会再歪。
  const float barGap = 178.0f;                          // 千分比
  const float firstBarY = 500.0f - barGap * 1.5f;       // 中点在 500
  Bar bars[4];
  for (int i = 0; i < 4; ++i) {
    const bool outer = (i == 0 || i == 3);
    bars[i].centerY = firstBarY + barGap * static_cast<float>(i);
    // 上下两条通长，中间两条从三角右侧起，让出侧栏的位置
    bars[i].from = outer ? 168.0f : 432.0f;
    bars[i].to = outer ? 856.0f : 872.0f;
  }

  // 胶囊形横杠：两端各半圆。直接拼多边形而不是 fillRoundRect——
  // 后者的圆角半径被夹紧到短边一半，2px 高的条子上就是 1px，
  // 抗锯齿几乎没覆盖率可用，端头看着仍是方的。
  for (const Bar& bar : bars) {
    const float x0 = px(bar.from);
    const float x1 = px(bar.to);
    const float left = (std::min)(x0, x1);
    const float right = (std::max)(x0, x1);
    const float cy = py(bar.centerY);

    constexpr int kArc = 6;  // 半圆分段数
    PointF pts[2 * (kArc + 1)];
    int n = 0;
    const float pi = 3.14159265f;
    // 左半圆：90° → 270°，从下经左绕到上
    for (int i = 0; i <= kArc; ++i) {
      const float a = pi * 0.5f + pi * static_cast<float>(i) / static_cast<float>(kArc);
      pts[n++] = PointF{left + halfT + std::cos(a) * halfT, cy + std::sin(a) * halfT};
    }
    // 右半圆：-90° → 90°，从上经右绕到下；收口时多边形自动接回起点
    for (int i = 0; i <= kArc; ++i) {
      const float a = -pi * 0.5f + pi * static_cast<float>(i) / static_cast<float>(kArc);
      pts[n++] = PointF{right - halfT + std::cos(a) * halfT, cy + std::sin(a) * halfT};
    }
    painter.fillPolygonF(pts, n, ink);
  }

  // 三角按参考图的比例：比中间两条杠更高更宽，实心块小了不起眼
  const float triTop = py(378.0f);
  const float triBottom = py(622.0f);
  const float triMid = (triTop + triBottom) * 0.5f;
  const float triBase = px(322.0f);
  const float triApex = px(150.0f);

  const PointF tri[3] = {
      PointF{triBase, triTop},
      PointF{triApex, triMid},
      PointF{triBase, triBottom},
  };
  painter.fillPolygonF(tri, 3, ink);
}

}  // namespace

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
  // 图标是正方形的一块：四条杠与三角都在这块里按比例排布
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

  // 图标本身是方形：四条杠 + 三角在一块正方形里排布，任何尺寸比例都不变
  const bool collapsed = isCollapsed();
  const Rect box = iconRect();
  drawSidebarGlyph(painter, box, collapsed, ink);
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
