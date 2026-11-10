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

// 行首图标的来源：矢量图标或位图，二者取其一
struct ListIcon {
  IconId glyph = IconId::kNone;
  Image image;           // 有效时优先于 glyph
  Color color = kNoColor;  // kNoColor 表示用样式默认色
};

// 单元格：文本 + 可选配色；图标只画在第 0 列
struct ListCell {
  std::wstring text;
  Color textColor = kNoColor;
};

// 一行数据。库内只按行号保存选中状态，业务标识放在 tag 里。
struct ListRow {
  std::vector<ListCell> cells;
  ListIcon icon;
  // 业务方自定义的整型标记（如条目下标），库不解释
  int tag = -1;
  // 整行禁用外观（如占位行）
  bool dimmed = false;
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

// 事件回调：只管交互通知，不再由调用方提供渲染数据
struct ListCallbacks {
  std::function<void(int row)> onActivate;  // 双击或回车，参数是行号
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

  // 整表替换数据；行号即选中索引
  void setRows(std::vector<ListRow> rows);
  const std::vector<ListRow>& rows() const { return rows_; }
  int rowCount() const { return static_cast<int>(rows_.size()); }
  // 取某行的业务标记；越界返回 -1
  int rowTag(int row) const;

  // 列宽随控件宽度重新分配
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
  // 只重绘一行（夹在表头之下）
  void invalidateRow(int row);
  // 只重绘行区，不动表头
  void invalidateContent();

  ListStyle style_{};
  std::vector<ListColumn> columns_;
  std::vector<int> colWidth_;   // 物理像素
  std::vector<int> userWidth_;  // 用户拖过的宽度，0 表示未拖
  std::vector<ListRow> rows_;
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
