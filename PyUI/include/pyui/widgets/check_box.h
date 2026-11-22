// CheckBox：方框 + 打勾 + 标签
//
// 三态（未选、已选、部分选）一次做全：树形勾选、批量选择都需要「部分选」，
// 后补这个状态会同时牵动点击循环与绘制两处，趁早留好。
#pragma once

#include "pyui/painter.h"
#include "pyui/view.h"

#include <functional>
#include <string>

namespace pyui {

enum class CheckState {
  kUnchecked,
  kChecked,
  kIndeterminate,  // 部分选：部分子项被选中
};

class CheckBox : public View {
 public:
  CheckBox() { setFocusable(true); }
  explicit CheckBox(std::string label) { setLabel(std::move(label)); }

  void setLabel(std::string label);
  const std::string& label() const { return label_; }

  void setState(CheckState state);
  CheckState state() const { return state_; }
  void setChecked(bool checked) { setState(checked ? CheckState::kChecked : CheckState::kUnchecked); }
  bool isChecked() const { return state_ == CheckState::kChecked; }
  // 点击是否在三种状态间循环（部分选 → 已选 → 未选）。
  // 只在部分选有实际意义时才打开，否则用户点一下要按两次才回到未选。
  void setTristate(bool tristate) { tristate_ = tristate; }
  bool isTristate() const { return tristate_; }

  // 状态变化回调；参数是新状态（setState 主动改也会回调）
  using ToggleHandler = std::function<void(CheckState)>;
  void setOnToggle(ToggleHandler fn) { onToggle_ = std::move(fn); }

  CursorShape cursorShape() const override { return CursorShape::kHand; }

  const char* className() const override { return "CheckBox"; }

  void dispatchClick() override;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;

  // 点击后推进状态；返回新状态
  CheckState advanceState() const;

 private:
  // 只改值、发通知，不参与「初始状态」去重
  void setStateInternal(CheckState state);

  std::string label_;
  CheckState state_ = CheckState::kUnchecked;
  bool tristate_ = false;
  ToggleHandler onToggle_;
};

}  // namespace pyui
