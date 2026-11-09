// 文件列表：虚拟 ListView，列与行样式对齐 ui.html
#include "ui_list.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_main.h"
#include "ui_toolbar.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>

namespace ui {
namespace {

HWND g_list = nullptr;
int g_sortColumn = 0;
bool g_sortAsc = true;
int g_hotRow = -1;

// 过滤后的可见行：值为 g_app.items 下标
std::vector<int> g_rows;
std::wstring g_filter;

// 当前实际列顺序：子项下标 -> 逻辑列
std::vector<int> g_colMap;

enum Col { kColName = 0, kColType, kColSize, kColModified, kColCount };

struct ColDef {
  const wchar_t* title;
  int width;
  int format;
  bool stretch;  // 名称列吃掉剩余宽度
};

// 与 ui.html 一致：名称 / 类型 / 大小 / 修改时间
const ColDef kCols[kColCount] = {
    {L"名称", 320, LVCFMT_LEFT, true},
    {L"类型", 110, LVCFMT_LEFT, false},
    {L"大小", 100, LVCFMT_RIGHT, false},
    {L"修改时间", 160, LVCFMT_LEFT, false},
};

std::wstring ItemText(int column, const Item& it) {
  switch (column) {
    case kColType:
      return it.typeName;
    case kColSize:
      return it.isDir ? L"—" : util::FormatSize(it.size);
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

// 用户手动拖过的列宽（0 = 用设计值）
int g_colWidth[kColCount] = {0};

void ApplyColumns() {
  if (!g_list) return;
  ::SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
  while (::SendMessageW(g_list, LVM_GETHEADER, 0, 0) &&
         ::SendMessageW(g_list, LVM_DELETECOLUMN, 0, 0)) {
  }

  g_colMap.clear();
  for (int i = 0; i < kColCount; ++i) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(kCols[i].title);
    col.cx = g_colWidth[i] > 0 ? g_colWidth[i] : g_app.px(kCols[i].width);
    col.fmt = kCols[i].format;
    col.iSubItem = i;
    ::SendMessageW(g_list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&col));
    g_colMap.push_back(i);
  }
}

HWND Header() { return reinterpret_cast<HWND>(::SendMessageW(g_list, LVM_GETHEADER, 0, 0)); }

// 后面才定义，这里先声明
void SaveUserColumnWidths();

int ClientRight() {
  RECT rc;
  ::GetClientRect(g_list, &rc);
  return rc.right;
}

// 行首图标：文件夹用线描图标（与 ui.html 一致），文件用系统关联图标
void DrawFileGlyph(HDC dc, const Item& it, const RECT& rc) {
  if (it.isDir) {
    DrawIcon(dc, IconId::kFolder, rc, RGB(0x9a, 0xa3, 0xaf));
    return;
  }
  HICON icon = ShellIconFor(it.fullPath.empty() ? it.name : it.fullPath, false);
  if (icon) {
    ::DrawIconEx(dc, rc.left, rc.top, icon, rc.right - rc.left, rc.bottom - rc.top, 0, nullptr,
                 DI_NORMAL);
    return;
  }
  DrawIcon(dc, IconId::kFile, rc, RGB(0x9a, 0xa3, 0xaf));
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
    case VK_RETURN: OnDoubleClick(mainWnd); break;
    case VK_DELETE: DeleteItems(mainWnd, GetSelectedItems()); break;
    case VK_BACK: AppGoUp(); break;
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
  ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), 1, L"解压到...");
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
      ShowExtractDialog(mainWnd, sel);
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

void OnGetDispInfo(NMLVDISPINFOW* di) {
  const int row = di->item.iItem;
  if (row < 0 || row >= static_cast<int>(g_rows.size())) return;
  const int idx = g_rows[row];
  if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) return;
  const Item& it = g_app.items[idx];

  // 行首图标自绘，这里固定用图像列表的空格子，只为撑行高
  if (di->item.mask & LVIF_IMAGE) di->item.iImage = 0;
  if (!(di->item.mask & LVIF_TEXT)) return;

  const int sub = di->item.iSubItem;
  if (sub < 0 || sub >= static_cast<int>(g_colMap.size())) return;
  const std::wstring text = ItemText(g_colMap[sub], it);
  wcscpy_s(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), text.c_str());
}

