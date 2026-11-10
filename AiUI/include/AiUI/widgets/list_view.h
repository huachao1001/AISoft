// ListView：自绘文件列表，表头与行都由自己画
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <functional>
#include <string>
#include <vector>

namespace AiUI {

enum class ListAlign { kLeft, kRight };

struct ListColumn {
  std::wstring title;
  int width = 120;  // 96 DPI 设计值
  ListAlign align = ListAlign::kLeft;
  bool stretch = false;  // 吃掉剩余宽度（通常只有名称列）
};

struct ListStyle {
  int headHeight = 40;
  int rowHeight = 44;
  int padX = 16;
  int iconSize = 18;
  int iconGap = 8;
  Color background = kColorPane;
  Color headBg = kColorHeaderBg;
  Color headLine = kColorLineHead;
  Color rowLine = kColorLineSoft;
  Color text = kColorText;
  Color headText = kColorTextHead;
  Color selBg = kColorSelBg;
  Color hoverBg = kColorRowHover;
};

// 调用方提供数据与事件；控件只做布局、选择、滚动、列宽与排序指示
struct ListCallbacks {
  std::function<int()> rowCount;
  std::function<std::wstring(int row, int column)> text;
  // 行首图标由调用方画；坐标是控件客户区绝对坐标
  std::function<void(Painter& painter, int row, const Rect& rect)> drawIcon;
  std::function<void(int row)> onActivate;  // 双击或回车
  std::function<void()> onSelectionChanged;
  std::function<void(int column, bool ascending)> onSort;
  std::function<void()> onContextMenu;
  std::function<void(Key key)> onKeyDown;  // 未消费的按键
  std::function<void()> onColumnResized;
};

class ListView : public Widget {
 public:
  ListView() { className_ = L"AiUIListView"; }

  void setStyle(const ListStyle& style);
  void setColumns(std::vector<ListColumn> columns);
  void setCallbacks(ListCallbacks callbacks);

  void reload();
  void layoutColumns();

  int sortColumn() const { return sortColumn_; }
  bool sortAscending() const { return sortAscending_; }
  void setSort(int column, bool ascending);

  // 显式指定列宽（物理像素）；用于恢复持久化的宽度
  void setUserWidth(int column, int physicalWidth);

  std::vector<int> selectedRows() const;
  void clearSelection();
  void selectAll();
  void selectRow(int row, bool scrollIntoView = true);
  int focusedRow() const { return anchor_; }
  int selectedCount() const;
  int columnWidth(int column) const;

  // 需要双击消息，窗口类注册时已带 CS_DBLCLKS

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onLayout(const Rect& frame) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onMouseWheel(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  void onContextMenu(const MouseEvent& e) override;
  void onScroll(const ScrollEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  Color backgroundColor() const override { return style_.background; }

 private:
  int headHeight() const;
  int rowHeight() const;
  int contentTop() const;
  int rowCount() const;
  int pageHeight() const;

  Rect rowRect(int row) const;
  int cellLeft(int column) const;
  int hitRow(Point pt) const;
  int columnAtX(int x) const;

  void updateScrollBar();
  void setScroll(int position);
  void scrollBy(int delta);
  void ensureRowVisible(int row);
  void syncSelection(int row, bool ctrl, bool shift);
  void paintHeader(Painter& painter, const Rect& rect);
  void paintRow(Painter& painter, int row, const Rect& rowRect);
  int maxScroll() const;

  ListStyle style_{};
  std::vector<ListColumn> columns_;
  std::vector<int> colWidth_;   // 物理像素
  std::vector<int> userWidth_;  // 用户拖过的宽度，0 表示未拖
  ListCallbacks callbacks_;

  std::vector<bool> selected_;
  int anchor_ = -1;
  int hot_ = -1;
  int scroll_ = 0;

  int sortColumn_ = 0;
  bool sortAscending_ = true;

  int dragColumn_ = -1;
  int dragStartX_ = 0;
  int dragStartWidth_ = 0;
};

}  // namespace AiUI
