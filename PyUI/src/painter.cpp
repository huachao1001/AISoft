#include "pyui/painter.h"

#include <algorithm>

namespace pyui {
namespace {

constexpr char kEllipsis[] = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
constexpr size_t kEllipsisLength = 3;

}  // namespace

// 覆盖率蒙版的默认实现：逐像素退回 fillRect。
//
// 慢，但语义正确。写在这里而不是各后端重复：只要有人新加一个后端却忘了覆盖
// 它，文字仍然会显示（只是慢），不会静默消失——静默消失最难查。
void PaintBackend::blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                                     Color color) {
  if (rc.isEmpty() || coverage == nullptr || stride <= 0) return;
  const std::uint8_t baseAlpha = colorAlpha(color);
  if (baseAlpha == 0) return;

  for (int y = 0; y < rc.height(); ++y) {
    const std::uint8_t* row = coverage + static_cast<size_t>(y) * static_cast<size_t>(stride);
    for (int x = 0; x < rc.width(); ++x) {
      const std::uint8_t cov = row[x];
      if (cov == 0) continue;
      const std::uint8_t alpha =
          static_cast<std::uint8_t>(static_cast<int>(baseAlpha) * cov / 255);
      if (alpha == 0) continue;
      fillRect(Rect{rc.left + x, rc.top + y, rc.left + x + 1, rc.top + y + 1},
               (color & 0x00FFFFFFu) | (static_cast<Color>(alpha) << 24));
    }
  }
}

void Painter::drawRectOutline(const Rect& rc, Color color, int width) {
  if (width <= 0 || rc.isEmpty()) return;
  const int w = std::min(width, std::min(rc.width(), rc.height()));
  // 四条边分别填充：不依赖后端的 stroke 实现，三平台结果必然一致
  backend_.fillRect(Rect{rc.left, rc.top, rc.right, rc.top + w}, color);
  backend_.fillRect(Rect{rc.left, rc.bottom - w, rc.right, rc.bottom}, color);
  backend_.fillRect(Rect{rc.left, rc.top + w, rc.left + w, rc.bottom - w}, color);
  backend_.fillRect(Rect{rc.right - w, rc.top + w, rc.right, rc.bottom - w}, color);
}

void Painter::drawRoundRectWithBorder(const Rect& rc, Color fill, Color border, int radius,
                                      int borderWidth) {
  if (rc.isEmpty()) return;
  const int r = std::min(radius, std::min(rc.width(), rc.height()) / 2);
  if (fill != kNoColor) backend_.fillRoundRect(rc, fill, r);
  if (border != kNoColor && borderWidth > 0) backend_.strokeRoundRect(rc, border, r, borderWidth);
}

void Painter::drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                       TextAlign align) {
  if (text.empty() || rc.isEmpty() || color == kNoColor) return;
  backend_.drawText(rc, text, color, font, align);
}

void Painter::drawTextRun(const Rect& rc, std::string_view text, Color color, const Font& font) {
  if (text.empty() || color == kNoColor) return;
  backend_.drawTextRun(rc, text, color, font);
}

size_t Painter::fitPrefix(std::string_view text, int limit, const Font& font) const {
  // 二分「字符数」而不是字节数：中文每字 3 字节，按字节二分一定会切出半个码点。
  // 宽度随字符数单调不减（kerning 的极轻微非单调不影响「放得下」的判断）。
  if (limit <= 0) return 0;
  size_t low = 0;
  size_t high = utf8Length(text);
  size_t best = 0;
  while (low <= high) {
    const size_t mid = low + (high - low) / 2;
    const size_t bytes = utf8PrefixByChars(text, mid);
    if (backend_.advanceText(text.substr(0, bytes), font) <= limit) {
      best = bytes;
      if (mid == high) break;
      low = mid + 1;
    } else {
      if (mid == 0) break;
      high = mid - 1;
    }
  }
  return best;
}

size_t Painter::fitSuffix(std::string_view text, int limit, const Font& font) const {
  if (limit <= 0) return 0;
  const size_t total = utf8Length(text);
  size_t low = 0;
  size_t high = total;
  size_t best = 0;
  while (low <= high) {
    const size_t mid = low + (high - low) / 2;
    const size_t start = utf8PrefixByChars(text, total - mid);
    if (backend_.advanceText(text.substr(start), font) <= limit) {
      best = text.size() - start;
      if (mid == high) break;
      low = mid + 1;
    } else {
      if (mid == 0) break;
      high = mid - 1;
    }
  }
  return best;
}

void Painter::drawTextEllipsis(const Rect& rc, std::string_view text, Color color,
                               const Font& font, TextAlign align) {
  if (rc.isEmpty() || color == kNoColor || text.empty()) return;

  const int limit = rc.width();
  if (backend_.advanceText(text, font) <= limit) {
    backend_.drawText(rc, text, color, font, align);
    return;
  }

  const int ellipsisWidth = backend_.advanceText(std::string_view(kEllipsis, kEllipsisLength), font);
  if (ellipsisWidth > limit) return;  // 连省略号都放不下，宁可不画也不截半个字

  const size_t prefixBytes = fitPrefix(text, limit - ellipsisWidth, font);
  std::string out(text.substr(0, prefixBytes));
  out.append(kEllipsis, kEllipsisLength);
  backend_.drawText(rc, out, color, font, align);
}

void Painter::drawTextMiddleEllipsis(const Rect& rc, std::string_view text, Color color,
                                     const Font& font, TextAlign align) {
  if (rc.isEmpty() || color == kNoColor || text.empty()) return;

  const int limit = rc.width();
  if (backend_.advanceText(text, font) <= limit) {
    backend_.drawText(rc, text, color, font, align);
    return;
  }

  const int ellipsisWidth = backend_.advanceText(std::string_view(kEllipsis, kEllipsisLength), font);
  if (ellipsisWidth > limit) return;

  const int budget = limit - ellipsisWidth;
  // 尾部优先：扩展名是用户最先要认的信息，所以后缀至少拿三分之二预算。
  const int headBudget = budget / 3;
  const int tailBudget = budget - headBudget;

  const size_t headBytes = fitPrefix(text, headBudget, font);
  const size_t tailBytes = fitSuffix(text, tailBudget, font);

  // 首尾重叠（文本很短但字很宽）时退化为纯头部省略，避免画出重复内容
  if (headBytes + tailBytes >= text.size()) {
    drawTextEllipsis(rc, text, color, font, align);
    return;
  }

  std::string out(text.substr(0, headBytes));
  out.append(kEllipsis, kEllipsisLength);
  out.append(text.substr(text.size() - tailBytes));
  backend_.drawText(rc, out, color, font, align);
}

}  // namespace pyui
