// 主窗口：导航条 + 工具栏 + 树 + 列表 + 状态栏
//
// 窗口外壳（标题栏、边框、拖动、缩放、最小化/最大化/关闭）由 AiUI::Window
// 提供，界面内容由 XML 生成的 MainFrame 布局搭起来。这里只剩原生宿主逻辑：
// 拖放注册与单实例转发，不涉及任何布局与绘制。
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
#include "AiUI/window.h"
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

std::unique_ptr<AiUI::Window> g_window;
AiUI::generated::MainFrame* g_frame = nullptr;

// 拖放转发：ViewRoot 子窗口收不到时转交主窗口
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

// 主窗口的拖放处理：文件丢进来自动判断是压缩包还是待压缩文件
void HandleDropFiles(HWND hwnd, HDROP drop) {
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  std::vector<std::wstring> paths;
  for (UINT i = 0; i < count; ++i) {
    wchar_t buffer[MAX_PATH * 2];
    ::DragQueryFileW(drop, i, buffer, MAX_PATH * 2);
    paths.push_back(buffer);
  }
  ::DragFinish(drop);
  if (paths.empty()) return;
  if (paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0])) {
    AppOpenArchive(paths[0]);
  } else {
    ShowCompressDialog(hwnd, paths);
  }
}

// 原生宿主消息：只处理拖放与单实例转发，其余交给默认窗口过程
LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_DROPFILES:
      HandleDropFiles(hwnd, reinterpret_cast<HDROP>(wParam));
      return 0;

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

    case WM_ERASEBKGND:
      return 1;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool RegisterMainWindow(HINSTANCE inst) {
  return RegisterWindowClass(kMainClass, HostProc, inst, CS_HREDRAW | CS_VREDRAW, nullptr, 101);
}

HWND CreateMainWindow(HINSTANCE inst) {
  (void)inst;
  const auto& s = g_app.settings;
  constexpr int kDefW = 1180;
  constexpr int kDefH = 760;

  g_window = std::make_unique<AiUI::Window>();

  AiUI::WindowStyle style;
  style.topLevel = true;
  style.resizable = true;
  style.minimizable = true;
  style.maximizable = true;
  style.captionHeight = 0;  // 视觉总高与逻辑带一致
  g_window->setStyle(style);
  g_window->setMinSize(kMinWidth, kMinHeight);
  g_window->setTitle(L"AIZip");

  auto* frame = new AiUI::generated::MainFrame();
  g_frame = frame;
  g_window->setContentView(frame);

  // 关闭前保存窗口状态；窗口位置尺寸由平台层处理
  g_window->setOnClose([] {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    HWND hwnd = g_app.mainWnd;
    if (hwnd && ::GetWindowPlacement(hwnd, &wp)) {
      g_app.settings.winMax = (wp.showCmd == SW_SHOWMAXIMIZED);
      if (!g_app.settings.winMax) {
        g_app.settings.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        g_app.settings.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
      }
    }
    util::SaveSettings(g_app.settings);
    return true;
  });

  // 先算出初始尺寸，再创建窗口
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

  const int x = work.left + ((work.right - work.left) - w) / 2;
  const int y = work.top + ((work.bottom - work.top) - h) / 2;

  if (!g_window->create(nullptr)) return nullptr;
  g_app.mainWnd = static_cast<HWND>(g_window->handle());
  g_window->setInitialBounds(AiUI::Rect{x, y, x + w, y + h});

  // 各模块只负责“拿到控件后怎么用”，不再关心摆放
  AttachCrumbBar(frame->crumb);
  AttachToolBar(frame->toolBar);
  AttachSideTree(frame->tree);
  AttachFileList(frame->list);
  AttachStatusBar(frame->status);

  // 拖放同时注册在宿主窗口与 ViewRoot 窗口上：光标实际在后者
  EnableDrop(g_app.mainWnd);
  EnableDrop(static_cast<HWND>(frame->handle()));

  return g_app.mainWnd;
}

void UpdateMainTitle(HWND) {
  if (!g_window) return;
  const std::wstring title =
      g_app.archiveMode ? util::BaseName(g_app.archivePath) + L" - AIZip" : L"AIZip";
  g_window->setTitle(title);
}

void RefreshAddressBar(HWND) {
  RefreshCrumbBar();
  SyncSideTree();
}

void FocusFileList() {
  auto* frame = g_window ? dynamic_cast<AiUI::generated::MainFrame*>(g_window->contentView())
                         : nullptr;
  if (frame && frame->list) frame->list->setFocus();
}

bool HandleMainShortcut(const MSG& msg) {
  if (msg.message != WM_KEYDOWN) return false;
  if (::GetKeyState(VK_CONTROL) >= 0) return false;

  switch (msg.wParam) {
    case 'L':
      BeginAddressEdit();
      return true;
    case 'F':
      FocusSearchBox();
      return true;
    default:
      return false;
  }
}

}  // namespace ui
