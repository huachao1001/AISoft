// 文件列表：虚拟 ListView（LVS_OWNERDATA），列与 HaoZip 对齐
#include "ui_list.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>

namespace ui {
namespace {

HWND g_list = nullptr;
int g_sortColumn = 0;
bool g_sortAsc = true;

// 过滤后的可见行：值为 g_app.items 下标
std::vector<int> g_rows;
std::wstring g_filter;

// 当前实际列顺序：子项下标 -> 逻辑列
std::vector<int> g_colMap;

enum Col {
  kColName = 0,
  kColSize,
  kColPacked,
  kColType,
  kColSafety,   // 安全（加密状态）
  kColModified,
  kColCrc,
  kColMethod,
  kColPath,
  kColCount
};

struct ColDef {
  const wchar_t* title;
  int width;
  int format;
  bool archiveOnly;  // 仅压缩包模式显示
};

// 列集合对齐 HaoZip：文件浏览 4 列，压缩包内 9 列
const ColDef kCols[kColCount] = {
    {L"名称", 300, LVCFMT_LEFT, false},
    {L"大小", 92, LVCFMT_RIGHT, false},
    {L"压缩后大小", 100, LVCFMT_RIGHT, true},
    {L"类型", 104, LVCFMT_LEFT, false},
    {L"安全", 64, LVCFMT_LEFT, true},
    {L"修改时间", 138, LVCFMT_LEFT, false},
    {L"CRC32", 82, LVCFMT_LEFT, true},
    {L"压缩算法", 90, LVCFMT_LEFT, true},
    {L"路径", 150, LVCFMT_LEFT, true},
};

std::wstring ItemText(int column, const Item& it) {
  switch (column) {
    case kColSize:
      return it.isDir ? L"" : util::FormatSize(it.size);
    case kColPacked:
      return it.isDir ? L"" : util::FormatSize(it.packSize);
    case kColType:
      return it.typeName;
    case kColSafety:
      return it.encrypted ? L"已加密" : L"";
    case kColModified:
      return (it.mtime.dwLowDateTime || it.mtime.dwHighDateTime) ? util::FormatTime(it.mtime) : L"";
    case kColCrc:
      return util::FormatCrc(it.crc, it.hasCrc);
    case kColMethod:
      return it.method;
    case kColPath:
      if (g_app.archiveMode) {
        // 压缩包内显示成员所在目录
        const size_t pos = it.name.find_last_of(L'\\');
        return pos == std::wstring::npos ? (g_app.innerPath.empty() ? L"" : g_app.innerPath)
                                         : (g_app.innerPath.empty() ? it.name.substr(0, pos)
                                                                    : g_app.innerPath + L"\\" +
                                                                          it.name.substr(0, pos));
      }
      return util::DirName(it.fullPath);
    default:
      return it.name;
  }
}

uint64_t ItemNumber(int column, const Item& it) {
  switch (column) {
    case kColSize:
      return it.size;
    case kColPacked:
      return it.packSize;
    case kColSafety:
      return it.encrypted ? 1 : 0;
    case kColModified:
      return (static_cast<uint64_t>(it.mtime.dwHighDateTime) << 32) | it.mtime.dwLowDateTime;
    default:
      return 0;
  }
}

bool SortItems(int col, bool asc) {
  const bool numeric =
      (col == kColSize || col == kColPacked || col == kColSafety || col == kColModified);
  std::stable_sort(g_app.items.begin(), g_app.items.end(), [&](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;  // 文件夹恒在前
    bool less;
    if (numeric) {
      less = ItemNumber(col, a) < ItemNumber(col, b);
    } else {
      less = ::CompareStringOrdinal(ItemText(col, a).c_str(), -1, ItemText(col, b).c_str(), -1, TRUE) ==
             CSTR_LESS_THAN;
    }
    return asc ? less : !less;
  });
  return true;
}

bool MatchFilter(const Item& it) {
  if (g_filter.empty()) return true;
  // 简单通配：包含子串即可
  return ::StrStrIW(it.name.c_str(), g_filter.c_str()) != nullptr;
}

void RebuildRows() {
  g_rows.clear();
  g_rows.reserve(g_app.items.size());
  for (int i = 0; i < static_cast<int>(g_app.items.size()); ++i) {
    if (MatchFilter(g_app.items[i])) g_rows.push_back(i);
  }
}

// 用户手动拖过的列宽（0 = 用设计值）。必须是唯一真源：
// LayoutFileList 会为了合身而临时压缩列宽，那些值绝不能写回这里，
// 否则一次窄窗口就会把所有列永久压扁。
int g_colWidth[kColCount] = {0};

// 应用当前模式需要的列集合
void ApplyColumns() {
  if (!g_list) return;
  const bool wantArchiveCols = g_app.archiveMode;

  ::SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
  while (::SendMessageW(g_list, LVM_GETHEADER, 0, 0) &&
         ::SendMessageW(g_list, LVM_DELETECOLUMN, 0, 0)) {
  }

  int sub = 0;
  g_colMap.clear();
  for (int i = 0; i < kColCount; ++i) {
    if (kCols[i].archiveOnly && !wantArchiveCols) continue;
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(kCols[i].title);
    col.cx = g_colWidth[i] > 0 ? g_colWidth[i] : g_app.px(kCols[i].width);
    col.fmt = kCols[i].format;
    col.iSubItem = sub;
    ::SendMessageW(g_list, LVM_INSERTCOLUMNW, sub, reinterpret_cast<LPARAM>(&col));
    g_colMap.push_back(i);
    ++sub;
  }
}

void UpdateSortIndicator() {
  HWND header = reinterpret_cast<HWND>(::SendMessageW(g_list, LVM_GETHEADER, 0, 0));
  if (!header) return;
  // 必须先读出原有 fmt 再只改 HDF_SORTUP/HDF_SORTDOWN 位，
  // 否则写入 HDF_LEFT(0) 会清掉 HDF_STRING，表头文字就不再绘制。
  for (int sub = 0; sub < static_cast<int>(g_colMap.size()); ++sub) {
    HDITEMW item{};
    item.mask = HDI_FORMAT;
    if (!::SendMessageW(header, HDM_GETITEMW, sub, reinterpret_cast<LPARAM>(&item))) continue;
    item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
    if (g_colMap[sub] == g_sortColumn) item.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
    ::SendMessageW(header, HDM_SETITEMW, sub, reinterpret_cast<LPARAM>(&item));
  }
}

void OnDoubleClick(HWND mainWnd) {
  const auto sel = GetSelectedItems();
  if (sel.empty()) return;
  const Item& it = g_app.items[sel[0]];

  if (g_app.archiveMode) {
    if (it.isDir) {
      g_app.innerPath = g_app.innerPath.empty() ? it.name : g_app.innerPath + L"\\" + it.name;
      AppRefreshView();
      return;
    }
    ShowExtractDialog(mainWnd, sel);
    return;
  }

  if (it.isDir) {
    AppNavigateTo(it.fullPath);
    return;
  }
  if (arc::Archive::IsArchiveFile(it.fullPath)) {
    AppOpenArchive(it.fullPath);
    return;
  }
  ::ShellExecuteW(mainWnd, L"open", it.fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnKeyDown(HWND mainWnd, NMLVKEYDOWN* key) {
  switch (key->wVKey) {
    case VK_RETURN:
      OnDoubleClick(mainWnd);
      break;
    case VK_DELETE:
      DeleteItems(mainWnd, GetSelectedItems());
      break;
    case VK_BACK:
      AppGoUp();
      break;
    case 'A':
      if (::GetKeyState(VK_CONTROL) < 0) {
        ::SendMessageW(g_list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), 0);
      }
      break;
    default:
      break;
  }
}

void OnRightClick(HWND mainWnd) {
  const auto sel = GetSelectedItems();
  const bool hasSel = !sel.empty();

  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 1, g_app.archiveMode
                                                                  ? L"解压到..."
                                                                  : L"解压到...");
  ::AppendMenuW(menu, MF_STRING, 2, L"压缩为...");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 3, L"删除");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, 4, L"刷新");
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 5,
                g_app.archiveMode ? L"属性" : L"属性");

  POINT pt;
  ::GetCursorPos(&pt);
  const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, mainWnd,
                                    nullptr);
  ::DestroyMenu(menu);

  switch (cmd) {
    case 1:
      ShowExtractDialog(mainWnd, sel);
      break;
    case 2: {
      std::vector<std::wstring> paths;
      if (g_app.archiveMode) {
        // 压缩包内不能直接再压缩，提示先解压
        ::MessageBoxW(mainWnd, L"压缩包内无法直接创建新压缩包，请先解压。", L"AIZip",
                      MB_OK | MB_ICONINFORMATION);
        break;
      }
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

void OnGetDispInfo(NMLVDISPINFOW* di) {
  const int row = di->item.iItem;
  if (row < 0 || row >= static_cast<int>(g_rows.size())) return;
  const int idx = g_rows[row];
  if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) return;
  const Item& it = g_app.items[idx];

  if (di->item.mask & LVIF_IMAGE) {
    di->item.iImage = it.iconIndex;
  }
  if (!(di->item.mask & LVIF_TEXT)) return;

  // 子项下标映射回逻辑列（压缩包模式列更多）
  const int sub = di->item.iSubItem;
  if (sub < 0 || sub >= static_cast<int>(g_colMap.size())) return;
  const std::wstring text = ItemText(g_colMap[sub], it);
  wcscpy_s(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), text.c_str());
}

}  // namespace