// 用户拖动表头分隔线后记录真实宽度，作为下次重建列的依据
void SaveUserColumnWidths() {
  if (!g_list) return;
  for (int sub = 1; sub < static_cast<int>(g_colMap.size()); ++sub) {
    const int w = static_cast<int>(::SendMessageW(g_list, LVM_GETCOLUMNWIDTH, sub, 0));
    if (w > 0) g_colWidth[g_colMap[sub]] = w;
  }
}

// 表头自绘：浅灰底 + 底部细线 + 排序列箭头。
// 表头的 NM_CUSTOMDRAW 不保证会转发到 ListView 的父窗口，直接子类化自己画。
void PaintHeader(HWND header) {
  RECT rc;
  ::GetClientRect(header, &rc);
  HDC target = ::GetDC(header);
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kThBg);

  const int count = static_cast<int>(g_colMap.size());
  for (int col = 0; col < count; ++col) {
    RECT cell{};
    cell.left = col;
    if (!::SendMessageW(header, HDM_GETITEMRECT, col, reinterpret_cast<LPARAM>(&cell))) continue;

    const bool isSort = (g_colMap[col] == g_sortColumn);
    RECT tr = cell;
    const int padX = g_app.px(16);
    tr.left += padX;
    tr.right -= padX;
    if (isSort) tr.right -= g_app.px(18);

    wchar_t buf[128] = {};
    HDITEMW hi{};
    hi.mask = HDI_TEXT;
    hi.pszText = buf;
    hi.cchTextMax = 128;
    ::SendMessageW(header, HDM_GETITEMW, col, reinterpret_cast<LPARAM>(&hi));

    const bool rightAligned = kCols[g_colMap[col]].format == LVCFMT_RIGHT;
    HFONT old = static_cast<HFONT>(::SelectObject(dc, fonts.ui));
    ::SetTextColor(dc, kTextHead);
    ::DrawTextW(dc, buf, -1, &tr,
                (rightAligned ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, old);

    if (isSort) {
      const int cx = cell.right - g_app.px(18);
      const int cy = (cell.top + cell.bottom) / 2;
      const int half = g_app.px(4);
      POINT pts[3];
      if (g_sortAsc) {
        pts[0] = POINT{cx - half, cy + half / 2};
        pts[1] = POINT{cx + half, cy + half / 2};
        pts[2] = POINT{cx, cy - half / 2};
      } else {
        pts[0] = POINT{cx - half, cy - half / 2};
        pts[1] = POINT{cx + half, cy - half / 2};
        pts[2] = POINT{cx, cy + half / 2};
      }
      HBRUSH b = ::CreateSolidBrush(kTextMuted);
      HPEN p = ::CreatePen(PS_SOLID, 1, kTextMuted);
      HBRUSH ob = static_cast<HBRUSH>(::SelectObject(dc, b));
      HPEN op = static_cast<HPEN>(::SelectObject(dc, p));
      ::Polygon(dc, pts, 3);
      ::SelectObject(dc, ob);
      ::SelectObject(dc, op);
      ::DeleteObject(b);
      ::DeleteObject(p);
    }

    // ui.html 的表头没有列分隔线，这里只保留底部一条细线
  }

  DrawHLine(dc, 0, rc.right, rc.bottom - 1, kLineHead);
  mem.Commit();
  ::ReleaseDC(header, target);
}

