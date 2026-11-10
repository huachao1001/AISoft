// 文件列表：数据与交互编排，绘制/选择/滚动由 AUI::ListView 负责
#include "ui_list.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_main.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "ui_theme.h"
#include "util.h"
#include "AUI/list_view.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <memory>

namespace ui {
namespace {

std::unique_ptr<AUI::ListView> g_view;
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

std::wstring ItemText(int column, const Item& it) {
  switch (column) {
    case kColType:
      return it.typeName;
    case kColSize:
      // 文件夹不统计大小，留空比画横线干净
      return it.isDir ? L"" : util::FormatSize(it.size);
    case kColModified:
      return (it.mtime.dwLowDateTime || it.mtime.dwHighDateTime) ? util::FormatTime(it.mtime) : L"";
    default:
      return it.name;
  }
}

uint64_t ItemNumber(int column, const Item& it) {
  switch (column) {
    case kColSize:
      return it.size;
    case kColModified:
      return (static_cast<uint64_t>(it.mtime.dwHighDateTime) << 32) | it.mtime.dwLowDateTime;
    default:
      return 0;
  }
}

void SortItems(int col, bool asc) {
  const bool numeric = (col == kColSize || col == kColModified);
  std::stable_sort(g_app.items.begin(), g_app.items.end(), [&](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;  // 文件夹恒在前
    bool less;
    if (numeric) {
      less = ItemNumber(col, a) < ItemNumber(col, b);
    } else {
      less = ::CompareStringOrdinal(ItemText(col, a).c_str(), -1, ItemText(col, b).c_str(), -1,
                                    TRUE) == CSTR_LESS_THAN;
    }
    return asc ? less : !less;
  });
}

bool MatchFilter(const Item& it) {
  if (g_filter.empty()) return true;
  return ::StrStrIW(it.name.c_str(), g_filter.c_str()) != nullptr;
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
  const int idx = g_rows[row];
  if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) return nullptr;
  return &g_app.items[idx];
}

// 行首图标：文件夹用系统图标，文件用系统关联图标
void DrawFileGlyph(AUI::Painter& p, const Item& it, const RECT& rc) {
  if (it.isDir) {
    p.Icon(AUI::IconId::kFolder, rc, RGB(0x9a, 0xa3, 0xaf));
    return;
  }
  HICON icon = AUI::ShellIconFor(it.fullPath.empty() ? it.name : it.fullPath, false);
  if (icon) {
    p.BitmapIcon(icon, rc);
    return;
  }
  p.Icon(AUI::IconId::kFile, rc, RGB(0x9a, 0xa3, 0xaf));
}

void OnActivate(HWND mainWnd, int row) {
  const Item* it = RowItem(row);
  if (!it) return;

  if (g_app.archiveMode) {
    if (it->isDir) {
      g_app.innerPath = g_app.innerPath.empty() ? it->name : g_app.innerPath + L"\\" + it->name;
      AppRefreshView();
      return;
    }
    const int idx = g_rows[row];
    ShowExtractDialog(mainWnd, {idx});
    return;
  }

  if (it->isDir) {
    AppNavigateTo(it->fullPath);
    return;
  }
  if (arc::Archive::IsArchiveFile(it->fullPath)) {
    AppOpenArchive(it->fullPath);
    return;
  }
  ::ShellExecuteW(mainWnd, L"open", it->fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnRightClick(HWND mainWnd) {
  const auto sel = GetSelectedItems();
  const bool hasSel = !sel.empty();

  HMENU menu = ::CreatePopupMenu();
  const bool canExtract = CanExtractSelected();
  ::AppendMenuW(menu, MF_STRING | (canExtract ? 0 : MF_GRAYED), 1, L"解压到...");
  ::AppendMenuW(menu, MF_STRING, 2, L"压缩为...");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 3, L"删除");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, 4, L"刷新");
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 5, L"属性");

  POINT pt;
  ::GetCursorPos(&pt);
  const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, mainWnd,
                                    nullptr);
  ::DestroyMenu(menu);

  switch (cmd) {
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
      for (int idx : sel) {
        if (idx >= 0 && idx < static_cast<int>(g_app.items.size()))
          paths.push_back(g_app.items[idx].fullPath);
      }
      if (paths.empty()) paths.push_back(g_app.curDir);
      ShowCompressDialog(mainWnd, paths);
      break;
    }
    case 3:
      DeleteItems(mainWnd, sel);
      break;
    case 4:
      AppRefreshView();
      break;
    case 5:
      if (hasSel && !g_app.archiveMode) {
        ::ShellExecuteW(mainWnd, L"properties", g_app.items[sel[0]].fullPath.c_str(), nullptr,
                        nullptr, SW_SHOW);
      }
      break;
    default:
      break;
  }
}

void OnListKeyDown(HWND mainWnd, int vk) {
  switch (vk) {
    case VK_DELETE:
      DeleteItems(mainWnd, GetSelectedItems());
      break;
    case VK_BACK:
      AppGoUp();
      break;
    default:
      break;
  }
}

std::vector<AUI::ListColumn> BuildColumns() {
  std::vector<AUI::ListColumn> out;
  out.reserve(kColCount);
  for (int i = 0; i < kColCount; ++i) {
    AUI::ListColumn c;
    c.title = kCols[i].title;
    c.width = kCols[i].width;  // 设计值，AUI 内部乘 DPI
    c.align = kCols[i].rightAligned ? AUI::ListAlign::kRight : AUI::ListAlign::kLeft;
    c.stretch = kCols[i].stretch;
    out.push_back(c);
  }
  return out;
}

}  // namespace