std::vector<int> GetSelectedItems() {
  std::vector<int> out;
  if (!g_list) return out;
  int row = -1;
  while ((row = static_cast<int>(::SendMessageW(g_list, LVM_GETNEXTITEM,
                                                 static_cast<WPARAM>(row), LVNI_SELECTED))) != -1) {
    if (row >= 0 && row < static_cast<int>(g_rows.size())) out.push_back(g_rows[row]);
  }
  return out;
}

const std::vector<int>& VisibleRows() { return g_rows; }

// 给列表的表头挂上子类化窗口过程（只挂一次）
void HookListHeader();

HWND CreateFileList(HWND parent, HINSTANCE inst, int id) {
  HWND list = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 0, 0, 0,
      0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);

  ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

  g_list = list;
  g_sortColumn = g_app.settings.listSortColumn;
  g_sortAsc = g_app.settings.listSortAsc;

  ApplyColumns();
  ::SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL,
                 reinterpret_cast<LPARAM>(ShellImageList()));
  HookListHeader();
  return list;
}

void LayoutFileList(HWND list) {
  if (!list) return;
  RECT rc;
  ::GetClientRect(list, &rc);
  if (rc.right <= 0) return;  // 窗口尚未布局完成

  const int count = static_cast<int>(g_colMap.size());
  if (count == 0) return;

  const int avail = rc.right - g_app.px(4);
  const int minName = g_app.px(180);

  // 一切宽度都从「设计值（或用户拖拽值）」重算，绝不读取当前实际宽度：
  // 窗口变窄时列会被临时压扁，若读回这些挤扁的值，放大后也无法恢复。
  std::vector<int> w(count, 0);
  int sum = 0;
  for (int sub = 1; sub < count; ++sub) {
    const int logical = g_colMap[sub];
    w[sub] = g_colWidth[logical] > 0 ? g_colWidth[logical] : g_app.px(kCols[logical].width);
    sum += w[sub];
  }

  const int budget = avail - minName;
  if (sum > budget) {
    // 放不下：轮流削减当前最宽的列，直到刚好合身。
    // 等比缩会让所有表头都变成省略号，轮流削能保住窄列的可读性。
    const int minCol = g_app.px(52);
    while (sum > budget) {
      int widest = -1;
      for (int sub = 1; sub < count; ++sub) {
        if (w[sub] <= minCol) continue;
        if (widest < 0 || w[sub] > w[widest]) widest = sub;
      }
      if (widest < 0) break;
      const int cut = (sum - budget) < g_app.px(8) ? (sum - budget) : g_app.px(8);
      w[widest] -= cut;
      sum -= cut;
    }
  }

  int used = 0;
  for (int sub = 1; sub < count; ++sub) {
    ::SendMessageW(list, LVM_SETCOLUMNWIDTH, sub, w[sub]);
    used += w[sub];
  }
  const int nameW = avail - used;
  ::SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, nameW > minName ? nameW : minName);
}

