// InputBox：圆角输入框，文本、光标、选区、IME 全部自绘
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <functional>
#include <string>
#include <vector>

namespace AiUI {

struct InputSpec {
  std::wstring cue;  // 空内容时的提示
  bool hasLeadIcon = false;
  IconId leadIcon = IconId::kSearch;
  int leadIconSize = 14;
  int leadIconGap = 5;
  int padX = 11;
  int radius = 7;
  // 插入点竖线宽度（96 DPI 设计值），按 DPI 四舍五入到物理像素：
  // 写死物理像素在 200% 屏上会变成发丝线
  int caretWidth = 1;
  // 密码模式：显示为圆点，但仍可选中与复制
  bool password = false;
};

class InputBox : public View {
 public:
  InputBox();

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
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  bool onChar(wchar_t ch) override;
  void onFocusChanged(bool focused) override;
  // 窗口整体失活时收起插入点，逻辑焦点保留
  void onWindowActivated(bool active) override;
  void onTimer() override;

 private:
  Rect textRect() const;
  int textLeft() const;
  // 每个插入点的横向偏移，按「每字符精确宽度」累加并缓存
  const std::vector<int>& caretOffsets() const;
  // 前 pos 个字符的显示形式：密码模式下统一替换为掩码字符
  std::wstring displayPrefix(int pos) const;
  std::wstring displayText() const { return displayPrefix(static_cast<int>(text_.size())); }
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
  // 让输入法候选框跟随自绘光标
  void updateCaretIme();

  std::wstring text_;
  int caret_ = 0;
  int selectionBegin_ = 0;
  int selectionEnd_ = 0;
  int scrollX_ = 0;
  // 键盘焦点由 ViewRoot 统一管，这里只记录自己的绘制状态
  bool focused_ = false;
  bool dragging_ = false;
  bool caretVisible_ = true;

  // 多组控件共用同一份字体对象，用句柄判断字体是否换过
  mutable std::vector<int> offsets_;
  mutable std::wstring offsetsText_;
  mutable void* offsetsFont_ = nullptr;
  mutable int offsetsDpi_ = 0;

  InputSpec spec_{};
  ChangeHandler onChange_;
  SubmitHandler onSubmit_;
  BlurHandler onBlur_;
};

}  // namespace AiUI
