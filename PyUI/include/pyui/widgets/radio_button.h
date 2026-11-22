// RadioButton：圆点 + 标签
//
// 单选按钮**不自己管互斥**：同一组由宿主显式指定，因为「一组」的边界是
// 业务概念（同一个分组框？同一个对话框？），控件按父容器猜会把「并排的
// 两组选项」合并成一组。
#pragma once

#include "pyui/painter.h"
#include "pyui/view.h"

#include <functional>
#include <string>
#include <vector>

namespace pyui {

class RadioButton;

// 一组互斥的按钮。不持有所有权：控件析构时自己 leave()，
// 所以这里只存裸指针，且必须处理「按钮先于组销毁」的顺序。
class RadioGroup {
 public:
  RadioGroup() = default;
  ~RadioGroup() { releaseAll(); }
  RadioGroup(const RadioGroup&) = delete;
  RadioGroup& operator=(const RadioGroup&) = delete;

  // 加入组。若组内还没有选中项且该按钮已选中，则它成为选中项。
  void join(RadioButton* button);
  void leave(RadioButton* button);
  void releaseAll();

  // 选中某一项；其余自动取消。传 nullptr 表示全不选（组内应尽量避免，
  // 但初始化时需要一个「未选择」的中间态）。
  void select(RadioButton* button);
  RadioButton* selected() const { return selected_; }

  const std::vector<RadioButton*>& buttons() const { return buttons_; }

  using ChangeHandler = std::function<void(RadioButton*)>;
  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

  // 供 RadioButton 内部调用，不对外
  void notifyChanged(RadioButton* button);

 private:
  std::vector<RadioButton*> buttons_;
  RadioButton* selected_ = nullptr;
  ChangeHandler onChange_;
};

class RadioButton : public View {
 public:
  RadioButton() { setFocusable(true); }
  explicit RadioButton(std::string label) { setLabel(std::move(label)); }
  ~RadioButton() override;

  void setLabel(std::string label);
  const std::string& label() const { return label_; }

  // 加入/离开互斥组
  void setGroup(RadioGroup* group);
  RadioGroup* group() const { return group_; }

  void setChecked(bool checked);
  bool isChecked() const { return checked_; }

  CursorShape cursorShape() const override { return CursorShape::kHand; }

  const char* className() const override { return "RadioButton"; }

  void dispatchClick() override;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;

 private:
  friend class RadioGroup;

  // 只改值并发通知，不处理组的联动（由 RadioGroup::select 负责）
  void setCheckedInternal(bool checked);

  std::string label_;
  bool checked_ = false;
  RadioGroup* group_ = nullptr;
};

}  // namespace pyui