LRESULT CALLBACK HeaderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*id*/,
                            DWORD_PTR /*ref*/) {
  switch (msg) {
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kThBg);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintHeader(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NOTIFY: {
      // 拖动列宽结束后记录真实宽度
      auto* hdr = reinterpret_cast<NMHDR*>(lParam);
      if (hdr && hdr->code == NM_RELEASEDCAPTURE) SaveUserColumnWidths();
      break;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, HeaderProc, 1);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void HookListHeader() {
  if (!g_list) return;
  HWND header = Header();
  if (!header) return;
  // 重复挂载会被系统拒绝，忽略返回值即可
  ::SetWindowSubclass(header, HeaderProc, 1, 0);
}

// 整行绘制：一次画完底色 + 图标 + 文字，坐标是列表客户区绝对坐标
void DrawRow(HDC dc, int row, bool selected, bool hot, const RECT& rowRc) {
  const Item& it = g_app.items[g_rows[row]];

  FillRect(dc, rowRc, selected ? kSelBg : (hot ? kRowHover : kPaneBg));

  for (int sub = 0; sub < static_cast<int>(g_colMap.size()); ++sub) {
    RECT cell{};
    cell.left = LVIR_BOUNDS;
    cell.top = sub;
    if (!::SendMessageW(g_list, LVM_GETSUBITEMRECT, static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&cell)))
      continue;

    RECT tr = cell;
    // 子项矩形和整行矩形在虚拟列表里不一定对齐，统一裁到行内
    tr.top = rowRc.top;
    tr.bottom = rowRc.bottom;
    const int padX = g_app.px(16);
    tr.left += padX;
    tr.right -= padX;

    if (sub == 0) {
      // 名称列：图标 + 文本。文件夹用自绘图标，文件用系统图标
      const int iconSide = g_app.px(18);
      const int iconTop = (tr.top + tr.bottom - iconSide) / 2;
      RECT ir{cell.left + padX, iconTop, cell.left + padX + iconSide, iconTop + iconSide};
      DrawFileGlyph(dc, it, ir);
      tr.left = ir.right + g_app.px(8);
      DrawTextEllipsis(dc, tr, it.name, kText, fonts.ui);
    } else {
      const bool rightAligned = kCols[g_colMap[sub]].format == LVCFMT_RIGHT;
      HFONT old = static_cast<HFONT>(::SelectObject(dc, fonts.ui));
      ::SetBkMode(dc, TRANSPARENT);
      ::SetTextColor(dc, kText);
      const std::wstring text = ItemText(g_colMap[sub], it);
      ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &tr,
                  (rightAligned ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
      ::SelectObject(dc, old);
    }
  }

  // 行底部分隔线
  DrawHLine(dc, rowRc.left, rowRc.right, rowRc.bottom - 1, kLineSoft);
}

// 行自绘：白底 / 悬停浅灰 / 选中淡蓝，底部一条极浅分隔线
LRESULT RowCustomDraw(LPNMLVCUSTOMDRAW cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT: {
      const int row = static_cast<int>(cd->nmcd.dwItemSpec);
      if (row < 0 || row >= static_cast<int>(g_rows.size())) return CDRF_DODEFAULT;
      const int idx = g_rows[row];
      if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) return CDRF_DODEFAULT;

      // nmcd.uItemState 在这个阶段不一定已同步，直接问列表要选中状态
      const bool selected =
          (::SendMessageW(g_list, LVM_GETITEMSTATE, static_cast<WPARAM>(row), LVIS_SELECTED) &
           LVIS_SELECTED) != 0;
      const bool hot = (row == g_hotRow) && !selected;

      // LVS_OWNERDATA 下 LVM_GETITEMRECT 不可靠，用子项 0 的矩形当整行边界
      RECT rowRc{};
      rowRc.left = LVIR_BOUNDS;
      rowRc.top = 0;
      if (!::SendMessageW(g_list, LVM_GETSUBITEMRECT, static_cast<WPARAM>(row),
                          reinterpret_cast<LPARAM>(&rowRc)))
        return CDRF_DODEFAULT;
      rowRc.left = 0;
      rowRc.right = ClientRight();

      // 先画进离屏位图再整体贴回：直接画会把“刷底色”和“画文字”分成两次提交，
      // 悬停换行时桌面会短暂看到只有底色、没有文字的一帧。
      MemDC mem(cd->nmcd.hdc, rowRc);
      DrawRow(mem.dc(), row, selected, hot, rowRc);
      mem.Commit();
      return CDRF_SKIPDEFAULT;
    }

    default:
      return CDRF_DODEFAULT;
  }
}

}  // namespace

std::vector<int> GetSelectedItems() {
  std::vector<int> out;
  if (!g_list) return out;
  int row = -1;
  while ((row = static_cast<int>(::SendMessageW(g_list, LVM_GETNEXTITEM, static_cast<WPARAM>(row),
                                                LVNI_SELECTED))) != -1) {
    if (row >= 0 && row < static_cast<int>(g_rows.size())) out.push_back(g_rows[row]);
  }
  return out;
}

const std::vector<int>& VisibleRows() { return g_rows; }

void SetListHotRow(int row);

LRESULT CALLBACK ListHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                               DWORD_PTR ref);

