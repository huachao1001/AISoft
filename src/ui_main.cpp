// 主窗口：工具栏 + 地址栏 + 树 + 列表 + 状态栏
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "sevenzip_loader.h"
#include "ui.h"
#include "ui_list.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "util.h"

// 拖放转发需要 SetWindowSubclass/DefSubclassProc（comctl32 v6），
// 这些声明要求 _WIN32_WINNT >= 0x0601，由 CMakeLists 全局定义。
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace ui {
namespace {

MainParts g_parts;
constexpr const wchar_t* kMainClass = L"AIZipMainWindow";
constexpr const wchar_t* kNavClass = L"AIZipNavBar";
constexpr int kMinWidth = 720;
constexpr int kMinHeight = 460;
int g_treeWidth = kTreeDefaultWidth;

// 地址栏内的导航按钮尺寸
constexpr int kNavBtnW = 26;
constexpr int kNavCount = 4;
int g_navHot = -1;
int g_navPressed = -1;

struct NavBtn {
  IconId icon;
  bool enabled;
};

RECT NavBtnRect(int i) {
  const int w = g_app.px(kNavBtnW);
  const int pad = g_app.px(3);
  return RECT{i * w + pad, pad, (i + 1) * w - pad, g_app.px(kAddressHeight) - pad};
}

// 导航按钮可用性：后退/前进看历史，上级看是否还有父目录
void NavStates(bool out[kNavCount]) {
  out[0] = AppCanBack();
  out[1] = AppCanForward();
  if (g_app.archiveMode) {
    out[2] = true;  // 包内可上跳，包根还可回到所在文件夹
  } else {
    const std::wstring parent = util::DirName(g_app.curDir);
    out[2] = !parent.empty() && parent != g_app.curDir;
  }
  out[3] = true;
}

void PaintNavBar(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  ui::MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, RGB(249, 252, 254));

  bool enabled[kNavCount]{};
  NavStates(enabled);
  const IconId icons[kNavCount] = {IconId::kArrowBack, IconId::kArrowForward, IconId::kArrowUp,
                                   IconId::kRefresh};

  for (int i = 0; i < kNavCount; ++i) {
    RECT br = NavBtnRect(i);
    if (i == g_navHot || i == g_navPressed) {
      FillRoundRect(dc, br, i == g_navPressed ? RGB(196, 226, 246) : RGB(226, 241, 251), 3);
    }
    // 图标区取按钮内的正方形，按高度收边，避免在窄高度下被压扁
    const int side = ((br.bottom - br.top) < (br.right - br.left)) ? (br.bottom - br.top)
                                                                   : (br.right - br.left);
    const int ix = (br.left + br.right - side) / 2;
    const int iy = (br.top + br.bottom - side) / 2;
    const int inset = side / 5;
    RECT ir{ix + inset, iy + inset, ix + side - inset, iy + side - inset};
    DrawIcon(dc, icons[i], ir, enabled[i]);
  }

  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

// ───────────────────────── 拖放转发 ─────────────────────────
//
// 主窗口带 WS_EX_ACCEPTFILES，但子窗口不会向上转发 WM_DROPFILES：
// 把文件拖到工具栏/导航条/地址栏/状态栏上都没反应，只有列表和空白区能收。
// 这里给这些区域各自注册拖放，统一转交主窗口，使整块窗口都是投放目标。

// 取主窗口：拖放目标都是主窗口的直接子窗口
HWND DropOwner(HWND child) {
  HWND parent = ::GetParent(child);
  return parent ? parent : child;
}

void EnableDrop(HWND hwnd) { ::DragAcceptFiles(hwnd, TRUE); }

// 子类化：把子窗口收到的 WM_DROPFILES 转给主窗口统一处理
LRESULT CALLBACK DropForwardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR id, DWORD_PTR) {
  if (msg == WM_DROPFILES) {
    ::SendMessageW(DropOwner(hwnd), WM_DROPFILES, wParam, 0);
    return 0;
  }
  if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, DropForwardProc, id);
  // 子窗口里拖拽时的鼠标光标由系统绘制，保持默认
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// 客户区铺满整个窗口后，贴边一圈被子窗口盖住了，系统收不到主窗口的缩放命中。
// 这里让边圈子窗口对鼠标透明，命中测试自然落到主窗口的 WM_NCHITTEST 上。
LRESULT CALLBACK EdgePassThroughProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR id, DWORD_PTR) {
  if (msg == WM_NCHITTEST) {
    HWND parent = ::GetParent(hwnd);
    if (parent) {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT wr;
      ::GetWindowRect(parent, &wr);
      const int band = ResizeBorder(g_app.dpi);
      if (pt.x < wr.left + band || pt.x >= wr.right - band || pt.y < wr.top + band ||
          pt.y >= wr.bottom - band) {
        return HTTRANSPARENT;
      }
    }
  }
  if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, EdgePassThroughProc, id);
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK NavProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_ERASEBKGND: {
      // 与主窗口同色，避免移动/缩放时闪黑条
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, RGB(249, 252, 254));
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintNavBar(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lParam);
      const int hit = x / g_app.px(kNavBtnW);
      const int next = (hit >= 0 && hit < kNavCount) ? hit : -1;
      if (next != g_navHot) {
        g_navHot = next;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      g_navHot = -1;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN:
      g_navPressed = GET_X_LPARAM(lParam) / g_app.px(kNavBtnW);
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONUP: {
      const int hit = GET_X_LPARAM(lParam) / g_app.px(kNavBtnW);
      const int was = g_navPressed;
      g_navPressed = -1;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      if (hit >= 0 && hit < kNavCount && hit == was) {
        bool enabled[kNavCount]{};
        NavStates(enabled);
        if (enabled[hit]) {
          switch (hit) {
            case 0: AppGoBack(); break;
            case 1: AppGoForward(); break;
            case 2: AppGoUp(); break;
            case 3: AppRefreshView(); break;
          }
        }
      }
      return 0;
    }

    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateNavBar(HWND parent, HINSTANCE inst) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kNavClass, NavProc, inst, CS_HREDRAW | CS_VREDRAW, nullptr);
  }
  return ::CreateWindowExW(0, kNavClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackBtnId)), inst, nullptr);
}

