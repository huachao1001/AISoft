// ChoiceCard：带大图标的整块可选项
//
// 安装器的「修复 / 卸载」这类页面需要的不是一行 16px 的小圆点，而是一块
// 可以一眼选中、点哪儿都算数的大卡片：图标在上、标题与说明在下，选中时
// 整块描边并上浮底色。
//
// 为什么单独做一个控件而不是给 RadioButton 加图标：
// RadioButton 的盒子尺寸、文字基线、行高都是按「一行选项」定的，塞进 48px
// 图标后整组的高度计算、垂直居中都会跟着变形，而它又被表单到处复用。
// 这里是一个平级的控件，复用 OptionView 的选中语义之外的绘制自己来。
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"
#include "AiUI/view_group.h"

#include <functional>
#include <string>
#include <vector>

namespace AiUI {

struct ChoiceCardSpec {
  std::wstring title;
  std::wstring detail;
  IconId icon = IconId::kNone;
  bool checked = false;
  // 是否画成单选（圆形指示点）。false 时画方形勾选盒。
  bool radio = true;
  // 96 DPI 设计值
  int radius = 10;
  int padX = 16;
  int padY = 14;
  int iconSize = 44;
  int iconGap = 14;
  int titleGap = 4;
  // 颜色：kNoColor 表示跟随主题
  Color background = kNoColor;
  Color border = kNoColor;
  Color checkedBackground = kNoColor;
  Color checkedBorder = kNoColor;
  Color ink = kNoColor;
  Color detailInk = kNoColor;
  Color iconInk = kNoColor;
  Color checkedIconInk = kNoColor;
  Color hoverBackground = kNoColor;
  // 是否显示右上角的选中指示点
  bool showIndicator = true;
};

// 一块选项卡。点击任意位置切换；单选模式下不会自己取消自己
// （取消由所属的组负责，与 RadioButton 一致）。
class ChoiceCard : public View {
 public:
  using ChangeHandler = std::function<void(bool checked)>;

  void setSpec(const ChoiceCardSpec& spec);
  const ChoiceCardSpec& spec() const { return spec_; }

  void setTitle(const std::wstring& title);
  void setDetail(const std::wstring& detail);

  bool isChecked() const { return spec_.checked; }
  // notify 为 false 时不触发 onChange，供组内同步外观
  void setChecked(bool checked, bool notify = true);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;

 private:
  ChoiceCardSpec spec_{};
  ChangeHandler onChange_;
  bool hot_ = false;
  bool pressed_ = false;
};

// 卡片组：与 RadioGroup 同样的语义，但排布与卡片对齐方式由它决定。
//
// 单选（默认）保证任意时刻恰好一项被选中；复选模式（radio=false）允许多选。
class ChoiceCardGroup : public ViewGroup {
 public:
  ChoiceCardGroup() = default;

  ChoiceCard* cardAt(int index) const;
  int cardCount() const { return static_cast<int>(cards_.size()); }

  int checkedIndex() const;
  void setCheckedIndex(int index);
  std::vector<int> checkedIndices() const;

  // 单选（默认）或复选。切换后不影响已选中的项。
  void setRadio(bool radio) { radio_ = radio; }
  bool isRadio() const { return radio_; }

  // 卡片间距（96 DPI 设计值）
  void setGap(int gap) {
    gap_ = gap;
    requestLayout();
  }

  using ChangeHandler = std::function<void(int index)>;
  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

 protected:
  Size onMeasure(const Size& available) override;
  void onLayout(const Rect& frame) override;
  void onChildAdded(View* child) override;

 private:
  void syncFrom(int index);

  std::vector<ChoiceCard*> cards_;
  ChangeHandler onChange_;
  int gap_ = 10;
  bool radio_ = true;
  bool syncing_ = false;
};

}  // namespace AiUI