HWND CreateFileList(HWND parent, HINSTANCE inst, int id) {
  g_view = std::make_unique<AUI::ListView>();

  AUI::ListStyle style;
  style.headH = kHeadH;  // 设计值，AUI 内部按 DPI 换算
  style.rowH = kRowH;
  style.padX = 16;
  style.iconSize = 18;
  g_view->SetStyle(style);

  g_sortColumn = g_app.settings.listSortColumn;
  g_sortAsc = g_app.settings.listSortAsc;
  if (g_sortColumn >= kColCount) g_sortColumn = kColName;

  g_view->SetColumns(BuildColumns());
  // 持久化的是物理像素，恢复时直接给，避免再乘一次 DPI
  for (int i = 0; i < kColCount; ++i) g_view->SetUserWidth(i, g_app.settings.listColWidth[i]);
  g_view->SetSort(g_sortColumn, g_sortAsc);

  HWND mainWnd = g_app.mainWnd;

  AUI::ListCallbacks cb;
  cb.rowCount = [] { return static_cast<int>(g_rows.size()); };
  cb.text = [](int row, int column) -> std::wstring {
    const Item* it = RowItem(row);
    return it ? ItemText(column, *it) : std::wstring();
  };
  cb.drawIcon = [](AUI::Painter& p, int row, const RECT& rc) {
    if (const Item* it = RowItem(row)) DrawFileGlyph(p, *it, rc);
  };
  cb.onActivate = [mainWnd](int row) { OnActivate(mainWnd, row); };
  cb.onSelectionChanged = [mainWnd] {
    RefreshStatusBar(mainWnd);
    RefreshToolbarState();
  };
  cb.onSort = [](int column, bool asc) {
    g_sortColumn = column;
    g_sortAsc = asc;
    g_app.settings.listSortColumn = column;
    g_app.settings.listSortAsc = asc;
    RefreshFileList();
  };
  cb.onContextMenu = [mainWnd] { OnRightClick(mainWnd); };
  cb.onKeyDown = [mainWnd](int vk) { OnListKeyDown(mainWnd, vk); };
  cb.onColumnResized = [] {
    for (int i = 0; i < kColCount; ++i) {
      const int w = g_view->ColumnWidth(i);
      if (w > 0) g_app.settings.listColWidth[i] = w;
    }
  };
  g_view->SetCallbacks(std::move(cb));

  return g_view->Create(parent, inst, id, L"AIZipFileList",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL);
}

void LayoutFileList(HWND) {
  if (!g_view || !g_view->hwnd()) return;
  g_view->LayoutColumns();
  g_view->Invalidate();
}

void RefreshFileList() {
  if (!g_view) return;
  SortItems(g_sortColumn, g_sortAsc);
  RebuildRows();
  g_view->SetSort(g_sortColumn, g_sortAsc);
  g_view->Reload();
  // 内容整体换了（换目录/改过滤/重排），按行号保留选中会落到别的条目上
  g_view->ClearSelection();
}

void SetFilterText(const std::wstring& text) {
  if (text == g_filter) return;
  g_filter = text;
  RefreshFileList();
  RefreshStatusBar(g_app.mainWnd);
}

std::vector<int> GetSelectedItems() {
  std::vector<int> out;
  if (!g_view) return out;
  for (int row : g_view->SelectedRows()) {
    if (row >= 0 && row < static_cast<int>(g_rows.size())) out.push_back(g_rows[row]);
  }
  return out;
}

const std::vector<int>& VisibleRows() { return g_rows; }

// 包内模式天然可解压；文件系统下只认单个受支持的压缩包，多选或普通文件一律不可。
// 右侧没有选中项时，退回左侧树当前节点判断。
bool CanExtractSelected() {
  const auto sel = GetSelectedItems();
  if (!sel.empty()) {
    if (g_app.archiveMode) return true;
    if (sel.size() != 1) return false;
    const int idx = sel[0];
    if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) return false;
    const Item& it = g_app.items[idx];
    return !it.isDir && arc::Archive::IsArchiveFile(it.fullPath);
  }
  // 右侧没选中时退回左侧树：树上的目标同样可以解压
  return TreeActiveCanExtract();
}

// 解压入口：右侧有选中项就按选中项，否则用左侧树的当前节点
void ExtractSelection(HWND owner) {
  const auto sel = GetSelectedItems();
  if (!sel.empty()) {
    if (g_app.archiveMode) {
      ShowExtractDialog(owner, sel);
    } else if (sel.size() == 1) {
      const int idx = sel[0];
      if (idx >= 0 && idx < static_cast<int>(g_app.items.size())) {
        AppExtractArchive(owner, g_app.items[idx].fullPath);
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

  const std::wstring msg = L"确定把选中的 " + std::to_wstring(indices.size()) + L" 项移到回收站吗？";
  if (::MessageBoxW(mainWnd, msg.c_str(), L"确认删除",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  std::wstring list;
  for (int idx : indices) {
    if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) continue;
    list += g_app.items[idx].fullPath;
    list.push_back(L'\0');
  }
  list.push_back(L'\0');

  SHFILEOPSTRUCTW op{};
  op.hwnd = mainWnd;
  op.wFunc = FO_DELETE;
  op.pFrom = list.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int r = ::SHFileOperationW(&op);
  if (r != 0) {
    ::MessageBoxW(mainWnd, L"部分项目删除失败。", L"AIZip", MB_OK | MB_ICONWARNING);
  }
  AppRefreshView();
}

}  // namespace ui
