// InputBox 实现：文本、光标、选区、剪贴板、IME
#include "AiUI/widgets/input_box.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

#include <algorithm>
#include <cwctype>

namespace AiUI {

InputBox::InputBox() {
  // 输入框是键盘焦点目标；容器上的 setFocus() 会落到这里
  setFocusable(true);
}

void InputBox::setSpec(const InputSpec& spec) {
  spec_ = spec;
  ensureCaretVisible();
  invalidate();
}

void InputBox::setText(const std::wstring& text) {
  text_ = text;
  caret_ = static_cast<int>(text_.size());
  selectionBegin_ = selectionEnd_ = caret_;
  scrollX_ = 0;
  clampCaret();
  ensureCaretVisible();
  resetBlink();
  syncTextToWindow();
  invalidate();
}

void InputBox::setCue(const std::wstring& cue) {
  spec_.cue = cue;
  invalidate();
}

void InputBox::selectAll() {
  selectionBegin_ = 0;
  selectionEnd_ = static_cast<int>(text_.size());
  caret_ = selectionEnd_;
  resetBlink();
  invalidate();
}

int InputBox::textLeft() const {
  const int d = dpi();
  return spec_.padX * d / 96 +
         (spec_.hasLeadIcon ? (spec_.leadIconSize + spec_.leadIconGap) * d / 96 : 0);
}

// 尾部图标在客户区里的矩形
Rect InputBox::tailIconRect() const {
  if (!spec_.hasTailIcon) return Rect{};
  const int d = dpi();
  const Rect rc = clientRect();
  const int size = spec_.tailIconSize * d / 96;
  // 图标右缘不贴边框：与左内边距对称留一档，视觉上才不显得被挤在边上
  const int pad = (spec_.padX + 2) * d / 96;
  const int right = rc.right - pad;
  return Rect{right - size, rc.center().y - size / 2, right, rc.center().y + size / 2};
}

// 文本可视区的右边界（物理像素）。滚动量、裁剪、绘制宽度都以它为界，
// 不能各处各减一次边距——差几像素就会出现「滚动量够了但末字仍被切」。
int InputBox::textRight() const {
  const int d = dpi();
  const Rect rc = clientRect();
  const int pad = spec_.padX * d / 96;
  // 有尾部图标时文本要让出「图标 + 间距」，否则字会压到眼睛上
  const int tail = spec_.hasTailIcon ? (spec_.tailIconSize + spec_.tailIconGap) * d / 96 : 0;
  return rc.right - pad - tail;
}

// 插入点竖线宽度（物理像素）。
// 固定 1px，不随 DPI 放大：GDI+ 的字符单元首尾相接，相邻字形之间的空隙
// 常常只有 1~2 物理像素；2px 的竖线会把空隙填满、与相邻笔画粘成一根粗黑条，
// 看上去就像压在字上。系统自身的光标宽度同样是固定 1px。
int InputBox::caretBarWidth() const {
  (void)spec_.caretWidth;
  return 1;
}

Rect InputBox::textRect() const {
  const Rect rc = clientRect();
  const int d = dpi();
  const int lineHeight = (std::max)(16 * d / 96, fonts.ui.pixelHeight());
  const int top = rc.top + (rc.height() - lineHeight) / 2;
  // 右边界与 ensureCaretVisible 共用 textRight()：可视区只有一个来源，
  // 否则滚动量和裁剪范围会各算一套，末字刚好卡在两者之间被切掉。
  return Rect{textLeft() - scrollX_, top, textRight(), top + lineHeight};
}

// 每个插入点的横向偏移（相对文本起点，物理像素）。长度 = 字符数 + 1。
// 坐标来源只有一个：后端按「整串一次排版」给出的字符单元边界。绘制
// （drawTextRun）、占位宽度（advanceText）、插入点（本函数）三者共用
// 同一套排版，才能保证光标落在字形之间而不是压字或错位。
const std::vector<int>& InputBox::caretOffsets() const {
  const std::wstring display = displayText();
  const Font& font = fonts.ui;
  const int n = static_cast<int>(display.size());

  // 文本、字体或 DPI 变了才重排；光标移动、重绘都复用这份结果
  const bool stale = offsets_.size() != static_cast<size_t>(n) + 1 ||
                     offsetsText_ != display || offsetsDpi_ != dpi() ||
                     offsetsFont_ != font.handle();
  if (!stale) return offsets_;

  offsets_.assign(static_cast<size_t>(n) + 1, 0);
  offsetsText_ = display;
  offsetsDpi_ = dpi();
  offsetsFont_ = font.handle();
  if (n == 0) return offsets_;

  // 排版推进量：既是绘制矩形的宽度依据，也是回退路径下末位插入点的位置
  const int total = platform()->advanceText(display, font);

  const std::vector<int> bounds = platform()->glyphBounds(display, font);
  // 后端给的边界要能当插入点用：单调不减、不超出整串推进量。
  // 落后端的实现会返回离谱坐标（整串退化成一个巨大矩形），
  // 那样插入点会把文本整段推出可视区，必须退回逐前缀测量。
  bool usable = bounds.size() == static_cast<size_t>(n) * 2;
  if (usable) {
    int prev = 0;
    for (size_t i = 0; i < bounds.size(); i += 2) {
      const int left = bounds[i];
      const int right = bounds[i + 1];
      if (right < left || left < prev - 1 || right > total + 2) {
        usable = false;
        break;
      }
      prev = right;
    }
  }

  if (usable) {
    // 第 i 个插入点 = 第 i 个字符单元的右缘。首字的左缘就是排版原点，
    // 正常为 0；若后端给了正值（自带左留白）也要跟着走，否则整段偏左。
    offsets_[0] = (std::max)(0, bounds[0]);
    for (int i = 0; i < n; ++i) {
      offsets_[static_cast<size_t>(i) + 1] = bounds[static_cast<size_t>(i) * 2 + 1];
    }
  } else {
    // 后端没给出可用边界时退回逐前缀推进
    for (int i = 1; i <= n; ++i) {
      offsets_[static_cast<size_t>(i)] =
          platform()->advanceText(display.substr(0, static_cast<size_t>(i)), font);
    }
  }

  // 插入点必须单调不减：字形可能略微重叠（负 bearing），若右缘比前一个
  // 插入点还小，光标会往回跳。夹平后视觉上始终向右推进。
  for (int i = 1; i <= n; ++i) {
    if (offsets_[static_cast<size_t>(i)] < offsets_[static_cast<size_t>(i) - 1]) {
      offsets_[static_cast<size_t>(i)] = offsets_[static_cast<size_t>(i) - 1];
    }
  }
  return offsets_;
}

int InputBox::xForPosition(int pos) const {
  const std::vector<int>& offsets = caretOffsets();
  const int clamped = std::clamp(pos, 0, static_cast<int>(text_.size()));
  return textLeft() + offsets[static_cast<size_t>(clamped)] - scrollX_;
}

int InputBox::hitTest(int x) const {
  const std::vector<int>& offsets = caretOffsets();
  const int target = x - textLeft() + scrollX_;
  // 表里是单调递增的位置，直接找最近的插入点
  int best = 0;
  int bestDiff = INT_MAX;
  for (size_t i = 0; i < offsets.size(); ++i) {
    const int diff = std::abs(offsets[i] - target);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = static_cast<int>(i);
    }
  }
  return best;
}

