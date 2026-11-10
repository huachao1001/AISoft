// 自绘文件列表：表头与行都自己画，不使用 SysListView32 / SysHeader32
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "AUI/icon.h"
#include "AUI/painter.h"
#include "AUI/theme.h"
#include "AUI/widget.h"

namespace AUI {

enum class ListAlign { kLeft, kRight };

struct ListColumn {
  std::wstring title;
  int width = 120;  // 96 DPI 设计值
  ListAlign align = ListAlign::kLeft;
  bool stretch = false;  // 吃掉剩余宽度（通常只有名称列）
};

struct ListStyle {
  int headH = 40;
  int rowH = 44;
  int padX = 16;
  int iconSize = 18;
  int iconGap = 8;
  COLORREF bg = kPaneBg;
  COLORREF headBg = kThBg;
  COLORREF headLine = kLineHead;
  COLORREF rowLine = kLineSoft;
  COLORREF text = kText;
  COLORREF headText = kTextHead;
  COLORREF selBg = kSelBg;
  COLORREF hotBg = kRowHover;
};

// 调用方提供数据与事件；控件只做布局、选择、滚动、列宽与排序指示
struct ListCallbacks {
  std::function<int()> rowCount;
  std::function<std::wstring(int row, int column)> text;
  // 行首图标由调用方画，坐标是控件客户区绝对坐标
  std::function<void(Painter& p, int row, const RECT& rc)> drawIcon;
  std::function<void(int row)> onActivate;  // 双击或回车
  std::function<void()> onSelectionChanged;
  std::function<void(int column, bool asc)> onSort;
  std::function<void()> onContextMenu;
  std::function<void(int vk)> onKeyDown;   // 未消费的按键
  std::function<void()> onColumnResized;   // 拖列宽结束
};

class ListView : public Widget {
 public:
  void SetStyle(const ListStyle& style);
  void SetColumns(std::vector<ListColumn> columns);
  void SetCallbacks(ListCallbacks cb);

  // 行数或内容变化后调用
  void Reload();
  // 重算列宽（拉伸列吃掉剩余宽度）
  void LayoutColumns();

  int sortColumn() const { return sortColumn_; }
  bool sortAsc() const { return sortAsc_; }
  void SetSort(int column, bool asc);

  // 显式指定列宽（物理像素）；常用于恢复用户持久化的宽度
  void SetUserWidth(int column, int physicalWidth);

  // 选中行下标
  std::vector<int> SelectedRows() const;
  void ClearSelection();
  void SelectAll();
  void SelectRow(int row, bool scrollIntoView = true);
  int FocusedRow() const { return anchor_; }
  // 当前选中数
  int selectedCount() const;
  // 拖过的列宽，0 表示尚未拖过
  int ColumnWidth(int column) const;

  // 绘制资源：列表不依赖原生控件，双击要自己声明 CS_DBLCLKS
  static constexpr UINT kClassStyle = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;

 protected:
  void OnSize(int cx, int cy) override;
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;
  LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

 private:
  int HeadHeight() const { return px(style_.headH); }
  int RowHeight() const { return px(style_.rowH); }
  int ContentTop() const { return HeadHeight(); }
  int RowCount() const;
  int PageHeight() const { return ClientRect().bottom - ContentTop(); }

  RECT RowRect(int row) const;
  int CellLeft(int column) const;
  int HitRow(POINT pt) const;
  int ColumnAtX(int x) const;

  void UpdateScrollBar();
  void ScrollTo(int pos);
  void EnsureRowVisible(int row);
  void SyncSelection(int row, bool ctrl, bool shift);
  void PaintHeader(Painter& p);
  void PaintRow(Painter& p, int row, const RECT& rr);

  ListStyle style_{};
  std::vector<ListColumn> columns_;
  std::vector<int> colWidth_;  // 物理像素宽度
  std::vector<int> userWidth_; // 用户拖过的宽度，0 表示未拖
  ListCallbacks cb_;

  std::vector<bool> selected_;
  int anchor_ = -1;  // 焦点行，也是 shift 选择的锚点
  int hot_ = -1;
  int scroll_ = 0;
  bool tracking_ = false;

  int sortColumn_ = 0;
  bool sortAsc_ = true;

  // 列宽拖动
  int dragCol_ = -1;
  int dragStartX_ = 0;
  int dragStartW_ = 0;
};

}  // namespace AUI
