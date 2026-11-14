// Button 实现
#include "AiUI/widgets/button.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

namespace AiUI {

void Button::setSpec(const ButtonSpec& spec) {
  spec_ = spec;
  invalidate();
}

void Button::setLabel(const std::wstring& label) {
  spec_.label = label;
  invalidate();
}

void Button::setIcon(IconId icon) {
  spec_.icon = icon;
  spec_.hasIcon = icon != IconId::kNone;
  invalidate();
}

void Button::setEnabled(bool enabled) {
  if (isEnabled() == enabled) return;
  View::setEnabled(enabled);
  // 置灰时清掉悬停/按下，避免残留高亮
  if (!enabled) {
    hot_ = false;
    pressed_ = false;
    invalidate();
  }
}

void Button::resolvePalette(Color& background, Color& border, Color& ink) const {
  const Theme& t = theme();

  // 只有 kOutline 有描边，其它样式默认无线；先给确定值，
  // 免得调用方拿到上一个控件残留的 color
  border = kNoColor;
  if (!isEnabled()) {
    // 主行动禁用时保留底色，只是褪成浅色调；完全透明会让人以为按钮消失了
    if (spec_.style == ButtonStyle::kPrimary) {
      background = blendColor(t.accent, t.pane, 0.72f);
      ink = t.textFaint;
    } else {
      background = spec_.style == ButtonStyle::kOutline ? t.btnDisBg : kNoColor;
      ink = t.textFaint;
    }
  } else {
    switch (spec_.style) {
      case ButtonStyle::kOutline:
        background = pressed_ ? t.btnDownBg : (hot_ ? t.btnHoverBg : t.pane);
        ink = t.ink;
        break;
      case ButtonStyle::kPrimary:
        // 强调色三档：常态 / 悬停 / 按下，文字固定用强调色底上的墨色
        background = pressed_ ? t.accentDown : (hot_ ? t.accentHover : t.accent);
        ink = t.accentInk;
        break;
      case ButtonStyle::kSubtle:
      case ButtonStyle::kFlat:
        background = pressed_ ? t.btnDownBg : (hot_ ? t.hover : kNoColor);
        ink = t.ink;
        break;
      case ButtonStyle::kGhost:
        // 叠在彩色区域上：任何状态都不铺底，只让内容换色
        background = kNoColor;
        ink = t.ink;
        break;
    }
  }
  // 显式指定的颜色优先
  if (spec_.background != kNoColor) background = spec_.background;
  if (spec_.ink != kNoColor) ink = spec_.ink;

  // 悬停/按下态的显式色盖过基础色，标题栏这类需要精确控制外观的按钮靠它
  if (isEnabled()) {
    if (pressed_ && spec_.downBackground != kNoColor) background = spec_.downBackground;
    else if (hot_ && spec_.hoverBackground != kNoColor) background = spec_.hoverBackground;
    if (hot_ && spec_.hoverInk != kNoColor) ink = spec_.hoverInk;
  }

  if (spec_.style != ButtonStyle::kOutline) return;
  if (spec_.border != kNoColor) {
    border = spec_.border;
    return;
  }
  // 描边跟着最终背景走：换主题、换显式底色都不用手工配一组边框色。
  // 悬停略深一档保留层次，禁用则更淡，不能比可用态还抢眼。
  const Color base = background != kNoColor ? background : resolvedBackground();
  const int percent = !isEnabled() ? 8 : (hot_ ? 24 : 17);
  border = edgeOn(base, percent);
}

int Button::preferredWidth() {
  const int d = dpi();

  // 文字宽度由平台后端直接测量，不依赖窗口（布局阶段控件可能还没建窗）
  int textWidth = 0;
  const bool showText = spec_.placement != IconPlacement::kOnly && !spec_.label.empty();
  if (showText) textWidth = platform()->measureText(spec_.label, fonts.ui);
  if (showText && textWidth <= 0) {
    textWidth = static_cast<int>(spec_.label.size()) * fonts.ui.pixelHeight();
  }

  const bool showIcon = spec_.hasIcon && spec_.placement != IconPlacement::kNone;
  int width = spec_.padX * 2 * d / 96;
  if (showIcon) width += spec_.iconSize * d / 96;
  if (showIcon && showText) width += spec_.iconGap * d / 96;
  if (showText) width += textWidth;
  return width;
}

Size Button::onMeasure(const Size& available) {
  (void)available;
  const int d = dpi();
  const int width = spec_.width > 0 ? spec_.width * d / 96 : preferredWidth();
  return Size{width, spec_.height * d / 96};
}

void Button::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  Color background, border, ink;
  resolvePalette(background, border, ink);

