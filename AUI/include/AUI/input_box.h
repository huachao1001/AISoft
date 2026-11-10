// 输入框：圆角外框与文本全部自绘，不依赖原生 EDIT。
// 自己维护文本、光标、选区与 IME 组合串，保证跨平台与视觉完全可控。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "AUI/icon.h"
#include "AUI/theme.h"
#include "AUI/widget.h"

namespace AUI {

struct InputSpec {
  std::wstring cue;  // 空内容时显示的提示
  bool hasLeadIcon = false;
  IconId leadIcon = IconId::kSearch;
  int leadIconSize = 14;
  int leadIconGap = 5;
  int padX = 11;
  int radius = 7;
  // 圆角外四个角的填充色，与宿主底色一致才不会露出白块
  COLORREF outerBg = kPaneBg;
};

class InputBox : public Widget {
 public:
  using ChangeHandler = std::function<void()>;
  using SubmitHandler = std::function<void(bool committed)>;

  void SetSpec(const InputSpec& spec);
  std::wstring Text() const { return text_; }
  void SetText(const std::wstring& text);
  void SetCue(const std::wstring& cue);

  // 控件自身就是输入窗口，不再有内嵌 EDIT；保留该名便于调用方迁移
  HWND Edit() const { return hwnd_; }

  void SetChangeHandler(ChangeHandler fn) { onChange_ = std::move(fn); }
  // 回车提交 / Esc 取消
  void SetSubmitHandler(SubmitHandler fn) { onSubmit_ = std::move(fn); }
  // 失去焦点（用于“点别处即放弃编辑”）
  void SetBlurHandler(std::function<void()> fn) { onBlur_ = std::move(fn); }

  void SelectAll();

 protected:
  void OnSize(int cx, int cy) override;
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;
  LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

 private:
  RECT TextRect() const;
  int  TextLeft() const;
  bool HasSelection() const { return selBegin_ != selEnd_; }
  int  SelLow() const { return selBegin_ < selEnd_ ? selBegin_ : selEnd_; }
  int  SelHigh() const { return selBegin_ < selEnd_ ? selEnd_ : selBegin_; }

  void ClampCaret();
  void ScrollCaretIntoView();
  void EnsureCaretVisible();
  void ResetBlink();
  void DeleteSelection();
  void InsertText(const std::wstring& s);
  void MoveCaret(int pos, bool extend);
  void MoveWord(bool right, bool extend);
  void MoveLineEdge(bool right, bool extend);
  void CopyToClipboard() const;
  void PasteFromClipboard();
  void CutToClipboard();
  int  HitTest(int x) const;
  int  XForPos(int pos) const;
  void OnCompositionDone();

  std::wstring text_;
  std::wstring composition_;  // IME 组合中未上屏的串
  int caret_ = 0;
  int selBegin_ = 0;
  int selEnd_ = 0;
  int scrollX_ = 0;  // 文本水平滚动量
  bool focused_ = false;
  bool dragging_ = false;

  // 光标闪烁
  bool caretOn_ = true;
  UINT_PTR blinkTimer_ = 0;

  InputSpec spec_{};
  ChangeHandler onChange_;
  SubmitHandler onSubmit_;
  std::function<void()> onBlur_;
};

}  // namespace AUI