HWND CreateFileList(HWND parent, HINSTANCE inst, int id) {
  HWND list = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 0, 0, 0,
      0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);

  ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  ::SendMessageW(list, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(kPaneBg));
  ::SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(kPaneBg));
  ::SendMessageW(list, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(kText));

  g_list = list;
  g_sortColumn = g_app.settings.listSortColumn;
  g_sortAsc = g_app.settings.listSortAsc;
  if (g_sortColumn >= kColCount) g_sortColumn = kColName;

  ApplyColumns();
  // 行高由图像列表单元格决定，撑到与 ui.html 一致的 44px
  ::SendMessageW(list, LVM_SETICONSPACING, 0,
                 MAKELPARAM(g_app.px(kRowH), g_app.px(kRowH)));
  ::SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL,
                 reinterpret_cast<LPARAM>(ListImageList(g_app.px(kRowH))));
  HookListHeader();
  ::SetWindowSubclass(list, ListHoverProc, 2, 0);
  return list;
}

void LayoutFileList(HWND list) {
  if (!list) return;
  RECT rc;
  ::GetClientRect(list, &rc);
  if (rc.right <= 0) return;

  const int count = static_cast<int>(g_colMap.size());
  if (count == 0) return;

  const int avail = rc.right;
  int fixed = 0;
  for (int i = 0; i < count; ++i) {
    if (kCols[g_colMap[i]].stretch) continue;
    const int w = g_colWidth[g_colMap[i]] > 0 ? g_colWidth[g_colMap[i]] : g_app.px(kCols[g_colMap[i]].width);
    ::SendMessageW(list, LVM_SETCOLUMNWIDTH, i, w);
    fixed += w;
  }
  const int nameW = std::max(g_app.px(180), avail - fixed);
  ::SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, nameW);
}

void RefreshFileList() {
  if (!g_list) return;

  ApplyColumns();
  SortItems(g_sortColumn, g_sortAsc);
  RebuildRows();

  // 列表图标按行高切片，缓存放在这里刷新
  ::SendMessageW(g_list, LVM_SETIMAGELIST, LVSIL_SMALL,
                 reinterpret_cast<LPARAM>(ListImageList(g_app.px(kRowH))));

  ::SendMessageW(g_list, LVM_SETITEMCOUNT, static_cast<WPARAM>(g_rows.size()),
                 LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  LayoutFileList(g_list);
  ::InvalidateRect(g_list, nullptr, TRUE);
  // 表头不是 g_list 的子窗口，自绘必须显式重画
  if (HWND header = Header()) ::InvalidateRect(header, nullptr, TRUE);
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

namespace {
// 重画单行时走 ListView 自己的绘制路径，避免手动失效区域
void RedrawListRow(int row) {
  if (!g_list || row < 0 || row >= static_cast<int>(g_rows.size())) return;
  ::SendMessageW(g_list, LVM_REDRAWITEMS, static_cast<WPARAM>(row), static_cast<LPARAM>(row));
}
}  // namespace

void SetListHotRow(int row) {
  if (row == g_hotRow) return;
  const int prev = g_hotRow;
  g_hotRow = row;
  RedrawListRow(prev);
  RedrawListRow(row);
}

// 悬停行：ListView 自己不会通知行悬停，用子类化跟踪鼠标
LRESULT CALLBACK ListHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                               DWORD_PTR) {
  switch (msg) {
    case WM_MOUSEMOVE: {
      LVHITTESTINFO ht{};
      ht.pt.x = GET_X_LPARAM(lParam);
      ht.pt.y = GET_Y_LPARAM(lParam);
      const int row = (::SendMessageW(hwnd, LVM_SUBITEMHITTEST, 0,
                                      reinterpret_cast<LPARAM>(&ht)) >= 0)
                          ? ht.iItem
                          : -1;
      SetListHotRow(row);
      if (row >= 0) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      break;
    }
    case WM_MOUSELEAVE:
      SetListHotRow(-1);
      break;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, ListHoverProc, id);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
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

  const std::wstring msg =
      L"确定把选中的 " + std::to_wstring(indices.size()) + L" 项移到回收站吗？";
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
      RefreshToolbarState();
      return 0;

    case NM_CUSTOMDRAW:
      return RowCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(hdr));

    default:
      break;
  }
  return 0;
}

}  // namespace ui