void CreateChildren(HWND hwnd) {
  HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

  g_parts.toolbar = CreateToolbarWindow(hwnd, inst, kToolbarId);
  g_parts.navBar = CreateNavBar(hwnd, inst);
  g_parts.pathEdit = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)), inst, nullptr);

  g_parts.search = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchId)), inst, nullptr);
  ::SendMessageW(g_parts.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"在当前目录中查找"));

  g_parts.tree = CreateTreeView(hwnd, inst, kTreeId);
  g_parts.list = CreateFileList(hwnd, inst, kListId);
  g_parts.status = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                     WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), inst,
                                     nullptr);

  for (HWND c : {g_parts.list, g_parts.tree, g_parts.pathEdit, g_parts.search}) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  }
  ::SendMessageW(g_parts.status, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);

  // 让顶部整片区域都能接收文件拖放（列表与主窗口空白区本来就能收）。
  // 每个子窗口都要自己 DragAcceptFiles，再统一把 WM_DROPFILES 转给主窗口。
  int subId = 1;
  for (HWND c : {g_parts.toolbar, g_parts.navBar, g_parts.pathEdit, g_parts.search,
                 g_parts.status}) {
    EnableDrop(c);
    ::SetWindowSubclass(c, DropForwardProc, static_cast<UINT_PTR>(subId++), 0);
  }

  // 贴边一圈的子窗口要让出命中，否则主窗口收不到缩放
  for (HWND c : {g_parts.toolbar, g_parts.navBar, g_parts.pathEdit, g_parts.search,
                 g_parts.tree, g_parts.list, g_parts.status}) {
    ::SetWindowSubclass(c, EdgePassThroughProc, static_cast<UINT_PTR>(subId++), 0);
  }
}

