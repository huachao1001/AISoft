// ListView：自绘虚拟列表，适配器按需供数，行容器循环复用
//
// 继承 ScrollView：滚动位置、滚轮、滚动条都由基类管。表头固定在顶部，
// 行区在表头之下滚动，所以 contentHeight() 只算行、viewportHeight() 扣表头。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/scroll_view.h"
#include "AiUI/theme.h"

#include <cstdint>
#include <functional>
#include <memory>
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
  Image image;             // 有效时优先于 glyph
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

// 数据适配器：库只问行数和某一行怎么画，不保存整表。
// 实现时把 bindRow 当作"把 row 摊平到 out"，可只填用得到的列。
class ListAdapter {
 public:
  virtual ~ListAdapter() = default;
  virtual int rowCount() const = 0;
  virtual void bindRow(int row, ListRow& out) const = 0;
  // 行号对应的业务标记；默认行号本身
  virtual int tagAt(int row) const { return row; }
};

// 函数式适配器：不写类，配好数据来源即可
class ListRowSource : public ListAdapter {
 public:
  using Counter = std::function<int()>;
  using Binder = std::function<void(int row, ListRow& out)>;
  using TagAt = std::function<int(int row)>;

  void setCount(Counter counter) { count_ = std::move(counter); }
  void setBinder(Binder binder) { binder_ = std::move(binder); }
  void setTagAt(TagAt tagAt) { tag_ = std::move(tagAt); }

  int rowCount() const override;
  void bindRow(int row, ListRow& out) const override;
  int tagAt(int row) const override;

 private:
  Counter count_;
  Binder binder_;
  TagAt tag_;
};

// 整表适配器：数据本身就在库这边时最省事
class ListArrayAdapter : public ListAdapter {
 public:
  explicit ListArrayAdapter(std::vector<ListRow> rows) : rows_(std::move(rows)) {}

  int rowCount() const override { return static_cast<int>(rows_.size()); }
  void bindRow(int row, ListRow& out) const override;
  int tagAt(int row) const override;

  const std::vector<ListRow>& rows() const { return rows_; }

 private:
  std::vector<ListRow> rows_;
};

// 行容器：一条可视行对应一个实例，滚动时换绑不同行号，对象与
// cells 容量在整个生命周期内复用，长列表不会反复分配。
struct ListViewHolder {
  int row = -1;             // 当前绑定的行号，-1 表示空闲
  uint64_t generation = 0;  // 与 ListView 的数据版本比对，决定是否重绑
  ListRow data;
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

class ListView : public ScrollView {
 public:
  ListView();
  ~ListView() override;

  void setStyle(const ListStyle& style);
  void setColumns(std::vector<ListColumn> columns);
  void setCallbacks(ListCallbacks callbacks);

  // 换适配器：库只按需问它要数据，不拷贝整表
  void setAdapter(std::shared_ptr<ListAdapter> adapter);
  const std::shared_ptr<ListAdapter>& adapter() const { return adapter_; }
  // 数据源内容变了：行数可能变、可见行要重绑
  void notifyDataSetChanged();
  // 只有某几行内容变了，行数没变；不做全表刷新
  void notifyRowsChanged(int firstRow, int lastRow);

  int rowCount() const;
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

  // ── ScrollView ──
  // 只算行；表头不参与滚动
  int contentHeight() const override;
  // 扣掉表头后的可视高度
  int viewportHeight() const override;
  // 滚动条从表头下沿开始，不盖住表头
  int viewportTop() const override { return headHeight(); }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onDoubleClick(const MouseEvent& e) override;
  void onContextMenu(const MouseEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  void onScrollChanged() override;
  void onLayout(const Rect& frame) override;
  Color backgroundColor() const override { return style_.background; }
  // 一格滚三行
  int wheelStep() const override { return style_.rowHeight * 3; }

 private:
  int headHeight() const;
  int rowHeight() const;
  int pageHeight() const;

  Rect rowRect(int row) const;
  int cellLeft(int column) const;
  int hitRow(Point pt) const;
  int columnAtX(int x) const;

  // 可视行池：滚进视野取一个 holder 绑定行号，滚出去放回池子
  ListViewHolder* acquireHolder(int row);
  void recycleHolders();
  // 让 holder 与当前数据版本对齐；内容和行号都没变就跳过绑定
  void bindHolder(ListViewHolder* holder, int row);

  void ensureRowVisible(int row);
  void syncSelection(int row, bool ctrl, bool shift);
  void paintHeader(Painter& painter, const Rect& rect);
  void paintRow(Painter& painter, const ListRow& data, int row, const Rect& rowRect);
  // 只重绘一行（夹在表头之下）
  void invalidateRow(int row);
  // 只重绘行区，不动表头
  void invalidateContent();

  ListStyle style_{};
  std::vector<ListColumn> columns_;
  std::vector<int> colWidth_;   // 物理像素
  std::vector<int> userWidth_;  // 用户拖过的宽度，0 表示未拖
  std::shared_ptr<ListAdapter> adapter_;
  std::vector<ListViewHolder> holderPool_;
  uint64_t generation_ = 1;
  ListCallbacks callbacks_;

  std::vector<bool> selected_;
  int anchor_ = -1;
  int hot_ = -1;

  int sortColumn_ = 0;
  bool sortAscending_ = true;

  int dragColumn_ = -1;
  int dragStartX_ = 0;
  int dragStartWidth_ = 0;
};

}  // namespace AiUI
