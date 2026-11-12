// 对话框常用控件：单选组、进度条、分组框、下拉框
//
// 都是纯自绘，行为与平台无关。下拉列表借用平台的弹出菜单：
// 那样在不同系统上都能得到用户熟悉的交互，也不必在通用层维护浮层。
#pragma once

#include "AiUI/layouts/linear_layout.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"
#include "AiUI/view_group.h"

#include <functional>
#include <string>
#include <vector>

namespace AiUI {

class RadioButton;

// 单选组：把同一批 RadioButton 放在一起，选中一个就清掉其余。
// 宿主不必自己记「上一项是谁」。
class RadioGroup : public ViewGroup {
 public:
  RadioGroup() = default;

  using ChangeHandler = std::function<void(int index)>;

  // 索引即加入顺序。XML 里直接写 RadioButton 子节点即可，
  // 这里额外给代码构建提供一条带文字的捷径。
  RadioButton* addItem(const std::wstring& label);
  int itemCount() const { return static_cast<int>(items_.size()); }
  RadioButton* itemAt(int index) const;

  int checkedIndex() const;
  void setCheckedIndex(int index);

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

  // 排布方向：默认纵向；格式选择这类一行几项的场景用横向
  void setOrientation(Orientation orientation) {
    orientation_ = orientation;
    requestLayout();
  }
  Orientation orientation() const { return orientation_; }

  // 项间距（96 DPI 设计值）
  void setRowGap(int gap) { rowGap_ = gap; requestLayout(); }

 protected:
  Size onMeasure(const Size& available) override;
  void onLayout(const Rect& frame) override;
  void onChildAdded(View* child) override;

 private:
  // 某个子项被选中后同步其余子项
  void syncFrom(int index);
  // 首次有子项时默认选第一个，与原生单选组一致
  void ensureInitialChoice();

  std::vector<RadioButton*> items_;
  ChangeHandler onChange_;
  int rowGap_ = 8;
  Orientation orientation_ = Orientation::kVertical;
  bool syncing_ = false;
  bool initialChoiceDone_ = false;
};

// 进度条：0..1 的填充，两端圆角
struct ProgressSpec {
  int height = 10;
  int radius = 5;
  // kNoColor 表示跟随主题
  Color track = kNoColor;
  Color fill = kNoColor;
};

class ProgressBar : public View {
 public:
  ProgressBar() = default;

  void setSpec(const ProgressSpec& spec);
  const ProgressSpec& spec() const { return spec_; }

  // 0..1，超出范围自动夹紧
  void setProgress(float value);
  float progress() const { return progress_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

 private:
  ProgressSpec spec_{};
  float progress_ = 0.0f;
};

// 分组框：标题压在边框上，子视图放在内部
class GroupBox : public ViewGroup {
 public:
  GroupBox() = default;

  void setTitle(const std::wstring& title);
  const std::wstring& title() const { return title_; }

  // 标题行占用的高度（96 DPI 设计值）
  void setTitleHeight(int height) { titleHeight_ = height; requestLayout(); }

 protected:
  Size onMeasure(const Size& available) override;
  void onPaint(Painter& painter, const Rect& rect) override;
  void onLayout(const Rect& frame) override;
  Color backgroundColor() const override { return kNoColor; }

 private:
  std::wstring title_;
  int titleHeight_ = 26;
};

// 下拉框：折叠态显示当前项，点开用平台弹出菜单选出
struct ComboSpec {
  int height = 30;
  int radius = 6;
  int padX = 10;
  // 颜色：kNoColor 表示跟随主题
  Color background = kNoColor;
  Color border = kNoColor;
  Color focusBorder = kNoColor;
  Color ink = kNoColor;
  Color arrow = kNoColor;
  Color disabledInk = kNoColor;
};

class ComboBox : public View {
 public:
  ComboBox() = default;

  using ChangeHandler = std::function<void(int index)>;

  void setSpec(const ComboSpec& spec);
  const ComboSpec& spec() const { return spec_; }

  void setItems(const std::vector<std::wstring>& items);
  void addItem(const std::wstring& item);
  int itemCount() const { return static_cast<int>(items_.size()); }
  const std::wstring& itemAt(int index) const;

  int selectedIndex() const { return selected_; }
  void setSelectedIndex(int index, bool notify = true);
  const std::wstring& selectedText() const;

  void setOnChange(ChangeHandler fn) { onChange_ = std::move(fn); }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  void onFocusChanged(bool focused) override;

 private:
  void openPopup();
  void moveSelection(int delta);

  ComboSpec spec_{};
  std::vector<std::wstring> items_;
  int selected_ = -1;
  bool hot_ = false;
  bool focused_ = false;
  ChangeHandler onChange_;
};

}  // namespace AiUI