// 地址栏布局：导航按钮 + 路径框 + 搜索框
void LayoutAddressBar(int cx) {  const int y = g_app.px(kToolbarHeight);
  const int h = g_app.px(kAddressHeight);
  const int pad = g_app.px(8);
  const int gap = g_app.px(6);
  const int searchW = g_app.px(190);
  const int navW = g_app.px(kNavBtnW) * kNavCount + g_app.px(6);

  if (g_parts.navBar) ::MoveWindow(g_parts.navBar, 0, y, navW, h, TRUE);

  int x = navW + gap;
  const int editW = cx - x - pad - gap - searchW;
  if (editW > g_app.px(120)) {
    ::MoveWindow(g_parts.pathEdit, x, y + g_app.px(3), editW, h - g_app.px(6), TRUE);
  }
  x += editW + gap;
  const int sw = cx - x - pad;
  if (sw > 0) {
    ::MoveWindow(g_parts.search, x, y + g_app.px(3), sw, h - g_app.px(6), TRUE);
  }
  // 历史状态变化后刷新按钮可用性
  if (g_parts.navBar) ::InvalidateRect(g_parts.navBar, nullptr, FALSE);
}

void OnSize(HWND hwnd, int cx, int cy) {
  if (!g_parts.toolbar) return;
  const int toolbarH = g_app.px(kToolbarHeight);
  const int addressH = g_app.px(kAddressHeight);
  const int statusH = g_app.px(kStatusHeight);

  ::MoveWindow(g_parts.toolbar, 0, 0, cx, toolbarH, TRUE);
  LayoutAddressBar(cx);

  const int contentY = toolbarH + addressH;
  const int statusTop = cy - statusH;
  const int contentH = statusTop - contentY;
  if (contentH > 0) {
    const bool showTree = g_app.settings.showTree;
    const int treeW = showTree ? g_app.px(g_treeWidth) : 0;
    ::ShowWindow(g_parts.tree, showTree ? SW_SHOW : SW_HIDE);
    if (showTree) ::MoveWindow(g_parts.tree, 0, contentY, treeW, contentH, TRUE);

    const int listX = showTree ? treeW + g_app.px(kSplitterWidth) : 0;
    ::MoveWindow(g_parts.list, listX, contentY, cx - listX, contentH, TRUE);
    // 窗口尺寸变化后名称列需要重新撑满剩余宽度
    ui::LayoutFileList(g_parts.list);
  }
  ::MoveWindow(g_parts.status, 0, statusTop, cx, statusH, TRUE);
  ::InvalidateRect(hwnd, nullptr, FALSE);
}

void OnCreate(HWND hwnd) {
  CreateChildren(hwnd);
  PopulateTree(g_parts.tree);
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  OnSize(hwnd, rc.right, rc.bottom);
  AppRefreshView();
}

void NavigateFromPathEdit(HWND hwnd) {
  wchar_t buf[MAX_PATH * 2];
  ::GetWindowTextW(g_parts.pathEdit, buf, MAX_PATH * 2);
  std::wstring target = buf;
  if (target.empty()) return;

  // 压缩包模式：允许 "包路径\内部\路径" 形式
  if (g_app.archiveMode) {
    const std::wstring base = g_app.archivePath;
    if (target.size() > base.size() &&
        ::CompareStringOrdinal(target.c_str(), static_cast<int>(base.size()), base.c_str(),
                               static_cast<int>(base.size()), TRUE) == CSTR_EQUAL) {
      std::wstring inner = target.substr(base.size());
      while (!inner.empty() && inner.front() == L'\\') inner.erase(inner.begin());
      g_app.innerPath = inner;
      AppRefreshView();
      return;
    }
  }

  if (util::IsDirectory(target)) {
    AppNavigateTo(target);
    ::SetFocus(g_parts.list);
    return;
  }
  if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
    AppOpenArchive(target);
    ::SetFocus(g_parts.list);
    return;
  }
  ::MessageBoxW(hwnd, (L"路径不存在：\n" + target).c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
  RefreshAddressBar(hwnd);
}

