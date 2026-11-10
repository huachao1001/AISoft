// 文件列表：数据与交互编排，绘制/选择/滚动由 AiUI::ListView 负责
#include "ui_list.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui_main.h"
#include "ui_status.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "util.h"
#include "AiUI/theme.h"
#include "AiUI/widgets/list_view.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <memory>

namespace ui {
namespace {

AiUI::ListView* g_view = nullptr;
// 列表适配器：库滚动到哪一行才来这里取数据，长目录不预先构建整表
std::shared_ptr<AiUI::ListRowSource> g_adapter;
int g_sortColumn = 0;
bool g_sortAsc = true;

// 过滤后的可见行：值为 g_app.items 下标
std::vector<int> g_rows;
std::wstring g_filter;

enum Col { kColName = 0, kColType, kColSize, kColModified, kColCount };

struct ColDef {
  const wchar_t* title;
  int width;
  bool rightAligned;
  bool stretch;  // 名称列吃掉剩余宽度
};

// 名称 / 类型 / 大小 / 修改时间
const ColDef kCols[kColCount] = {
    {L"名称", 320, false, true},
    {L"类型", 110, false, false},
    {L"大小", 100, true, false},
    {L"修改时间", 160, false, false},
};

std::wstring ItemText(int column, const Item& item) {
  switch (column) {
    case kColType:
      return item.typeName;
    case kColSize:
      // 文件夹不统计大小，留空比画横线干净
      return item.isDir ? L"" : util::FormatSize(item.size);
    case kColModified:
      return (item.mtime.dwLowDateTime || item.mtime.dwHighDateTime)
                 ? util::FormatTime(item.mtime)
                 : L"";
    default:
      return item.name;
  }
}

uint64_t ItemNumber(int column, const Item& item) {
  switch (column) {
    case kColSize:
      return item.size;
    case kColModified:
      return (static_cast<uint64_t>(item.mtime.dwHighDateTime) << 32) | item.mtime.dwLowDateTime;
    default:
      return 0;
  }
}

void SortItems(int column, bool ascending) {
  const bool numeric = (column == kColSize || column == kColModified);
  std::stable_sort(g_app.items.begin(), g_app.items.end(), [&](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;  // 文件夹恒在前
    bool less;
    if (numeric) {
      less = ItemNumber(column, a) < ItemNumber(column, b);
    } else {
      less = ::CompareStringOrdinal(ItemText(column, a).c_str(), -1, ItemText(column, b).c_str(), -1,
                                    TRUE) == CSTR_LESS_THAN;
    }
    return ascending ? less : !less;
  });
}

bool MatchFilter(const Item& item) {
  if (g_filter.empty()) return true;
  return ::StrStrIW(item.name.c_str(), g_filter.c_str()) != nullptr;
}

void RebuildRows() {
  g_rows.clear();
  g_rows.reserve(g_app.items.size());
  for (int i = 0; i < static_cast<int>(g_app.items.size()); ++i) {
    if (MatchFilter(g_app.items[i])) g_rows.push_back(i);
  }
}

// 可见行下标 -> g_app.items 下标
const Item* RowItem(int row) {
  if (row < 0 || row >= static_cast<int>(g_rows.size())) return nullptr;
  const int index = g_rows[row];
  if (index < 0 || index >= static_cast<int>(g_app.items.size())) return nullptr;
  return &g_app.items[index];
}

// 行首图标：文件夹用矢量图标，文件用系统关联图标
AiUI::ListIcon ItemIcon(const Item& item) {
  AiUI::ListIcon icon;
  if (item.isDir) {
    icon.glyph = AiUI::IconId::kFolder;
    icon.color = AiUI::kColorTextFaint;
    return icon;
  }
  icon.image = AiUI::fileIcon(item.fullPath.empty() ? item.name : item.fullPath, false);
  icon.glyph = AiUI::IconId::kFile;
  icon.color = AiUI::kColorTextFaint;
  return icon;
}

void OnActivate(HWND mainWnd, int row) {
  const Item* item = RowItem(row);
  if (!item) return;

  if (g_app.archiveMode) {
    if (item->isDir) {
      g_app.innerPath =
          g_app.innerPath.empty() ? item->name : g_app.innerPath + L"\\" + item->name;
      AppRefreshView();
      return;
    }
    ShowExtractDialog(mainWnd, {g_view->rowTag(row)});
    return;
  }

  if (item->isDir) {
    AppNavigateTo(item->fullPath);
    return;
  }
  if (arc::Archive::IsArchiveFile(item->fullPath)) {
    AppOpenArchive(item->fullPath);
    return;
  }
  ::ShellExecuteW(mainWnd, L"open", item->fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnRightClick(HWND mainWnd) {
  const auto selected = GetSelectedItems();
  const bool hasSelection = !selected.empty();

  HMENU menu = ::CreatePopupMenu();
  const bool canExtract = CanExtractSelected();
  ::AppendMenuW(menu, MF_STRING | (canExtract ? 0 : MF_GRAYED), 1, L"解压到...");
  ::AppendMenuW(menu, MF_STRING, 2, L"压缩为...");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), 3, L"删除");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, 4, L"刷新");
  ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), 5, L"属性");

  POINT pt;
  ::GetCursorPos(&pt);
  // 菜单 owner 必须是前台窗口，否则 TrackPopupMenu 会立即取消
  ::SetForegroundWindow(mainWnd);
  const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                        mainWnd, nullptr);
  ::PostMessageW(mainWnd, WM_NULL, 0, 0);
  ::DestroyMenu(menu);

  switch (command) {
    case 1:
      ExtractSelection(mainWnd);
      break;
    case 2: {
      if (g_app.archiveMode) {
        ::MessageBoxW(mainWnd, L"压缩包内无法直接创建新压缩包，请先解压。", L"AIZip",
                      MB_OK | MB_ICONINFORMATION);
        break;
      }
      std::vector<std::wstring> paths;
      for (int index : selected) {
        if (index >= 0 && index < static_cast<int>(g_app.items.size())) {
          paths.push_back(g_app.items[index].fullPath);
        }
      }
      if (paths.empty()) paths.push_back(g_app.curDir);
      ShowCompressDialog(mainWnd, paths);
      break;
    }
    case 3:
      DeleteItems(mainWnd, selected);
      break;
    case 4:
      AppRefreshView();
      break;
    case 5:
      if (hasSelection && !g_app.archiveMode) {
        ::ShellExecuteW(mainWnd, L"properties", g_app.items[selected[0]].fullPath.c_str(), nullptr,
                        nullptr, SW_SHOW);
      }
      break;
    default:
      break;
  }
}

