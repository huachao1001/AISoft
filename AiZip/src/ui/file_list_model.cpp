// 文件列表模型实现
#include "file_list_model.h"

#include "util.h"

#include "AiUI/platform.h"

#include <algorithm>

namespace ui {

std::wstring FileListModel::cellText(int column, const Item& item) {
  switch (column) {
    case kColType:
      return item.typeName;
    case kColSize:
      // 文件夹不统计大小，留空比画横线干净
      return item.isDir ? L"" : util::FormatSize(item.size);
    case kColModified:
      return item.modifiedMillis > 0 ? util::FormatTime(item.modifiedMillis) : L"";
    default:
      return item.name;
  }
}

uint64_t FileListModel::sortKey(int column, const Item& item) {
  switch (column) {
    case kColSize:
      return item.size;
    case kColModified:
      return static_cast<uint64_t>(item.modifiedMillis);
    default:
      return 0;
  }
}

bool FileListModel::lessByColumn(int column, const Item& a, const Item& b) {
  const bool numeric = (column == kColSize || column == kColModified);
  if (numeric) return sortKey(column, a) < sortKey(column, b);
  // 文件名比较忽略大小写，避免排序结果随系统区域设置变化
  return AiUI::platform()->compareText(cellText(column, a), cellText(column, b)) < 0;
}

void FileListModel::sort(int column, bool ascending) {
  sortColumn_ = column;
  sortAscending_ = ascending;
  std::stable_sort(g_app.items.begin(), g_app.items.end(), [&](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;  // 文件夹恒在前
    const bool less = lessByColumn(column, a, b);
    return ascending ? less : !less;
  });
}

bool FileListModel::matches(const Item& item) const {
  if (filter_.empty()) return true;
  return AiUI::platform()->containsTextIgnoreCase(item.name, filter_);
}

void FileListModel::rebuild() {
  rows_.clear();
  rows_.reserve(g_app.items.size());
  for (int i = 0; i < static_cast<int>(g_app.items.size()); ++i) {
    if (matches(g_app.items[i])) rows_.push_back(i);
  }
}

void FileListModel::setFilter(const std::wstring& text) {
  if (text == filter_) return;
  filter_ = text;
}

int FileListModel::itemIndexAt(int row) const {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
  return rows_[row];
}

const Item* FileListModel::itemAt(int row) const {
  const int index = itemIndexAt(row);
  if (index < 0 || index >= static_cast<int>(g_app.items.size())) return nullptr;
  return &g_app.items[index];
}

std::vector<int> FileListModel::selectedItemIndices(const std::vector<int>& selectedRows) const {
  std::vector<int> out;
  out.reserve(selectedRows.size());
  for (int row : selectedRows) {
    const int index = itemIndexAt(row);
    if (index >= 0) out.push_back(index);
  }
  return out;
}

bool FileListModel::canExtract(const std::vector<int>& selected, bool treeCanExtract) {
  if (selected.empty()) return treeCanExtract;
  if (g_app.archiveMode) return true;
  if (selected.size() != 1) return false;
  const int index = selected[0];
  if (index < 0 || index >= static_cast<int>(g_app.items.size())) return false;
  const Item& item = g_app.items[index];
  return !item.isDir && arc::Archive::IsArchiveFile(item.fullPath);
}

}  // namespace ui
