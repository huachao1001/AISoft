#include "pyui/widgets/text_field.h"

#include "pyui/text_renderer.h"

#include <algorithm>

namespace pyui {
namespace {

bool isWordByte(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// 光标竖线宽度：1 物理像素在 150% 屏上细得像发丝，看不出是光标。
// 取 1.5 设计像素再取整：100% 下 2px、200% 下 3px，随缩放增减但不失形。
int caretBarWidth(int dpi) {
  const int width = static_cast<int>(1.5 * static_cast<double>(dpi) / 96.0 + 0.5);
  return width < 1 ? 1 : width;
}

}  // namespace

TextField::TextField() { setFocusable(true); }

TextField::~TextField() = default;

// ── 内容 ──

void TextField::setText(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  caret_ = text_.size();
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;
  scrollX_ = 0;
  requestLayout();
  invalidate();
}

void TextField::setPasswordChar(std::uint32_t codepoint, bool enabled) {
  passwordChar_ = codepoint;
  password_ = enabled && codepoint != 0;
  // 掩码字符宽度与原文不同，旧滚动量已经无意义
  scrollX_ = 0;
  invalidate();
}

void TextField::setPassword(bool enabled) { setPasswordChar(0x25CF, enabled); }

// ── 编辑状态 ──

void TextField::setCaretByte(size_t byteOffset) {
  const size_t clamped = std::min(byteOffset, text_.size());
  caret_ = utf8AlignDown(text_, clamped);
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;
  invalidate();
}

void TextField::setSelection(size_t anchorByte, size_t caretByte) {
  const size_t anchor = utf8AlignDown(text_, std::min(anchorByte, text_.size()));
  const size_t caret = utf8AlignDown(text_, std::min(caretByte, text_.size()));
  selAnchor_ = anchor;
  caret_ = caret;
  selStart_ = std::min(anchor, caret);
  selEnd_ = std::max(anchor, caret);
  invalidate();
}

void TextField::selectAll() { setSelection(0, text_.size()); }

void TextField::clearSelection() {
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;
  invalidate();
}

// ── 排版 ──

Font TextField::font() const { return uiFont(9.0f, false, false, dpi()); }

Rect TextField::textRect(const Rect& rect) const {
  // 左右各留 padding_，上下让文字按行高垂直居中。
  // 描边占 1 像素，内边距从描边内侧起算，文字才不会压到边框上。
  const int pad = px(padding_);
  const int lineHeight = textLineHeight(font());
  const int top = rect.top + (rect.height() - lineHeight) / 2;
  return Rect{rect.left + pad, top, rect.right - pad, top + lineHeight};
}

std::string TextField::displayText() const {
  if (!password_ || passwordChar_ == 0) return text_;
  // 每个码点换成一个掩码字符：不是按字节替换，中文名也能正确遮住
  std::string out;
  const std::string mask = utf8Encode(passwordChar_);
  size_t i = 0;
  while (i < text_.size()) {
    const size_t next = utf8NextBoundary(text_, i);
    out += mask;
    i = next;
  }
  return out;
}

int TextField::byteToX(size_t byteOffset, const Rect& area) const {
  return textOriginX(area) + textAdvance(text_.substr(0, utf8AlignDown(text_, byteOffset)), font());
}

int TextField::textOriginX(const Rect& area) const {
  // 对齐只在内容放得下时生效：超宽时必然要滚动，此时只能从左侧开始排版，
  // 否则「居中 + 滚动」会让插入点与文字一起来回跳。
  const int width = textAdvance(displayText(), font());
  int offset = 0;
  if (width <= area.width()) {
    if (align_ == TextAlign::kCenter) {
      offset = (area.width() - width) / 2;
    } else if (align_ == TextAlign::kRight) {
      offset = area.width() - width;
    }
  }
  return area.left + offset - scrollX_;
}

size_t TextField::xToByte(int x, const Rect& area) const {
  // 二分字符数：宽度随字符数单调不减，比逐字符累加少一半调用
  const size_t total = utf8Length(text_);
  const int target = x - textOriginX(area);
  if (target <= 0) return 0;

  size_t low = 0;
  size_t high = total;
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    const size_t bytes = utf8PrefixByChars(text_, mid);
    if (textAdvance(text_.substr(0, bytes), font()) <= target) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  // 落在字符中间时取更近的一侧，点击才不会总偏左
  const size_t left = utf8PrefixByChars(text_, low);
  const size_t right = left < text_.size() ? utf8NextBoundary(text_, left) : left;
  if (right == left) return left;
  const int leftX = textAdvance(text_.substr(0, left), font());
  const int rightX = textAdvance(text_.substr(0, right), font());
  return (target - leftX <= rightX - target) ? left : right;
}

void TextField::clampScroll(const Rect& area) {
  const int avail = area.width();
  if (avail <= 0) return;

  const int contentWidth = textAdvance(displayText(), font());
  // 光标位置也要算进可视范围：只按文本宽度滚，末尾光标会贴在右边界只露一半
  const int caretX = textAdvance(text_.substr(0, caret_), font()) + caretBarWidth(dpi());
  const int limit = std::max(0, avail - caretBarWidth(dpi()));

  if (caretX - scrollX_ < 0) {
    scrollX_ = caretX;
  } else if (caretX - scrollX_ > limit) {
    scrollX_ = caretX - limit;
  }
  // 滚动量不能超过内容宽度：文本删短后若沿用旧值，整行会被推出左边界，
  // 看起来像输入框空了。
  //
  // 上界要包含光标条宽度，否则末尾光标会溢出可视区两个像素：内容宽度
  // 刚好等于「已滚 + 可见宽」时，光标仍然画在可见区之外。
  const int scrollMax = std::max(0, contentWidth + caretBarWidth(dpi()) - avail);
  scrollX_ = std::max(0, std::min(scrollX_, scrollMax));
}

// ── 测量与绘制 ──

Size TextField::onMeasure(const Size& available) {
  const Font f = font();
  const int pad = px(padding_);
  const std::string shown = displayText().empty() ? placeholder_ : displayText();
  const int textWidth = shown.empty() ? 0 : measureTextWidth(shown, f);

  int width = textWidth + pad * 2;
  int height = std::max(px(kSizeControlH), textLineHeight(f));
  if (available.width > 0) width = std::min(width, available.width);
  if (available.height > 0) height = std::min(height, available.height);
  return Size{width, height};
}

void TextField::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const bool enabled = isEffectivelyEnabled();
  const bool focused = isFocused();