void OnCommand(HWND hwnd, int id, int code) {
  switch (id) {
    case kPathEditId:
      if (code == EN_KILLFOCUS) {
        // 失焦时还原为当前真实路径，避免用户误以为已跳转
        wchar_t buf[MAX_PATH * 2];
        ::GetWindowTextW(g_parts.pathEdit, buf, MAX_PATH * 2);
        if (buf[0] == L'\0') RefreshAddressBar(hwnd);
      }
      break;
    case kSearchId:
      if (code == EN_CHANGE) SetFilterFromEdit(g_parts.search);
      break;
    default:
      break;
  }
}

LRESULT OnNotify(HWND hwnd, NMHDR* hdr) {
  if (!hdr) return 0;
  if (hdr->hwndFrom == g_parts.tree) return HandleTreeNotify(hwnd, hdr);
  if (hdr->hwndFrom == g_parts.list) return HandleListNotify(hwnd, hdr);
  return 0;
}

// 回车 / Esc 在地址栏内
bool HandleAddressKey(HWND hwnd, MSG* msg) {
  if (msg->message != WM_KEYDOWN) return false;
  HWND focus = ::GetFocus();
  if (focus != g_parts.pathEdit && focus != g_parts.search) return false;

  if (msg->wParam == VK_RETURN) {
    if (focus == g_parts.pathEdit) NavigateFromPathEdit(hwnd);
    return true;
  }
  if (msg->wParam == VK_ESCAPE) {
    RefreshAddressBar(hwnd);
    ::SetFocus(g_parts.list);
    return true;
  }
  return false;
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      OnCreate(hwnd);
      return 0;

    case WM_SIZE:
      OnSize(hwnd, LOWORD(lParam), HIWORD(lParam));
      return 0;

    case WM_NCCALCSIZE: {
      // 客户区铺满整个窗口，视觉上只剩 DWM 在外侧画的 1px 边框，
      // 不再是系统默认那圈 11px 的厚边框。
      // 缩放热区改由 WM_NCHITTEST 在客户区内侧补一圈感应带。
      if (!wParam) break;
      auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
      if (::IsZoomed(hwnd)) {
        // 最大化：客户区贴合工作区，避免压住任务栏
        HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (mon && ::GetMonitorInfoW(mon, &mi)) {
          p->rgrc[0] = mi.rcWork;
          return 0;
        }
      }
      // 还原态：不改 rgrc[0]（它就是新的窗口矩形），客户区自然铺满整个窗口
      return 0;
    }

    case WM_NCHITTEST: {
      // 客户区铺满后系统不再提供缩放热区，这里按感应带宽度自己判定八向。
      // 感应带是隐形的：视觉上只有 1px 边框，可抓取范围却是 band 那么宽。
      if (::IsZoomed(hwnd)) break;
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT wr;
      ::GetWindowRect(hwnd, &wr);
      const int band = ResizeBorder(g_app.dpi);
      const bool left = pt.x < wr.left + band;
      const bool right = pt.x >= wr.right - band;
      const bool top = pt.y < wr.top + band;
      const bool bottom = pt.y >= wr.bottom - band;

      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      break;
    }

    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      mmi->ptMinTrackSize.x = g_app.px(kMinWidth);
      mmi->ptMinTrackSize.y = g_app.px(kMinHeight);
      return 0;
    }

    case WM_COMMAND:
      OnCommand(hwnd, LOWORD(wParam), HIWORD(wParam));
      return 0;

    case WM_NOTIFY:
      return OnNotify(hwnd, reinterpret_cast<NMHDR*>(lParam));

    case WM_ERASEBKGND: {
      // 拖动/缩放时新暴露的区域必须立刻填底色，否则会闪黑条
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kWindowBg);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(hwnd, &ps);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kWindowBg);
      const int top = g_app.px(kToolbarHeight);
      const int bottom = g_app.px(kToolbarHeight) + g_app.px(kAddressHeight);
      RECT addr{0, top, rc.right, bottom};
      FillRect(dc, addr, RGB(249, 252, 254));
      DrawHLine(dc, 0, rc.right, bottom - 1, kBorder);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SETFOCUS:
      if (g_parts.list) ::SetFocus(g_parts.list);
      return 0;

    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wParam);
      const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> paths;
      for (UINT i = 0; i < n; ++i) {
        wchar_t buf[MAX_PATH * 2];
        ::DragQueryFileW(drop, i, buf, MAX_PATH * 2);
        paths.push_back(buf);
      }
      ::DragFinish(drop);
      if (!paths.empty()) {
        if (paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0])) {
          AppOpenArchive(paths[0]);
        } else {
          ShowCompressDialog(hwnd, paths);
        }
      }
      return 0;
    }

    case WM_COPYDATA: {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
      if (cds && cds->dwData == 1 && cds->lpData && cds->cbData > 0) {
        const std::wstring arg(static_cast<const wchar_t*>(cds->lpData));
        if (util::IsDirectory(arg)) {
          AppNavigateTo(arg);
        } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
          AppOpenArchive(arg);
        }
      }
      return TRUE;
    }

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY: {
      WINDOWPLACEMENT wp{};
      wp.length = sizeof(wp);
      if (::GetWindowPlacement(hwnd, &wp)) {
        g_app.settings.winMax = (wp.showCmd == SW_SHOWMAXIMIZED);
        // 只记尺寸：启动位置固定居中，换显示器后旧坐标可能落到屏幕外
        if (!g_app.settings.winMax) {
          g_app.settings.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
          g_app.settings.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
      }
      util::SaveSettings(g_app.settings);
      ::PostQuitMessage(0);
      return 0;
    }

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool RegisterMainWindow(HINSTANCE inst) {
  // 必须给背景刷：窗口移动/缩放时新露出的区域要靠它填，否则会闪黑条
  return RegisterWindowClass(kMainClass, MainProc, inst, CS_HREDRAW | CS_VREDRAW,
                             ::CreateSolidBrush(kWindowBg), 101);
}

// 把窗口摆到当前显示器工作区正中。
// 用窗口的【实际】尺寸来算，避免 AdjustWindowRectEx 在不同 DPI 上下文中
// 返回值不一致导致视觉偏移。
void CenterOnWorkArea(HWND hwnd) {
  RECT wr{};
  if (!::GetWindowRect(hwnd, &wr)) return;
  RECT work{};
  if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
  }
  const int w = wr.right - wr.left;
  const int h = wr.bottom - wr.top;
  const int x = work.left + ((work.right - work.left) - w) / 2;
  const int y = work.top + ((work.bottom - work.top) - h) / 2;
  ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND CreateMainWindow(HINSTANCE inst) {
  const auto& s = g_app.settings;
  // 位置不再恢复上次坐标：换显示器或改分辨率后旧坐标可能落在屏幕外，
  // 统一改为启动时居中。
  constexpr int kDefW = 1100;
  constexpr int kDefH = 700;
  // 旧版注册表里可能残留 733x467 这类过小值（当年按 96dpi 记的），
  // 低于窗口最小尺寸就直接用默认值，不然会挤成一条。
  const int minW = g_app.px(kMinWidth);
  const int minH = g_app.px(kMinHeight);
  int w = (s.winW >= minW) ? s.winW : g_app.px(kDefW);
  int h = (s.winH >= minH) ? s.winH : g_app.px(kDefH);

  RECT work{};
  if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
  }
  // 不超过工作区，避免超出屏幕后被系统再裁一次
  if (w > work.right - work.left) w = work.right - work.left;
  if (h > work.bottom - work.top) h = work.bottom - work.top;

  HWND hwnd = ::CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, L"AIZip",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, w, h, nullptr,
                                nullptr, inst, nullptr);
  if (!hwnd) return nullptr;
  // 强制走一遍 WM_NCCALCSIZE，把标题栏从非客户区里去掉
  ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  // 客户区铺满后 DWM 不再自动画边，显式要求它在外侧留 1px 边框和投影
  const MARGINS frame{1, 1, 1, 1};
  ::DwmExtendFrameIntoClientArea(hwnd, &frame);
  CenterOnWorkArea(hwnd);
  return hwnd;
}

