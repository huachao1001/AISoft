// InputBox：圆角输入框，文本、光标、选区、IME 全部自绘
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <functional>
#include <string>

namespace AiUI {

struct InputSpec {
  std::wstring cue;  // 空内容时的提示
  bool hasLeadIcon = false;
  IconId leadIcon = IconId::kSearch;
  int leadIconSize = 14;
  int leadIconGap = 5;
  int padX = 11;
  int radius = 7;
  // 圆角外的四角填充色，需与宿主底色一致
  Color outerBackground = kColorPane;
};

class InputBox : public Widget {
 public:
  InputBox() { className_ = L"AiUIInputBox"; }

  using ChangeHandler = std::function<void()>;
  using SubmitHandler = std::function<void(bool committed)>;
  using BlurHandler = std::function<void()>;

  void setSpec(const InputSpec& spec);
  const InputSpec& spec() const { return spec_; }

  const std::wstring& text() const { return text_; }
  void setText(const std::wstring& text);
  void setCue(const std::wstring& cue);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }
  // 回车提交 / Esc 取消
  void setOnSubmit(SubmitHandler fn) { onSubmit_ = std::move(fn); }
  void setOnBlur(BlurHandler fn) { onBlur_ = std::move(fn); }

  void selectAll();

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  bool onChar(wchar_t ch) override;
  void onFocusChanged(bool focused) override;
  void onTimer() override;
  Color backgroundColor() const override { return spec_.outerBackground; }

 private:
  Rect textRect() const;
  int textLeft() const;
  bool hasSelection() const { return selectionBegin_ != selectionEnd_; }
  int selectionLow() const { return selectionBegin_ < selectionEnd_ ? selectionBegin_ : selectionEnd_; }
  int selectionHigh() const { return selectionBegin_ < selectionEnd_ ? selectionEnd_ : selectionBegin_; }

  void clampCaret();
  void ensureCaretVisible();
  void resetBlink();
  void deleteSelection();
  void insertText(const std::wstring& s);
  void moveCaret(int pos, bool extend);
  void moveWord(bool right, bool extend);
  void copyToClipboard() const;
  void pasteFromClipboard();
  void cutToClipboard();
  int hitTest(int x) const;
  int xForPosition(int pos) const;
  void syncTextToWindow();

  std::wstring text_;
  int caret_ = 0;
  int selectionBegin_ = 0;
  int selectionEnd_ = 0;
  int scrollX_ = 0;
  bool focused_ = false;
  bool dragging_ = false;
  bool caretVisible_ = true;

  InputSpec spec_{};
  ChangeHandler onChange_;
  SubmitHandler onSubmit_;
  BlurHandler onBlur_;
};

}  // namespace AiUI