void OnListKeyDown(HWND mainWnd, AiUI::Key key) {
  switch (key) {
    case AiUI::Key::kDelete:
      DeleteItems(mainWnd, GetSelectedItems());
      break;
    case AiUI::Key::kBackspace:
      AppGoUp();
      break;
    default:
      break;
  }
}

std::vector<AiUI::ListColumn> BuildColumns() {
  std::vector<AiUI::ListColumn> out;
  out.reserve(kColCount);
  for (int i = 0; i < kColCount; ++i) {
    AiUI::ListColumn column;
    column.title = kCols[i].title;
    column.width = kCols[i].width;  // 设计值，库内按 DPI 换算
    column.align = kCols[i].rightAligned ? AiUI::ListAlign::kRight : AiUI::ListAlign::kLeft;
    column.stretch = kCols[i].stretch;
    out.push_back(column);
  }
  return out;
}

}  // namespace

void AttachFileList(AiUI::ListView* view) {
  if (!view || g_view) return;
  g_view = view;

  AiUI::ListStyle style;
  style.headHeight = AiUI::kSizeHeaderH;
  style.rowHeight = AiUI::kSizeRowH;
  style.padX = 16;
  style.iconSize = 18;
  view->setStyle(style);

  g_sortColumn = g_app.settings.listSortColumn;
  g_sortAsc = g_app.settings.listSortAsc;
  if (g_sortColumn >= kColCount) g_sortColumn = kColName;

  view->setColumns(BuildColumns());
  // 持久化的是物理像素，恢复时直接给，避免再乘一次 DPI
  for (int i = 0; i < kColCount; ++i) view->setUserWidth(i, g_app.settings.listColWidth[i]);
  view->setSort(g_sortColumn, g_sortAsc);

  HWND mainWnd = g_app.mainWnd;

  AiUI::ListCallbacks callbacks;
  callbacks.onActivate = [mainWnd](int row) { OnActivate(mainWnd, row); };
  callbacks.onSelectionChanged = [mainWnd] {
    RefreshStatusBarData();
    RefreshToolbarState();
  };
  callbacks.onSort = [](int column, bool ascending) {
    g_sortColumn = column;
    g_sortAsc = ascending;
    g_app.settings.listSortColumn = column;
    g_app.settings.listSortAsc = ascending;
    RefreshFileList();
  };
  callbacks.onContextMenu = [mainWnd] { OnRightClick(mainWnd); };
  callbacks.onKeyDown = [mainWnd](AiUI::Key key) { OnListKeyDown(mainWnd, key); };
  callbacks.onColumnResized = [] {
    for (int i = 0; i < kColCount; ++i) {
      const int width = g_view->columnWidth(i);
      if (width > 0) g_app.settings.listColWidth[i] = width;
    }
  };
  view->setCallbacks(std::move(callbacks));

  // 适配器只记数据访问方式，具体某行画什么等滚动到那里再算
  g_adapter = std::make_shared<AiUI::ListRowSource>();
  g_adapter->setCount([] { return static_cast<int>(g_rows.size()); });
  g_adapter->setBinder([](int row, AiUI::ListRow& out) {
    const Item* item = RowItem(row);
    if (!item) return;
    // tag 记住条目下标，交互时再翻回来
    out.tag = g_rows[row];
    out.icon = ItemIcon(*item);
    if (out.cells.size() < static_cast<size_t>(kColCount)) out.cells.resize(kColCount);
    for (int col = 0; col < kColCount; ++col) {
      out.cells[col].text = ItemText(col, *item);
      out.cells[col].textColor = AiUI::kNoColor;
    }
  });
  g_adapter->setTagAt([](int row) {
    if (row < 0 || row >= static_cast<int>(g_rows.size())) return -1;
    return g_rows[row];
  });
  view->setAdapter(g_adapter);

  RefreshFileList();
}