std::wstring InputBox::displayPrefix(int pos) const {
  const int n = std::clamp(pos, 0, static_cast<int>(text_.size()));
  if (!spec_.password) return text_.substr(0, n);
  // 掩码用 U+2022，与系统密码框观感一致
  return std::wstring(static_cast<size_t>(n), L'\u2022');
}

void InputBox::clampCaret() {
  const int n = static_cast<int>(text_.size());
  caret_ = std::clamp(caret_, 0, n);
  selectionBegin_ = std::clamp(selectionBegin_, 0, n);
  selectionEnd_ = std::clamp(selectionEnd_, 0, n);
}

void InputBox::ensureCaretVisible() {
  // 没挂载时没有真实宽度，等布局完成后再校正
  const Rect rc = clientRect();
  if (rc.isEmpty()) return;
  // 可用宽度与 textRect() 同一口径：绘制、裁剪、滚动不能各算一套，
  // 否则末字会刚好落在两种边界之间被切掉。
  const int avail = textRight() - textLeft();
  if (avail <= 0) return;

  const int caretWidth = caretOffsets()[static_cast<size_t>(std::clamp(caret_, 0, static_cast<int>(text_.size())))];
  // 光标本体也占宽度：滚动量要把它的宽度算进去，否则插入点会贴着右边界、
  // 只露一半，看起来像末尾被裁
  const int caretBar = caretBarWidth();
  const int limit = (std::max)(0, avail - caretBar);

  if (caretWidth - scrollX_ < 0) {
    scrollX_ = caretWidth;
  } else if (caretWidth - scrollX_ > limit) {
    scrollX_ = caretWidth - limit;
  }

  // 滚动量不能超过内容宽度：文本删短后若沿用旧值，整行会被推到左边界之外，
  // 看起来就像输入框空了。上限取「内容宽 - 可视宽」，内容放得下时恒为 0。
  // 内容宽用排版推进量，与绘制矩形的宽度同一口径。
  const std::wstring shown = displayText();
  const int contentWidth = platform()->advanceText(shown, fonts.ui);
  scrollX_ = std::clamp(scrollX_, 0, (std::max)(0, contentWidth - avail));
}

