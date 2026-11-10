// 面包屑导航条：后退 + 路径节点 + 搜索框
#pragma once

#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <string>
#include <vector>

namespace AiUI {
class InputBox;
}

namespace ui {

// 自绘导航条：平时只读展示路径，点空白处或 Ctrl+L 切换成可编辑输入框。
// 搜索框是它的子控件，位置由本控件在 arrangeChildren 里算出来。
class CrumbBar : public AiUI::Widget {
 public:
  CrumbBar();
  ~CrumbBar() override;

  // 当前视图变化后重建路径节点
  void refresh();
  // 进入地址编辑态（Ctrl+L 或点空白）
  void beginEdit();
  bool isEditing() const { return editing_; }
  // 把焦点交给搜索框（Ctrl+F）
  void focusSearch();

 protected:
  void onCreated() override;
  void onPaint(AiUI::Painter& painter, const AiUI::Rect& rect) override;
  void onMouseDown(const AiUI::MouseEvent& e) override;
  void onMouseUp(const AiUI::MouseEvent& e) override;
  void onMouseMove(const AiUI::MouseEvent& e) override;
  void onMouseLeave() override;
  void arrangeChildren(const AiUI::Rect& content) override;
  AiUI::Color backgroundColor() const override { return AiUI::kColorPane; }

 private:
  struct Node {
    std::wstring label;
    std::wstring target;  // 文件系统目录或包内路径
    bool current = false;
    bool home = false;
    AiUI::Rect rect{};  // 命中原区；空表示放不下，不画也不响应
  };

  void buildNodes();
  void layoutNodes();
  void navTo(const Node& node);
  void showEdit();
  void hideEdit();
  void commitEdit();
  void repositionEdit();

  AiUI::Rect backRect() const;
  AiUI::Rect searchRect() const;
  int hitNode(AiUI::Point pt) const;

  std::vector<Node> nodes_;
  AiUI::InputBox* search_ = nullptr;
  AiUI::InputBox* edit_ = nullptr;
  bool editing_ = false;
  int hot_ = -1;
  bool backHot_ = false;
  bool pressed_ = false;
};

}  // namespace ui
