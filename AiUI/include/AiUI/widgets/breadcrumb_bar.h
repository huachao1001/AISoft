// BreadcrumbBar：后退按钮 + 路径节点 + 搜索框，全部自绘
//
// 调用方只负责传路径段和回调，节点宽度、省略、命中区都在内部算。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <functional>
#include <string>
#include <vector>

namespace AiUI {

class InputBox;

// 一个路径节点。tag 是业务标识，库只原样回传。
struct BreadcrumbSegment {
  std::wstring label;
  std::wstring tag;
  bool current = false;  // 当前所在层，画成高亮底
  bool home = false;     // 段首画房子图标
};

struct BreadcrumbStyle {
  int padX = 16;
  int nodePadX = 7;
  int nodePadY = 4;
  int nodeGap = 8;
  int backWidth = 30;
  int backHeight = 30;
  int iconSize = 16;
  int iconGap = 6;
  int radius = 5;
  int searchWidth = 270;
  int searchHeight = 34;
  int editHeight = 32;
  Color background = kColorPane;
  Color text = kColorLink;
  Color textCurrent = makeColor(0x11, 0x11, 0x11);
  Color backInk = kColorInk;
  Color backInkDisabled = kColorTextFaint;
  Color hoverBg = kColorLinkHover;
  Color currentBg = kColorCurrentBg;
  Color separator = makeColor(0x9c, 0xa3, 0xaf);
  Color line = kColorLine;
};

struct BreadcrumbCallbacks {
  std::function<void()> onBack;
  std::function<void(const std::wstring& tag)> onNavigate;
  std::function<void(const std::wstring& text)> onSearch;
  // 进入编辑态时填进输入框的地址
  std::function<std::wstring()> addressText;
  // 提交编辑；路径合法性由业务判断
  std::function<void(const std::wstring& text)> onCommitAddress;
  // 编辑态结束，用于把焦点交回主视图
  std::function<void()> onEditClosed;
};

class BreadcrumbBar : public Widget {
 public:
  BreadcrumbBar() { className_ = L"AiUIBreadcrumbBar"; }
  ~BreadcrumbBar() override;

  void setSegments(std::vector<BreadcrumbSegment> segments);
  void setStyle(const BreadcrumbStyle& style);
  void setCallbacks(BreadcrumbCallbacks callbacks);
  void setBackEnabled(bool enabled);
  void setSearchCue(const std::wstring& cue);
  // 清空搜索框内容
  void clearSearch();

  // 进入地址编辑态（点空白处或 Ctrl+L）
  void beginEdit();
  bool isEditing() const { return editing_; }
  // 把焦点交给搜索框（Ctrl+F）
  void focusSearch();

 protected:
  void onCreated() override;
  void onPaint(Painter& painter, const Rect& rect) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onLayout(const Rect& frame) override;
  void arrangeChildren(const Rect& content) override;
  Color backgroundColor() const override { return style_.background; }

 private:
  struct Node {
    BreadcrumbSegment data;
    Rect rect{};  // 空表示放不下，不画也不响应
  };

  void layoutNodes();
  Rect backRect() const;
  Rect searchRect() const;
  int hitNode(Point pt) const;
  void showEdit();
  void hideEdit();

  BreadcrumbStyle style_{};
  BreadcrumbCallbacks callbacks_;
  std::vector<Node> nodes_;
  InputBox* search_ = nullptr;
  InputBox* edit_ = nullptr;
  bool backEnabled_ = false;
  bool editing_ = false;
  int hot_ = -1;
  bool backHot_ = false;
  bool pressed_ = false;
};

}  // namespace AiUI
