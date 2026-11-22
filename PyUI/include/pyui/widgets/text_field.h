// TextField：单行文本输入
//
// 这是全库唯一需要「文字四处同源」真正落地的地方：光标的 x 必须由
// advanceText 得到，选区的左右边界必须由 glyphBounds 得到，绘制的起点必须
// 与它们共用同一份排版。任何一处单独实现，光标就会与文字错开半个留白。
//
// 输入法：组合中的文本单独存在 composition_ 里并画下划线，不混进 value_。
// 混进去的后果是「用户还在选词，内容已经被当成提交值通知出去了」。
#pragma once

#include "pyui/painter.h"
#include "pyui/text_renderer.h"
#include "pyui/view.h"

#include <functional>
#include <string>

namespace pyui {

class TextField : public View {
 public:
  TextField();
  ~TextField() override;

  // ── 内容 ──
  void setText(std::string text);
  const std::string& text() const { return text_; }
  void setPlaceholder(std::string text) { placeholder_ = std::move(text); }
  const std::string& placeholder() const { return placeholder_; }

  // 掩码字符：非 0 时显示为密码框。用码点而不是字符串，
  // 因为一个掩码字符可能占多个字节（如 '●' U+25CF 是三字节）。
  void setPasswordChar(std::uint32_t codepoint, bool enabled = true);
  // 开关密码框，掩码字符用默认的 U+25CF
  void setPassword(bool enabled);
  bool isPassword() const { return password_; }

  // ── 编辑状态 ──
  // 光标位置以**字节偏移**表示，且保证落在 UTF-8 字符边界上。
  void setCaretByte(size_t byteOffset);
  size_t caretByte() const { return caret_; }
  void setSelection(size_t anchorByte, size_t caretByte);
  // 选区；两者相等表示无选区
  size_t selectionStart() const { return selStart_; }
  size_t selectionEnd() const { return selEnd_; }
  bool hasSelection() const { return selStart_ != selEnd_; }
  void selectAll();
  void clearSelection();

  // ── 外观 ──
  void setAlign(TextAlign align) { align_ = align; }
  TextAlign align() const { return align_; }
  void setReadOnly(bool readOnly) { readOnly_ = readOnly; }
  bool isReadOnly() const { return readOnly_; }

  // ── 回调 ──
  // 每次内容变化（含输入法提交）都会回调
  using ChangeHandler = std::function<void(const std::string&)>;
  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }
  // 回车；返回 true 表示已消费
  using SubmitHandler = std::function<bool(const std::string&)>;
  void setOnSubmit(SubmitHandler fn) { onSubmit_ = std::move(fn); }

  CursorShape cursorShape() const override { return CursorShape::kIBeam; }

  const char* className() const override { return "TextField"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  bool onChar(std::uint32_t codepoint) override;
  void onFocusChanged(bool focused) override;
  void onTimer() override;

 private:
  Font font() const;
  // 内容区（去掉内边距与描边）；光标与文字都在这里排版
  Rect textRect(const Rect& rect) const;
  // 可见文字（密码框返回掩码后的串）
  std::string displayText() const;
  // 字节偏移 → 排版本 x（相对文字区左缘）
  int byteToX(size_t byteOffset, const Rect& area) const;
  // 排版起点（含对齐偏移与水平滚动）；byteToX / xToByte / 绘制必须共用它
  int textOriginX(const Rect& area) const;
  // 排版本 x → 最近的字节偏移（落在字符边界上）
  size_t xToByte(int x, const Rect& area) const;
  // 保证光标可见：必要时滚动水平偏移
  void clampScroll(const Rect& area);
  void notifyChange();
  // 删除选区或光标前的字符
  void deleteBackward();
  void deleteForward();
  std::string selectedText() const;

  std::string text_;
  std::string placeholder_;
  std::uint32_t passwordChar_ = 0;
  bool password_ = false;
  bool readOnly_ = false;

  size_t caret_ = 0;
  size_t selAnchor_ = 0;
  size_t selStart_ = 0;
  size_t selEnd_ = 0;

  TextAlign align_ = TextAlign::kLeft;
  int padding_ = 10;
  int scrollX_ = 0;   // 水平滚动偏移（密码/长文本用）
  bool dragging_ = false;

  ChangeHandler onChange_;
  SubmitHandler onSubmit_;
};

}  // namespace pyui
