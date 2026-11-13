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
  // 尾部图标：密码显隐这类行内按钮。与前端图标一样参与文字区宽度计算，
  // 否则文字会压到图标上，点击热区也会与正文重叠。
  bool hasTailIcon = false;
  IconId tailIcon = IconId::kNone;
  int tailIconSize = 16;
  int tailIconGap = 6;
  int padX = 11;
  int radius = 7;
  // 插入点竖线宽度（96 DPI 设计值）。实际绘制固定 1px，见 caretBarWidth()
  int caretWidth = 1;
  // 密码模式：显示为圆点，但仍可选中与复制
  bool password = false;

  // ── 颜色：kNoColor 表示跟随主题 ──
  Color background = kNoColor;
  Color border = kNoColor;
  Color focusBorder = kNoColor;
  Color text = kNoColor;
  Color cueColor = kNoColor;
  Color leadIconColor = kNoColor;
  Color tailIconColor = kNoColor;
  Color selBg = kNoColor;
  Color caret = kNoColor;
};

class InputBox : public View {
 public:
  InputBox();

  using ChangeHandler = std::function<void()>;
  using SubmitHandler = std::function<void(bool committed)>;
  using BlurHandler = std::function<void()>;

  // 虚函数：PasswordBox 覆盖它，保证 XML 注入 spec 之后掩码与图标仍与状态一致
  virtual void setSpec(const InputSpec& spec);
  const InputSpec& spec() const { return spec_; }

  const std::wstring& text() const { return text_; }
  void setText(const std::wstring& text);
  void setCue(const std::wstring& cue);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }
  // 回车提交 / Esc 取消
  void setOnSubmit(SubmitHandler fn) { onSubmit_ = std::move(fn); }
  void setOnBlur(BlurHandler fn) { onBlur_ = std::move(fn); }
  // 尾部图标点击（密码显隐）。未挂回调时图标只是装饰
  void setOnTailIcon(ChangeHandler fn) { onTailIcon_ = std::move(fn); }
  void setTailIcon(IconId icon);

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
  // 拿到真实宽度后重新校正滚动：setText 常在布局前调用，那时 clientRect 为空
  void onLayout(const Rect& frame) override;
  // 尾部图标的矩形；没有尾部图标时返回空矩形。
  // 放在 protected：子类可能要贴着图标排布自己的标记
  Rect tailIconRect() const;

 private:
  Rect textRect() const;
  int textLeft() const;
  // 文本可视区右边界：裁剪、绘制宽度、滚动量共用
  int textRight() const;
  // 插入点宽度（物理像素），固定 1px
  int caretBarWidth() const;
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
  ChangeHandler onTailIcon_;
  // 尾部图标按下态：点击才生效，按下即切换会误触
  bool tailPressed_ = false;
};

}  // namespace AiUI