  const Color background = enabled ? t.inputBg : t.btnDisBg;
  const Color border = focused ? t.inputFocus : t.controlBorder;
  const Color ink = enabled ? t.text : t.textDisabled;
  const Color cueColor = t.textFaint;
  const Color caretColor = t.accent;

  const int radius = px(kRadiusControl);
  painter.fillRoundRect(rect, background, radius);
  painter.strokeRoundRect(rect, border, radius);

  const Font f = font();
  Rect area = textRect(rect);
  if (area.isEmpty()) return;

  // 先按当前内容与光标位置更新水平滚动量，再排版：不调用它的话 scrollX_
  // 永远是 0，长文本输到末尾时光标会被右边界切掉，用户看不到自己正在输入什么。
  clampScroll(area);

  // 内容裁到输入区：滚动时文字不会溢出圆角
  painter.save();
  painter.clipRect(Rect{rect.left + px(padding_), rect.top, rect.right - px(padding_), rect.bottom});

  const std::string shown = displayText();

  if (shown.empty()) {
    // 占位提示与光标可以并存：空输入框也要能看见插入点
    if (!placeholder_.empty()) {
      painter.drawTextEllipsis(area, placeholder_, cueColor, f);
    }
  } else {
    if (hasSelection()) {
      const int x1 = byteToX(selStart_, area);
      const int x2 = byteToX(selEnd_, area);
      painter.fillRect(Rect{x1, area.top, x2, area.bottom}, t.selBg);
    }
    // 用 drawTextRun 而不是 drawText：它与 advanceText / byteToX 共用同一套
    // 排版，插入点才会落在字形之间而不是差半个留白（铁律七）。
    const int width = textAdvance(shown, f);
    const Rect drawRect{textOriginX(area), area.top, textOriginX(area) + width + caretBarWidth(dpi()),
                        area.bottom};
    painter.drawTextRun(drawRect, shown, ink, f);
  }

  if (focused) {
    const int barWidth = caretBarWidth(dpi());
    const int x = byteToX(caret_, area);
    painter.fillRect(Rect{x, area.top, x + barWidth, area.bottom}, caretColor);
  }

  painter.restore();
}

// ── 鼠标 ──

bool TextField::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  if (!isEffectivelyEnabled()) return false;
  setFocus();

  const Rect area = textRect(clientRect());
  const size_t offset = xToByte(e.position.x, area);
  if (e.modifiers.shift) {
    setSelection(selAnchor_, offset);
  } else {
    setCaretByte(offset);
    selAnchor_ = caret_;
  }
  dragging_ = true;
  invalidate();
  return true;
}