void InputBox::resetBlink() {
  caretVisible_ = true;
  // 只有真正持有焦点时才需要定时器
  if (focused_) startBlinkTimer();
  invalidate();
}

void InputBox::updateCaretIme() {
  const Rect area = textRect();
  updateImeCaret(xForPosition(caret_), area.top);
}

void InputBox::deleteSelection() {
  if (!hasSelection()) return;
  const int low = selectionLow();
  const int high = selectionHigh();
  text_.erase(low, high - low);
  caret_ = low;
  selectionBegin_ = selectionEnd_ = low;
  clampCaret();
  syncTextToWindow();
}

void InputBox::insertText(const std::wstring& s) {
  if (s.empty()) return;
  deleteSelection();
  text_.insert(caret_, s);
  caret_ += static_cast<int>(s.size());
  selectionBegin_ = selectionEnd_ = caret_;
  clampCaret();
  ensureCaretVisible();
  resetBlink();
  syncTextToWindow();
  invalidate();
  if (onChange_) onChange_();
}

void InputBox::moveCaret(int pos, bool extend) {
  const int n = static_cast<int>(text_.size());
  caret_ = std::clamp(pos, 0, n);
  if (extend) {
    selectionEnd_ = caret_;
  } else {
    selectionBegin_ = selectionEnd_ = caret_;
  }
  ensureCaretVisible();
  resetBlink();
  updateCaretIme();
  invalidate();
}

void InputBox::moveWord(bool right, bool extend) {
  int pos = caret_;
  const int n = static_cast<int>(text_.size());
  auto isWord = [](wchar_t c) {
    return std::iswalnum(c) != 0 || c == L'_' || c == L'.' || c == L'-';
  };
  if (right) {
    while (pos < n && isWord(text_[pos])) ++pos;
    while (pos < n && !isWord(text_[pos])) ++pos;
  } else {
    while (pos > 0 && !isWord(text_[pos - 1])) --pos;
    while (pos > 0 && isWord(text_[pos - 1])) --pos;
  }
  moveCaret(pos, extend);
}

void InputBox::copyToClipboard() const {
  if (!hasSelection()) return;
  const std::wstring part = text_.substr(selectionLow(), selectionHigh() - selectionLow());
  platform()->setClipboardText(part);
}

void InputBox::cutToClipboard() {
  copyToClipboard();
  deleteSelection();
  ensureCaretVisible();
  invalidate();
  if (onChange_) onChange_();
}

void InputBox::pasteFromClipboard() {
  std::wstring data = platform()->clipboardText();
  if (data.empty()) return;
  // 单行输入框：换行统一折成空格
  for (auto& c : data) {
    if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
  }
  insertText(data);
}

void InputBox::syncTextToWindow() {
  // 平台窗口没有文本存储，文本只在控件里；这里保留钩子便于扩展
}

Size InputBox::onMeasure(const Size& available) {
  (void)available;
  return Size{0, 34};
}

