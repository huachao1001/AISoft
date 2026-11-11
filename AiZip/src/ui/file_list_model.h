// 文件列表模型：排序、过滤、选中语义
//
// 只认数据不认控件：行内容、排序规则、过滤、选中项换算全在这里，
// 列表控件通过适配器按需来取。这样界面装配（MainActivity）与业务判断
// （能否解压、删除哪些）不会缠在一起。
#pragma once

#include "app.h"

#include <string>
#include <vector>

namespace ui {

// 列定义：名称 / 类型 / 大小 / 修改时间
enum ListColumnId { kColName = 0, kColType, kColSize, kColModified, kColCount };

class FileListModel {
 public:
  // 按当前排序与过滤重建可见行；数据变化后由调用方触发
  void rebuild();

  // 列与行内容
  static std::wstring cellText(int column, const Item& item);
  // 行首图标依据
  static bool isFolderIcon(const Item& item) { return item.isDir; }

  // 排序：文件夹恒在前，其余按列比较
  void sort(int column, bool ascending);
  int sortColumn() const { return sortColumn_; }
  bool sortAscending() const { return sortAscending_; }

  // 过滤：空串表示不过滤
  void setFilter(const std::wstring& text);
  const std::wstring& filter() const { return filter_; }

  // 可见行数与换算：可见行号 ↔ g_app.items 下标
  int rowCount() const { return static_cast<int>(rows_.size()); }
  int itemIndexAt(int row) const;
  const Item* itemAt(int row) const;
  const std::vector<int>& rows() const { return rows_; }

  // 选中项换算成 g_app.items 下标；过滤后也不会错位
  std::vector<int> selectedItemIndices(const std::vector<int>& selectedRows) const;

  // 能否解压：包内模式任意选中项可提取；文件系统下只允许单个受支持的压缩包。
  // 没有选中项时退回左侧树的当前节点判断，因此需要传入树的结论。
  static bool canExtract(const std::vector<int>& selected, bool treeCanExtract);

 private:
  bool matches(const Item& item) const;
  static uint64_t sortKey(int column, const Item& item);
  static bool lessByColumn(int column, const Item& a, const Item& b);

  int sortColumn_ = kColName;
  bool sortAscending_ = true;
  std::wstring filter_;
  // 可见行：值为 g_app.items 下标
  std::vector<int> rows_;
};

}  // namespace ui