void UpdateMainTitle(HWND hwnd) {
  const std::wstring title =
      g_app.archiveMode ? util::BaseName(g_app.archivePath) + L" - AIZip" : L"AIZip";
  ::SetWindowTextW(hwnd, title.c_str());
}

void RefreshAddressBar(HWND) {
  std::wstring text;
  if (g_app.archiveMode) {
    text = g_app.archivePath;
    if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
  } else {
    text = g_app.curDir;
  }
  if (g_parts.pathEdit) {
    ::SetWindowTextW(g_parts.pathEdit, text.c_str());
    // 光标置于末尾，避免长路径滚动到开头
    const int len = ::GetWindowTextLengthW(g_parts.pathEdit);
    ::SendMessageW(g_parts.pathEdit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
  }
  // 前进/后退可用性随历史变化
  if (g_parts.navBar) ::InvalidateRect(g_parts.navBar, nullptr, FALSE);
}

void RefreshStatusBar(HWND hwnd) {
  if (!g_parts.status) return;
  int cx = 0;
  if (hwnd) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    cx = rc.right;
  }
  const int rightW = g_app.px(280);
  const int parts[2] = {cx - rightW > g_app.px(200) ? cx - rightW : cx / 2, -1};
  ::SendMessageW(g_parts.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

  // 统计按“已过滤后的可见项”计算，与列表显示一致
  size_t files = 0, dirs = 0;
  uint64_t total = 0;
  const auto& rows = VisibleRows();
  for (int idx : rows) {
    if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) continue;
    const Item& it = g_app.items[idx];
    if (it.isDir) {
      ++dirs;
    } else {
      ++files;
      total += it.size;
    }
  }

  std::wstring left;
  if (dirs > 0 && files > 0) {
    left = L"总计 " + std::to_wstring(files) + L" 个文件、" + std::to_wstring(dirs) + L" 个文件夹";
  } else if (dirs > 0) {
    left = L"总计 " + std::to_wstring(dirs) + L" 个文件夹";
  } else if (files > 0) {
    left = L"总计 " + std::to_wstring(files) + L" 个文件";
  } else {
    left = L"没有项目";
  }
  if (files > 0) left += L"（" + util::FormatSize(total) + L"）";

  const int selCount = static_cast<int>(GetSelectedItems().size());
  if (selCount > 0) {
    left += L"，已选中 " + std::to_wstring(selCount) + L" 项";
  }
  ::SendMessageW(g_parts.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(left.c_str()));

  std::wstring right;
  if (g_app.archiveMode && g_app.archive.IsOpen()) {
    LARGE_INTEGER li{};
    HANDLE h = ::CreateFileW(g_app.archivePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::GetFileSizeEx(h, &li);
      ::CloseHandle(h);
    }
    const std::wstring fmt = util::FileExtension(g_app.archivePath);
    right = std::wstring(z7::FormatNameFromExtension(fmt)) + L"  ·  压缩后 " +
            util::FormatSize(static_cast<uint64_t>(li.QuadPart));
  }
  ::SendMessageW(g_parts.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(right.c_str()));
}

bool HandleMainKeyMessage(MSG* msg) { return HandleAddressKey(::GetActiveWindow(), msg); }

}  // namespace ui