void InputBox::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int radius = spec_.radius * d / 96;
  const Theme& t = theme();

  // 未显式指定时跟随主题；描边统一取主题的控件描边，和单选/复选对齐
  const Color background = spec_.background != kNoColor ? spec_.background : t.inputBg;
  const Color border = spec_.border != kNoColor ? spec_.border : t.controlBorder;
  const Color focusBorder = spec_.focusBorder != kNoColor ? spec_.focusBorder : t.inputFocus;
  const Color textColor = spec_.text != kNoColor ? spec_.text : t.text;
  const Color cueColor = spec_.cueColor != kNoColor ? spec_.cueColor : t.textFaint;
  const Color iconColor = spec_.leadIconColor != kNoColor ? spec_.leadIconColor : t.textFaint;
  const Color selBg = spec_.selBg != kNoColor ? spec_.selBg : t.selBg;
  // 插入点用主题强调色：与聚焦描边同族，视觉上属于「当前编辑位置」；
  // 用正文色会和字形混在一起，看不出是光标
  const Color caretColor = spec_.caret != kNoColor ? spec_.caret : t.accent;

  painter.fillRoundRect(rect, background, radius);
  painter.strokeRoundRect(rect, focused_ ? focusBorder : border, radius);

  const Rect textArea = textRect();
  if (spec_.hasLeadIcon) {
    const int size = spec_.leadIconSize * d / 96;
    const Rect iconRect{rect.left + spec_.padX * d / 96, rect.center().y - size / 2,
                        rect.left + spec_.padX * d / 96 + size, rect.center().y + size / 2};
    painter.drawIcon(spec_.leadIcon, iconRect, iconColor);
  }

  // 文本裁剪到输入区，滚动时不会溢出圆角。
  // 用 save/restore 包住：尾部图标与描边不在文本流里，不能被这条裁剪切掉
  painter.save();
  painter.clipRect(Rect{rect.left + spec_.padX * d / 96, rect.top, textRight(), rect.bottom});

  if (text_.empty()) {
    // 占位提示与光标可以并存：空输入框也要能看见插入点
    painter.drawTextEllipsis(textArea, spec_.cue, cueColor, fonts.ui);
  } else {
    if (hasSelection()) {
      const int x1 = xForPosition(selectionLow());
      const int x2 = xForPosition(selectionHigh());
      painter.fillRect(Rect{x1, textArea.top, x2, textArea.bottom}, selBg);
    }
    // 用 drawTextRun 绘制：它与 caretOffsets / advanceText 共用同一套排版，
    // 排版原点就是 textArea.left，字符单元边界就是插入点位置。
    // 普通 drawText 会额外补 em/6 留白，光标与字形就会差几个像素。
    // 矩形宽度取「推进量 + 1」：左对齐时各字形位置只由累计推进决定，右边
    // 多给 1px 不会移动任何字形，却能保证落在整数边界上的末字不被排掉。
    // 溢出部分由上面的 clipRect 负责拦。
    const std::wstring shown = displayText();
    const int textWidth = platform()->advanceText(shown, fonts.ui);
    const Rect drawRect{textArea.left, textArea.top, textArea.left + textWidth + 1,
                        textArea.bottom};
    painter.drawTextRun(drawRect, shown, textColor, fonts.ui);
  }

  if (focused_ && caretVisible_) {
    // 1px 竖线 + 普通填充：实测 fillRect 在整数坐标下就是单列实色，
    // 不需要额外的抗锯齿开关。粗细只由 caretBarWidth 决定。
    const int caretW = caretBarWidth();
    const int x = xForPosition(caret_);
    painter.fillRect(Rect{x, textArea.top, x + caretW, textArea.bottom}, caretColor);
  }

  painter.restore();

  // 尾部图标画在裁剪区之外：它不在文本流里，不该被文字裁剪影响
  if (spec_.hasTailIcon) {
    const Rect iconRect = tailIconRect();
    const Color color = spec_.tailIconColor != kNoColor ? spec_.tailIconColor : t.textMuted;
    painter.drawIcon(spec_.tailIcon, iconRect, color);
  }
}

void InputBox::setTailIcon(IconId icon) {
  spec_.tailIcon = icon;
  spec_.hasTailIcon = icon != IconId::kNone;
  invalidate();
}

bool InputBox::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  // 尾图标是行内按钮：命中它就只切换图标，不移动插入点，
  // 否则点眼睛会把光标甩到行尾
  if (spec_.hasTailIcon && tailIconRect().contains(e.position)) {
    tailPressed_ = true;
    captureMouse();
    return true;
  }
  setFocus();
  dragging_ = true;
  captureMouse();
  moveCaret(hitTest(e.position.x), e.modifiers.shift);
  return true;
}

void InputBox::onMouseUp(const MouseEvent& e) {
  if (tailPressed_) {
    tailPressed_ = false;
    releaseMouse();
    if (tailIconRect().contains(e.position) && onTailIcon_) onTailIcon_();
    return;
  }
  if (dragging_) {
    dragging_ = false;
    releaseMouse();
  }
}

