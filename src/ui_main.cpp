// 主窗口：面包屑 + 工具栏 + 树 + 列表 + 状态栏
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "sevenzip_loader.h"
#include "ui.h"
#include "ui_crumb.h"
#include "ui_list.h"
#include "ui_status.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

namespace ui {
namespace {

MainParts g_parts;
constexpr const wchar_t* kMainClass = L"AIZipMainWindow";
constexpr int kMinWidth = 860;
constexpr int kMinHeight = 520;

// 拖放转发：子窗口各自注册，统一转交主窗口
HWND DropOwner(HWND child) {
  HWND parent = ::GetParent(child);
  return parent ? parent : child;
}

void EnableDrop(HWND hwnd) { ::DragAcceptFiles(hwnd, TRUE); }

LRESULT CALLBACK DropForwardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                 DWORD_PTR) {
  if (msg == WM_DROPFILES) {
    ::SendMessageW(DropOwner(hwnd), WM_DROPFILES, wParam, 0);
    return 0;
  }
  if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, DropForwardProc, id);
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void CreateChildren(HWND hwnd) {
  HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

  g_parts.toolbar = CreateToolBar(hwnd, inst, kToolbarId);
  g_parts.crumb = CreateCrumbBar(hwnd, inst, kCrumbId);
  g_parts.tree = CreateSideTree(hwnd, inst, kTreeId);
  g_parts.list = CreateFileList(hwnd, inst, kListId);
  g_parts.status = CreateStatusBar(hwnd, inst, kStatusId);

  // 搜索框内部有自己的 EDIT，只给列表设字体
  ::SendMessageW(g_parts.list, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);

  // 顶部区域也要能接文件拖放
  int subId = 1;
  for (HWND c : {g_parts.toolbar, g_parts.crumb, CrumbSearchEdit(), g_parts.tree, g_parts.status}) {
    EnableDrop(c);
    ::SetWindowSubclass(c, DropForwardProc, static_cast<UINT_PTR>(subId++), 0);
  }
}

void OnSize(HWND hwnd, int cx, int cy) {
  if (!g_parts.toolbar) return;
  const int toolH = g_app.px(kToolH);
  const int crumbH = g_app.px(kCrumbH);
  const int statusH = g_app.px(kStatusH);

  ::MoveWindow(g_parts.crumb, 0, 0, cx, crumbH, TRUE);
  ::MoveWindow(g_parts.toolbar, 0, crumbH, cx, toolH, TRUE);

  const int contentY = toolH + crumbH;
  const int statusTop = cy - statusH;
  const int contentH = statusTop - contentY;
  if (contentH > 0) {
    const int treeW = g_app.px(kSidebarW);
    ::MoveWindow(g_parts.tree, 0, contentY, treeW, contentH, TRUE);
    ::MoveWindow(g_parts.list, treeW, contentY, cx - treeW, contentH, TRUE);
    LayoutSideTree();
    LayoutFileList(g_parts.list);
  }
  ::MoveWindow(g_parts.status, 0, statusTop, cx, statusH, TRUE);
  RefreshStatusBar(hwnd);
}

void OnCreate(HWND hwnd) {
  CreateChildren(hwnd);
  RefreshCrumbBar();
  RefreshSideTree();
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  OnSize(hwnd, rc.right, rc.bottom);
  AppRefreshView();
}

LRESULT OnNotify(HWND hwnd, NMHDR* hdr) {
  if (!hdr) return 0;
  if (hdr->hwndFrom == g_parts.list) return HandleListNotify(hwnd, hdr);
  return 0;
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      OnCreate(hwnd);
      return 0;

    case WM_SIZE:
      OnSize(hwnd, LOWORD(lParam), HIWORD(lParam));
      return 0;

    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      mmi->ptMinTrackSize.x = g_app.px(kMinWidth);
      mmi->ptMinTrackSize.y = g_app.px(kMinHeight);
      return 0;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      // 搜索框的 EN_CHANGE 由 ui_search 的回调处理，地址编辑框的提交在
      // HandleMainKeyMessage 里消费回车/Esc，这里不需要额外分支。
      (void)id;
      (void)code;
      return 0;
    }

    case WM_NOTIFY:
      return OnNotify(hwnd, reinterpret_cast<NMHDR*>(lParam));

    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kBodyBg);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(hwnd, &ps);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kBodyBg);
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
  return RegisterWindowClass(kMainClass, MainProc, inst, CS_HREDRAW | CS_VREDRAW,
                             ::CreateSolidBrush(kBodyBg), 101);
}

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
  constexpr int kDefW = 1180;
  constexpr int kDefH = 760;
  const int minW = g_app.px(kMinWidth);
  const int minH = g_app.px(kMinHeight);
  int w = (s.winW >= minW) ? s.winW : g_app.px(kDefW);
  int h = (s.winH >= minH) ? s.winH : g_app.px(kDefH);

  RECT work{};
  if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
  }
  if (w > work.right - work.left) w = work.right - work.left;
  if (h > work.bottom - work.top) h = work.bottom - work.top;

  HWND hwnd = ::CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, L"AIZip",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, w, h, nullptr,
                                nullptr, inst, nullptr);
  if (!hwnd) return nullptr;
  CenterOnWorkArea(hwnd);
  return hwnd;
}

void UpdateMainTitle(HWND hwnd) {
  const std::wstring title =
      g_app.archiveMode ? util::BaseName(g_app.archivePath) + L" - AIZip" : L"AIZip";
  ::SetWindowTextW(hwnd, title.c_str());
}

void RefreshAddressBar(HWND) {
  RefreshCrumbBar();
  RefreshSideTree();
}

void RefreshToolbarState() { InvalidateToolBar(); }

HWND MainList() { return g_parts.list; }

void RefreshStatusBar(HWND hwnd) {
  (void)hwnd;
  if (!g_parts.status) return;
  RefreshStatusBarData();
}

bool HandleMainKeyMessage(MSG* msg) {
  if (HandleCrumbKey(msg)) return true;

  if (msg->message == WM_KEYDOWN && ::GetKeyState(VK_CONTROL) < 0) {
    if (msg->wParam == 'L') {
      BeginCrumbEdit();
      return true;
    }
  }
  return false;
}

// 单行 EDIT 里的按键必须直接派发：IsDialogMessage 会把回车当默认按钮吞掉，
// 地址栏/搜索框就收不到 WM_KEYDOWN。
bool IsEditKeystroke(const MSG* msg) {
  if (!msg || !msg->hwnd) return false;
  if (msg->message != WM_KEYDOWN && msg->message != WM_CHAR && msg->message != WM_SYSKEYDOWN) {
    return false;
  }
  wchar_t cls[16] = {};
  if (::GetClassNameW(msg->hwnd, cls, 16) == 0) return false;
  return ::CompareStringOrdinal(cls, -1, L"Edit", -1, TRUE) == CSTR_EQUAL;
}

}  // namespace ui
