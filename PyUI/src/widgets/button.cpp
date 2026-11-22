#include "pyui/widgets/button.h"

#include "pyui/text_renderer.h"

#include <algorithm>

namespace pyui {

void Button::setSpec(const ButtonSpec& spec) {
  spec_ = spec;
  requestLayout();
  invalidate();
}

void Button::setLabel(std::string label) {
  if (spec_.label == label) return;
  spec_.label = std::move(label);
  requestLayout();
  invalidate();
}

void Button::setIcon(IconId icon) {
  if (spec_.icon == icon && spec_.hasIcon == (icon != IconId::kNone)) return;
  spec_.icon = icon;
  spec_.hasIcon = icon != IconId::kNone;
  requestLayout();
  invalidate();
}

void Button::setStyle(ButtonStyle style) {
  if (spec_.style == style) return;
  spec_.style = style;
  invalidate();
}

void Button::setIconPlacement(IconPlacement placement) {
  if (spec_.placement == placement) return;
  spec_.placement = placement;
  requestLayout();
  invalidate();
}

void Button::setIconSize(int designPixels) {
  if (spec_.iconSize == designPixels) return;
  spec_.iconSize = designPixels;
  requestLayout();
  invalidate();
}

void Button::setPadX(int designPixels) {
  if (spec_.padX == designPixels) return;
  spec_.padX = designPixels;
  requestLayout();
  invalidate();
}

void Button::setRadius(int designPixels) {
  if (spec_.radius == designPixels) return;
  spec_.radius = designPixels;
  // 圆角只影响绘制，不影响尺寸
  invalidate();
}

void Button::setEnabled(bool enabled) {
  if (isEnabled() == enabled) return;
  View::setEnabled(enabled);
  // 置灰时不该继续停在悬停/按下的高亮里：换成灰底又带高亮会显得
  // 「虽然不能点但好像被选中了」。
  if (!enabled) clearVisualState();
  invalidate();
}

void Button::resolvePalette(Color& background, Color& border, Color& ink) const {
  const Theme& t = theme();

  // 先给确定值：调用方可能复用同一组变量，残留上一步的颜色会串味
  border = kNoColor;

  if (!isEffectivelyEnabled()) {
    // 主行动禁用时保留一点底色，完全透明会让人以为按钮消失了
    if (spec_.style == ButtonStyle::kPrimary) {
      background = blendColor(t.accent, t.pane, 0.72f);
    } else {
      background = spec_.style == ButtonStyle::kOutline ? t.btnDisBg : kNoColor;
    }
    ink = spec_.disabledInk != kNoColor ? spec_.disabledInk : t.textFaint;
    if (spec_.disabledBackground != kNoColor) background = spec_.disabledBackground;
    if (spec_.style == ButtonStyle::kOutline) {
      border = edgeOn(background != kNoColor ? background : t.pane, 8);
    }
    return;
  }

  switch (spec_.style) {
    case ButtonStyle::kOutline:
      background = isPressed() ? t.btnDownBg : (isHot() ? t.btnHoverBg : t.pane);
      ink = t.ink;
      break;
    case ButtonStyle::kPrimary:
      background = isPressed() ? t.accentDown : (isHot() ? t.accentHover : t.accent);
      ink = t.accentInk;
      break;
    case ButtonStyle::kSubtle:
    case ButtonStyle::kFlat:
      background = isPressed() ? t.btnDownBg : (isHot() ? t.hover : kNoColor);
      ink = t.ink;
      break;
    case ButtonStyle::kGhost:
      // 叠在彩色区域上：任何状态都不铺底，只让内容换色
      background = kNoColor;
      ink = t.ink;
      break;
  }

  // 显式色优先于主题；悬停/按下态再叠一层，标题栏那种需要精确控制外观的
  // 按钮靠它，而不是去继承 Button。
  if (spec_.background != kNoColor) background = spec_.background;
  if (spec_.ink != kNoColor) ink = spec_.ink;

  if (isPressed() && spec_.downBackground != kNoColor) {
    background = spec_.downBackground;
  } else if (isHot() && spec_.hoverBackground != kNoColor) {
    background = spec_.hoverBackground;
  }
  if (isHot() && spec_.hoverInk != kNoColor) ink = spec_.hoverInk;

  if (spec_.style != ButtonStyle::kOutline) return;

  if (spec_.border != kNoColor) {
    border = spec_.border;
    return;
  }
  // 描边跟着最终背景走：换主题或设了显式底色都不用手工再配一组边框色
  const Color base = background != kNoColor ? background : theme().pane;
  border = edgeOn(base, isHot() ? 24 : 17);
}

int Button::preferredWidth() const {
  const bool showText = spec_.placement != IconPlacement::kOnly && !spec_.label.empty();
  const bool showIcon = spec_.hasIcon && spec_.placement != IconPlacement::kNone;

  int width = px(spec_.padX) * 2;
  if (showIcon) width += px(spec_.iconSize);
  if (showIcon && showText) width += px(spec_.iconGap);
  if (showText) width += measureTextWidth(spec_.label, uiFont(9.0f, false, false, dpi()));
  return width;
}

Size Button::onMeasure(const Size& available) {
  (void)available;
  const int width = spec_.width > 0 ? px(spec_.width) : preferredWidth();
  const int height = px(spec_.height);
  return Size{width, height};
}

void Button::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  Color background = kNoColor;
  Color border = kNoColor;
  Color ink = kNoColor;
  resolvePalette(background, border, ink);