void InputBox::onMouseMove(const MouseEvent& e) {
  if (dragging_) {
    moveCaret(hitTest(e.position.x), true);
    return;
  }
  // 输入框内一律用文本光标
  setCursor(CursorShape::kIBeam);
}

void InputBox::onDoubleClick(const MouseEvent& e) {
  (void)e;
  selectAll();
}

bool InputBox::onKeyDown(const Key& key, const Modifiers& mods) {
  switch (key) {
    case Key::kHome: moveCaret(0, mods.shift); return true;
    case Key::kEnd: moveCaret(static_cast<int>(text_.size()), mods.shift); return true;
    case Key::kLeft:
      if (mods.ctrl) {
        moveWord(false, mods.shift);
      } else {
        moveCaret(hasSelection() && !mods.shift ? selectionLow() : caret_ - 1, mods.shift);
      }
      return true;
    case Key::kRight:
      if (mods.ctrl) {
        moveWord(true, mods.shift);
      } else {
        moveCaret(hasSelection() && !mods.shift ? selectionHigh() : caret_ + 1, mods.shift);
      }
      return true;
    case Key::kBackspace:
      if (hasSelection()) {
        deleteSelection();
      } else if (caret_ > 0) {
        text_.erase(caret_ - 1, 1);
        --caret_;
        selectionBegin_ = selectionEnd_ = caret_;
      }
      ensureCaretVisible();
      resetBlink();
      invalidate();
      if (onChange_) onChange_();
      return true;
    case Key::kDelete:
      if (hasSelection()) {
        deleteSelection();
      } else if (caret_ < static_cast<int>(text_.size())) {
        text_.erase(caret_, 1);
      }
      selectionBegin_ = selectionEnd_ = caret_;
      ensureCaretVisible();
      resetBlink();
      invalidate();
      if (onChange_) onChange_();
      return true;
    case Key::kEnter:
      if (onSubmit_) onSubmit_(true);
      return true;
    case Key::kEscape:
      if (onSubmit_) onSubmit_(false);
      return true;
    case Key::kA:
      if (mods.ctrl) {
        selectAll();
        return true;
      }
      return false;
    case Key::kC:
      if (mods.ctrl) {
        copyToClipboard();
        return true;
      }
      return false;
    case Key::kX:
      if (mods.ctrl) {
        cutToClipboard();
        return true;
      }
      return false;
    case Key::kV:
      if (mods.ctrl) {
        pasteFromClipboard();
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool InputBox::onChar(wchar_t ch) {
  if (ch == L'\r' || ch == L'\n' || ch == L'\t') return true;
  if (ch < L' ') return true;
  insertText(std::wstring(1, ch));
  return true;
}

void InputBox::onFocusChanged(bool focused) {
  focused_ = focused;
  dragging_ = false;
  if (focused) {
    caretVisible_ = true;
    // 光标闪烁定时器由基类转给 ViewRoot，整棵树共用一个
    startBlinkTimer();
    ensureCaretVisible();
    updateCaretIme();
  } else {
    stopBlinkTimer();
  }
  invalidate();
  if (!focused && onBlur_) onBlur_();
}

void InputBox::onWindowActivated(bool active) {
  // 窗口不在前台时插入点不该出现：它看起来会像「卡住的光标」。
  // 逻辑焦点保留，窗口回到前台后重新开始闪烁。
  if (active) {
    if (!focused_) return;
    caretVisible_ = true;
    startBlinkTimer();
  } else {
    stopBlinkTimer();
    caretVisible_ = false;
  }
  invalidate();
}

void InputBox::onTimer() {
  if (!focused_) return;
  caretVisible_ = !caretVisible_;
  invalidate();
}

// 布局完成才知道真实可用宽度。setText / setCue 通常在 onCreate 里先于布局调用，
// 那时 clientRect 为空，ensureCaretVisible 直接返回，滚动量一直停在 0，
// 长文本就从开头绘制、尾部被裁掉（看着像末尾字符消失）。
void InputBox::onLayout(const Rect& frame) {
  (void)frame;
  ensureCaretVisible();
  // 尺寸变了插入点画在哪儿也要跟着变
  if (focused_) updateCaretIme();
  invalidate();
}

}  // namespace AiUI