  if (background != kNoColor) {
    painter.fillRoundRect(rect, background, spec_.radius * d / 96);
  }
  if (spec_.style == ButtonStyle::kOutline && border != kNoColor) {
    painter.strokeRoundRect(rect, border, spec_.radius * d / 96);
  }

  const bool showIcon = spec_.hasIcon && spec_.placement != IconPlacement::kNone;
  const bool showText = spec_.placement != IconPlacement::kOnly && !spec_.label.empty();
  const int iconSize = spec_.iconSize * d / 96;
  const int gap = spec_.iconGap * d / 96;

  // 图标在上：竖排居中
  if (spec_.placement == IconPlacement::kTop && showIcon) {
    const int textHeight = showText ? painter.textHeight(fonts.ui) : 0;
    const int gapY = 4 * d / 96;
    const int totalHeight = iconSize + (showText ? gapY + textHeight : 0);
    const int centerX = rect.center().x;
    const int top = rect.top + (rect.height() - totalHeight) / 2;

    const Rect iconRect{centerX - iconSize / 2, top, centerX + iconSize / 2, top + iconSize};
    painter.drawIcon(spec_.icon, iconRect, ink);
    if (showText) {
      const Rect textRect{rect.left, iconRect.bottom + gapY, rect.right,
                          iconRect.bottom + gapY + textHeight};
      painter.drawText(textRect, spec_.label, ink, fonts.ui, TextAlign::kCenter);
    }
    return;
  }

  // 横排：图标 + 文字整体居中
  const int textWidth =
      showText ? painter.measureText(spec_.label, fonts.ui) : 0;
  const int contentWidth = (showIcon ? iconSize : 0) + (showIcon && showText ? gap : 0) +
                           textWidth;
  int x = rect.left + (rect.width() - contentWidth) / 2;

  if (showIcon) {
    const Rect iconRect{x, rect.top + (rect.height() - iconSize) / 2, x + iconSize,
                        rect.top + (rect.height() - iconSize) / 2 + iconSize};
    painter.drawIcon(spec_.icon, iconRect, ink);
    x = iconRect.right + gap;
  }
  if (showText) {
    const Rect textRect{x, rect.top, x + textWidth, rect.bottom};
    painter.drawText(textRect, spec_.label, ink, fonts.ui, TextAlign::kLeft);
  }
}

bool Button::onMouseDown(const MouseEvent& e) {
  if (!isEnabled() || e.button != MouseButton::kLeft) return false;
  pressed_ = true;
  captureMouse();
  invalidate();
  return true;
}

void Button::onMouseUp(const MouseEvent& e) {
  if (!pressed_) return;
  pressed_ = false;

  // 只有抬起仍在按钮内才算点击
  const bool inside = clientRect().contains(e.position);
  invalidate();
  if (inside && isEnabled()) dispatchClick();
}

void Button::onMouseMove(const MouseEvent& e) {
  (void)e;
  if (!isEnabled()) return;
  if (!hot_) {
    hot_ = true;
    invalidate();
  }
  setCursor(spec_.cursor);
}

void Button::onMouseLeave() {
  if (hot_) {
    hot_ = false;
    invalidate();
  }
  setCursor(CursorShape::kArrow);
}

}  // namespace AiUI