void RefreshFileList() {
  if (!g_view) return;
  SortItems(g_sortColumn, g_sortAsc);
  RebuildRows();

  // 只告诉控件行数变了，不预先生成任何一行
  g_view->setSort(g_sortColumn, g_sortAsc);
  g_view->notifyDataSetChanged();
}

void SetFilterText(const std::wstring& text) {
  if (text == g_filter) return;
  g_filter = text;
  RefreshFileList();
  RefreshStatusBarData();
}

std::vector<int> GetSelectedItems() {
  std::vector<int> out;
  if (!g_view) return out;
  // tag 就是条目在 g_app.items 里的下标，过滤后也不会错位
  for (int row : g_view->selectedRows()) {
    const int index = g_view->rowTag(row);
    if (index >= 0 && index < static_cast<int>(g_app.items.size())) out.push_back(index);
  }
  return out;
}

const std::vector<int>& VisibleRows() { return g_rows; }

// 包内模式天然可解压；文件系统下只认单个受支持的压缩包，多选或普通文件一律不可。
// 右侧没有选中项时，退回左侧树当前节点判断。
bool CanExtractSelected() {
  const auto selected = GetSelectedItems();
  if (!selected.empty()) {
    if (g_app.archiveMode) return true;
    if (selected.size() != 1) return false;
    const int index = selected[0];
    if (index < 0 || index >= static_cast<int>(g_app.items.size())) return false;
    const Item& item = g_app.items[index];
    return !item.isDir && arc::Archive::IsArchiveFile(item.fullPath);
  }
  return TreeActiveCanExtract();
}

// 解压入口：右侧有选中项就按选中项，否则用左侧树的当前节点
void ExtractSelection(HWND owner) {
  const auto selected = GetSelectedItems();
  if (!selected.empty()) {
    if (g_app.archiveMode) {
      ShowExtractDialog(owner, selected);
    } else if (selected.size() == 1) {
      const int index = selected[0];
      if (index >= 0 && index < static_cast<int>(g_app.items.size())) {
        AppExtractArchive(owner, g_app.items[index].fullPath);
      }
    }
    return;
  }

  // 右侧没选中：退到左侧树的当前节点
  if (!TreeActiveCanExtract()) return;
  const std::wstring path = TreeActivePath();
  if (!g_app.archiveMode) {
    AppExtractArchive(owner, path);
    return;
  }
  // 包内树节点按目录前缀解压；根节点（空路径）等于整包
  ShowExtractDialog(owner, {}, path);
}

void DeleteItems(HWND mainWnd, const std::vector<int>& indices) {
  if (g_app.archiveMode) {
    ::MessageBoxW(mainWnd, L"压缩包内的条目不支持直接删除。\n请解压后重新打包。", L"AIZip",
                  MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (indices.empty()) {
    ::MessageBoxW(mainWnd, L"请先选择要删除的文件或文件夹。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }

  const std::wstring message =
      L"确定把选中的 " + std::to_wstring(indices.size()) + L" 项移到回收站吗？";
  if (::MessageBoxW(mainWnd, message.c_str(), L"确认删除",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  std::wstring list;
  for (int index : indices) {
    if (index < 0 || index >= static_cast<int>(g_app.items.size())) continue;
    list += g_app.items[index].fullPath;
    list.push_back(L'\0');
  }
  list.push_back(L'\0');

  SHFILEOPSTRUCTW op{};
  op.hwnd = mainWnd;
  op.wFunc = FO_DELETE;
  op.pFrom = list.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  if (::SHFileOperationW(&op) != 0) {
    ::MessageBoxW(mainWnd, L"部分项目删除失败。", L"AIZip", MB_OK | MB_ICONWARNING);
  }
  AppRefreshView();
}

}  // namespace ui