// 用户拖动表头分隔线后记录真实宽度，作为下次重建列的依据
void SaveUserColumnWidths() {
  if (!g_list) return;
  for (int sub = 1; sub < static_cast<int>(g_colMap.size()); ++sub) {
    const int w = static_cast<int>(::SendMessageW(g_list, LVM_GETCOLUMNWIDTH, sub, 0));
    if (w > 0) g_colWidth[g_colMap[sub]] = w;
  }
}

// 表头拖动结束的通知发给表头自己，只有子类化才能收到
LRESULT CALLBACK HeaderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                            UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
  if (msg == WM_NOTIFY) {
    auto* hdr = reinterpret_cast<NMHDR*>(lParam);
    if (hdr && hdr->code == NM_RELEASEDCAPTURE) SaveUserColumnWidths();
  }
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// 给列表的表头挂上子类化窗口过程（只挂一次）
void HookListHeader() {
  if (!g_list) return;
  HWND header = reinterpret_cast<HWND>(::SendMessageW(g_list, LVM_GETHEADER, 0, 0));
  if (!header) return;
  // 重复挂载会被系统拒绝，用返回值去重
  ::SetWindowSubclass(header, HeaderProc, 1, 0);
}

void RefreshFileList() {
  if (!g_list) return;

  ApplyColumns();
  SortItems(g_sortColumn, g_sortAsc);
  RebuildRows();

  ::SendMessageW(g_list, LVM_SETITEMCOUNT, static_cast<WPARAM>(g_rows.size()),
                 LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  RECT rc;
  ::GetClientRect(g_list, &rc);
  ::InvalidateRect(g_list, &rc, TRUE);
  LayoutFileList(g_list);
  UpdateSortIndicator();
}

void SetFilterFromEdit(HWND edit) {
  wchar_t buf[256];
  ::GetWindowTextW(edit, buf, 256);
  const std::wstring next = buf;
  if (next == g_filter) return;
  g_filter = next;
  RefreshFileList();
  RefreshStatusBar(g_app.mainWnd);
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

  const std::wstring msg = L"确定把选中的 " + std::to_wstring(indices.size()) +
                           L" 项移到回收站吗？";
  if (::MessageBoxW(mainWnd, msg.c_str(), L"确认删除",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  // 汇总成一次 FO_DELETE，双 null 结尾的多路径列表
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

LRESULT HandleListNotify(HWND mainWnd, NMHDR* hdr) {
  switch (hdr->code) {
    case LVN_GETDISPINFOW:
      OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(hdr));
      return 0;

    case LVN_COLUMNCLICK: {
      auto* nv = reinterpret_cast<NMLISTVIEW*>(hdr);
      const int sub = nv->iSubItem;
      if (sub < 0 || sub >= static_cast<int>(g_colMap.size())) return 0;
      const int col = g_colMap[sub];
      const bool asc = (col == g_sortColumn) ? !g_sortAsc : true;
      g_sortColumn = col;
      g_sortAsc = asc;
      g_app.settings.listSortColumn = col;
      g_app.settings.listSortAsc = asc;
      RefreshFileList();
      return 0;
    }

    case NM_DBLCLK:
      OnDoubleClick(mainWnd);
      return 0;

    case LVN_KEYDOWN:
      OnKeyDown(mainWnd, reinterpret_cast<NMLVKEYDOWN*>(hdr));
      return 0;

    case NM_RCLICK:
      OnRightClick(mainWnd);
      return 0;

    case LVN_ITEMCHANGED:
      RefreshStatusBar(mainWnd);
      return 0;

    case NM_CUSTOMDRAW: {
      auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
      if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const int row = static_cast<int>(cd->nmcd.dwItemSpec);
        if (row >= 0 && row < static_cast<int>(g_rows.size())) {
          const int idx = g_rows[row];
          if (idx >= 0 && idx < static_cast<int>(g_app.items.size()) && g_app.items[idx].encrypted) {
            cd->clrText = RGB(160, 110, 0);
          }
        }
      }
      return CDRF_DODEFAULT;
    }

    default:
      break;
  }
  return 0;
}

}  // namespace ui