  const int radius = px(spec_.radius);

  // 按下时内容下沉一像素：比只换底色更接近系统按钮的手感
  const int sink = isPressed() && isEffectivelyEnabled() ? 1 : 0;

  if (background != kNoColor) painter.fillRoundRect(rect, background, radius);
  if (spec_.style == ButtonStyle::kOutline && border != kNoColor) {
    painter.strokeRoundRect(rect, border, radius);
  }

  const bool showIcon = spec_.hasIcon && spec_.placement != IconPlacement::kNone;
  const bool showText = spec_.placement != IconPlacement::kOnly && !spec_.label.empty();
  const Font font = uiFont(9.0f, false, false, dpi());
  const int iconSize = px(spec_.iconSize);
  const int gap = px(spec_.iconGap);

  // 图标在上：竖排居中
  if (spec_.placement == IconPlacement::kTop && showIcon) {
    const int textHeight = showText ? textLineHeight(font) : 0;
    const int gapY = px(4);
    const int totalHeight = iconSize + (showText ? gapY + textHeight : 0);
    const int centerX = rect.center().x;
    const int top = rect.top + (rect.height() - totalHeight) / 2 + sink;

    const Rect iconRect{centerX - iconSize / 2, top, centerX + iconSize / 2, top + iconSize};
    painter.drawIcon(spec_.icon, iconRect, ink);
    if (showText) {
      const Rect textRect{rect.left, iconRect.bottom + gapY, rect.right,
                          iconRect.bottom + gapY + textHeight};
      painter.drawText(textRect, spec_.label, ink, font, TextAlign::kCenter);
    }
    return;
  }

  // 横排：图标 + 文字作为整体居中
  const int textWidth = showText ? measureTextWidth(spec_.label, font) : 0;
  const int contentWidth =
      (showIcon ? iconSize : 0) + (showIcon && showText ? gap : 0) + textWidth;
  int x = rect.left + (rect.width() - contentWidth) / 2;

  if (showIcon) {
    const int iconTop = rect.top + (rect.height() - iconSize) / 2 + sink;
    const Rect iconRect{x, iconTop, x + iconSize, iconTop + iconSize};
    painter.drawIcon(spec_.icon, iconRect, ink);
    x = iconRect.right + gap;
  }
  if (showText) {
    const Rect textRect{x, rect.top + sink, x + textWidth, rect.bottom + sink};
    painter.drawText(textRect, spec_.label, ink, font, TextAlign::kLeft);
  }
}

}  // namespace pyui
