// Tooltip 实现：纯计算 + 绘制，不持有窗口也不吃事件
#include "AiUI/tooltip.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"

#include <algorithm>

namespace AiUI {
namespace {

// 尺寸按 96 DPI 设计，绘制时按实际 DPI 放大
constexpr int kPadX = 8;
constexpr int kPadY = 5;
// 与锚点之间的空隙：贴着控件边会让人以为它是控件的一部分
constexpr int kGap = 6;
constexpr int kSideMargin = 4;

int px(int designPx, int dpi) {
  const int d = (std::max)(96, dpi);
  return (std::max)(1, designPx * d / 96);
}

}  // namespace

void Tooltip::show(const std::wstring& text) {
  text_ = text;
  visible_ = !text_.empty();
}

void Tooltip::hide() {
  text_.clear();
  visible_ = false;
}

Rect Tooltip::bounds(const Rect& clientArea, const Rect& anchor, int dpi) const {
  if (!visible_ || text_.empty()) return Rect{};

  const Font& font = fonts.uiSmall;
  if (!font.isValid()) return Rect{};

  const int textWidth = platform()->measureText(text_, font);
  const int padX = px(kPadX, dpi);
  const int padY = px(kPadY, dpi);
  const int gap = px(kGap, dpi);
  const int margin = px(kSideMargin, dpi);

  const int boxH = font.pixelHeight() + padY * 2;
  const int maxW = (std::max)(0, clientArea.width() - margin * 2);
  const int boxW = (std::min)(textWidth + padX * 2, maxW);
  if (boxW <= 0 || boxH <= 0) return Rect{};

  // 优先放上方；顶端放不下改放下方。两侧都要夹进客户区，
  // 否则靠右的控件（如标题栏按钮）会把自己的提示框推到窗口外。
  int top = anchor.top - gap - boxH;
  if (top < clientArea.top + margin) top = anchor.bottom + gap;
  if (top + boxH > clientArea.bottom - margin) {
    top = (std::max)(clientArea.top + margin, clientArea.bottom - margin - boxH);
  }

  int left = anchor.center().x - boxW / 2;
  left = (std::max)(clientArea.left + margin, (std::min)(left, clientArea.right - margin - boxW));

  return Rect{left, top, left + boxW, top + boxH};
}

void Tooltip::paint(Painter& painter, const Rect& clientArea, const Rect& anchor, int dpi) const {
  const Rect box = bounds(clientArea, anchor, dpi);
  if (box.isEmpty()) return;

  // 底色取深色实底、文字用浅色：提示框在任何主题下都要能读，
  // 不能沿用宿主底色（浅底浅字会糊成一片）。
  painter.fillRoundRect(box, makeColor(0x2b, 0x2f, 0x36, 0xf2), px(4, dpi));
  const Font& font = fonts.uiSmall;
  if (!font.isValid()) return;
  painter.drawText(box, text_, makeColor(0xff, 0xff, 0xff), font, TextAlign::kCenter);
}

}  // namespace AiUI
