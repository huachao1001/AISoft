// 主窗口：导航条 + 工具栏 + 树 + 列表 + 状态栏
//
// 窗口本身只做原生宿主（拖放、标题、设置持久化），内部界面全部由
// XML 生成的 MainFrame 布局搭起来，尺寸交给布局系统计算。
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_util.h"
#include "sevenzip_loader.h"
#include "ui_crumb.h"
#include "ui_list.h"
#include "ui_status.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "util.h"
#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"
#include "AiUI/widgets/button_bar.h"
#include "AiUI/widgets/list_view.h"
#include "AiUI/widgets/status_bar.h"
#include "AiUI/widgets/tree_view.h"

#include "main_frame.h"

#include <commctrl.h>
#include <shellapi.h>

#include <memory>

namespace ui {
namespace {

constexpr const wchar_t* kMainClass = L"AIZipMainWindow";

std::unique_ptr<AiUI::generated::MainFrame> g_frame;

// 拖放转发：子窗口各自注册，统一转交主窗口
HWND DropOwner(HWND child) {
  HWND parent = ::GetParent(child);
  return parent ? parent : child;
}

LRESULT CALLBACK DropForwardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                 DWORD_PTR) {
  if (msg == WM_DROPFILES) {
    ::SendMessageW(DropOwner(hwnd), WM_DROPFILES, wParam, 0);
    return 0;
  }
  if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, DropForwardProc, id);
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void EnableDrop(HWND hwnd) {
  if (!hwnd) return;
  ::DragAcceptFiles(hwnd, TRUE);
  ::SetWindowSubclass(hwnd, DropForwardProc, 1, 0);
}

void OnCreate(HWND hwnd) {
  g_frame = std::make_unique<AiUI::generated::MainFrame>();
  g_frame->create(hwnd, 0, AiUI::Rect{});

  // 各模块只负责“拿到控件后怎么用”，不再关心摆放
  AttachToolBar(g_frame->toolBar);
  AttachSideTree(g_frame->tree);
  AttachFileList(g_frame->list);
  AttachStatusBar(g_frame->status);

  // 顶部区域与树也要能接收拖放
  EnableDrop(hwnd);
  EnableDrop(static_cast<HWND>(g_frame->tree->handle()));
  EnableDrop(static_cast<HWND>(g_frame->status->handle()));

  RefreshAddressBar(hwnd);
  AppRefreshView();
}

void OnSize(int width, int height) {
  if (!g_frame) return;
  g_frame->layout(AiUI::Rect{0, 0, width, height});
}

}  // namespace

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      OnCreate(hwnd);
      return 0;

    case WM_SIZE:
      OnSize(LOWORD(lParam), HIWORD(lParam));
      return 0;

    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      mmi->ptMinTrackSize.x = g_app.px(kMinWidth);
      mmi->ptMinTrackSize.y = g_app.px(kMinHeight);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // 全部自绘

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(hwnd, &ps);
      // 布局覆盖整个客户区，这里只兜底铺一层底色，防止缩放瞬间露出白边
      RECT rc{};
      ::GetClientRect(hwnd, &rc);
      const COLORREF body = RGB(AiUI::colorRed(AiUI::kColorBody), AiUI::colorGreen(AiUI::kColorBody),
                                AiUI::colorBlue(AiUI::kColorBody));
      HBRUSH brush = ::CreateSolidBrush(body);
      ::FillRect(dc, &rc, brush);
      ::DeleteObject(brush);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SETFOCUS:
      FocusFileList();
      return 0;

    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wParam);
      const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> paths;
      for (UINT i = 0; i < count; ++i) {
        wchar_t buffer[MAX_PATH * 2];
        ::DragQueryFileW(drop, i, buffer, MAX_PATH * 2);
        paths.push_back(buffer);
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
      g_frame.reset();

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

bool RegisterMainWindow(HINSTANCE inst) {
  return RegisterWindowClass(kMainClass, MainProc, inst, CS_HREDRAW | CS_VREDRAW, nullptr, 101);
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
  if (g_frame) {
    g_frame->crumb->refresh();
    RefreshSideTree();
  }
}

void FocusFileList() {
  if (g_frame) g_frame->list->setFocus();
}

bool HandleMainShortcut(const MSG& msg) {
  if (msg.message != WM_KEYDOWN) return false;
  if (::GetKeyState(VK_CONTROL) >= 0) return false;

  switch (msg.wParam) {
    case 'L':
      if (g_frame) g_frame->crumb->beginEdit();
      return true;
    case 'F':
      if (g_frame) g_frame->crumb->focusSearch();
      return true;
    default:
      return false;
  }
}

}  // namespace ui
