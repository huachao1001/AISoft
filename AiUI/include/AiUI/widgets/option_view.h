// CheckBox / RadioButton：带文字的选项行
//
// 两者只有盒子的画法不同（方框打勾 / 圆点），选中语义与交互完全一致，
// 因此共用一份实现，RadioGroup 通过 isRadio 区分。
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <functional>
#include <string>

namespace AiUI {

struct OptionSpec {
  std::wstring label;
  bool checked = false;
  // 96 DPI 设计值
  int boxSize = 16;
  int gap = 8;
  int radius = 4;
  // 颜色：kNoColor 表示跟随主题
  Color ink = kNoColor;
  Color boxBackground = kNoColor;
  Color boxBorder = kNoColor;
  Color accent = kNoColor;
  Color hoverBackground = kNoColor;
  Color checkInk = kNoColor;
};

// 选项行的公共基类：绘制盒子与文字，点击切换选中
class OptionView : public View {
 public:
  using ChangeHandler = std::function<void(bool checked)>;

  void setSpec(const OptionSpec& spec);
  const OptionSpec& spec() const { return spec_; }

  void setLabel(const std::wstring& label);
  const std::wstring& label() const { return spec_.label; }

  bool isChecked() const { return spec_.checked; }
  // notify 为 false 时不触发 onChange，供 RadioGroup 成组设置
  void setChecked(bool checked, bool notify = true);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

  // 圆形盒子为单选，方形为复选
  virtual bool isRadio() const { return false; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;

 private:
  Rect boxRect(const Rect& rect) const;

  OptionSpec spec_{};
  ChangeHandler onChange_;
  bool hot_ = false;
  bool pressed_ = false;
};

class CheckBox : public OptionView {
 public:
  CheckBox() = default;
};

// 单选按钮：选中其它项时由 RadioGroup 负责清除自身
class RadioButton : public OptionView {
 public:
  RadioButton() = default;
  bool isRadio() const override { return true; }
};

}  // namespace AiUI