void TextField::onMouseMove(const MouseEvent& e) {
  if (!dragging_) return;
  const Rect area = textRect(clientRect());
  const size_t offset = xToByte(e.position.x, area);
  // 拖选时锚点保持按下时的位置，光标跟着指针走
  setSelection(selAnchor_, offset);
}

void TextField::onMouseUp(const MouseEvent& e) {
  (void)e;
  dragging_ = false;
}

void TextField::onMouseLeave() { dragging_ = false; }

// ── 键盘 ──

void TextField::notifyChange() {
  if (onChange_) onChange_(text_);
}

std::string TextField::selectedText() const {
  if (!hasSelection()) return {};
  return text_.substr(selStart_, selEnd_ - selStart_);
}

void TextField::deleteBackward() {
  if (hasSelection()) {
    text_.erase(selStart_, selEnd_ - selStart_);
    caret_ = selStart_;
  } else if (caret_ > 0) {
    const size_t previous = utf8PrevBoundary(text_, caret_);
    text_.erase(previous, caret_ - previous);
    caret_ = previous;
  } else {
    return;
  }
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;
  requestLayout();
  invalidate();
  notifyChange();
}

void TextField::deleteForward() {
  if (hasSelection()) {
    text_.erase(selStart_, selEnd_ - selStart_);
    caret_ = selStart_;
  } else if (caret_ < text_.size()) {
    const size_t next = utf8NextBoundary(text_, caret_);
    text_.erase(caret_, next - caret_);
  } else {
    return;
  }
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;
  requestLayout();
  invalidate();
  notifyChange();
}

bool TextField::onKeyDown(const Key& key, const Modifiers& mods) {
  if (readOnly_ && key != Key::kLeft && key != Key::kRight && key != Key::kHome &&
      key != Key::kEnd && key != Key::kTab) {
    return false;
  }

  switch (key) {
    case Key::kHome:
      setSelection(mods.shift ? selAnchor_ : 0, 0);
      return true;
    case Key::kEnd:
      setSelection(mods.shift ? selAnchor_ : text_.size(), text_.size());
      return true;
    case Key::kLeft: {
      if (hasSelection() && !mods.shift) {
        setCaretByte(selStart_);
        return true;
      }
      const size_t target = utf8PrevBoundary(text_, caret_);
      if (mods.shift) {
        setSelection(selAnchor_, target);
      } else {
        setCaretByte(target);
      }
      return true;
    }
    case Key::kRight: {
      if (hasSelection() && !mods.shift) {
        setCaretByte(selEnd_);
        return true;
      }
      const size_t target = utf8NextBoundary(text_, caret_);
      if (mods.shift) {
        setSelection(selAnchor_, target);
      } else {
        setCaretByte(target);
      }
      return true;
    }
    case Key::kBackspace:
      if (!readOnly_) deleteBackward();
      return true;
    case Key::kDelete:
      if (!readOnly_) deleteForward();
      return true;
    case Key::kEnter:
      if (onSubmit_) return onSubmit_(text_);
      return false;
    case Key::kEscape:
      clearSelection();
      return false;
    case Key::kA:
      if (mods.ctrl) {
        selectAll();
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool TextField::onChar(std::uint32_t codepoint) {
  if (readOnly_ || !isEffectivelyEnabled()) return false;
  // 单行输入框：换行与制表符一律忽略，不当成可输入字符
  if (codepoint == '\r' || codepoint == '\n' || codepoint == '\t') return true;
  if (codepoint < 0x20 || codepoint == 0x7F) return true;

  if (hasSelection()) {
    text_.erase(selStart_, selEnd_ - selStart_);
    caret_ = selStart_;
  }
  const std::string encoded = utf8Encode(codepoint);
  text_.insert(caret_, encoded);
  caret_ += encoded.size();
  selAnchor_ = caret_;
  selStart_ = selEnd_ = caret_;

  requestLayout();
  invalidate();
  notifyChange();
  return true;
}

void TextField::onFocusChanged(bool focused) {
  if (!focused) {
    dragging_ = false;
    // 失焦时保留选区没有意义：用户看不出它还在选，再回来点别处会意外替换
    clearSelection();
  } else {
    scrollX_ = 0;
    requestLayout();
  }
  invalidate();
}

void TextField::onTimer() {
  // 光标闪烁由平台定时器驱动；这里只切可见性，不改内容
  invalidate();
}

}  // namespace pyui
