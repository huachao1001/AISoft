// 程序入口：单实例、DPI、命令行分发
#include "app.h"
#include "main_activity.h"
#include "shell_menu.h"
#include "single_instance.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/font.h"
#include "AiUI/icon.h"
#include "AiUI/platform.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

// 浏览窗口的单实例锁：只包住"有没有主窗口"，与右键菜单的收件箱互斥体互不干扰
constexpr const wchar_t* kBrowserMutex = L"Local\\AIZip.Browser.v1";

void ShowFatal(const wchar_t* message) {
  ::MessageBoxW(nullptr, message, L"AIZip", MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  util::EnableDpiAwareness();

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                               ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                               ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  ::InitCommonControlsEx(&icc);

  g_app.instance = hInst;
  g_app.settings = util::LoadSettings();
  g_app.dpi = util::DpiForWindow(nullptr);
  AiUI::startupPlatform();
  AiUI::fonts.create(g_app.dpi);

  // 右键菜单默认注册到当前用户（不需要管理员，也不改任何文件关联）。
  // 程序换位置后旧菜单会指向失效路径，所以每次启动都校对一遍。
  if (!shell::IsContextMenuRegistered()) {
    const std::wstring err = shell::RegisterContextMenu();
    if (!err.empty()) {
      ::MessageBoxW(nullptr, err.c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
    }
  }

  LaunchRequest req = ParseLaunchArgs();

  // 右键菜单发起的压缩类动作：静态动词没有 %*，多选时 shell 为每个文件各起
  // 一个进程。先在收集窗口里把同批进程合并成一次操作，再往下走。
  if (req.explicitAction && req.action != LaunchRequest::Action::kExtract) {
    LaunchRequest merged;
    if (!inst::Collect(req, merged)) {  // 已转交给先启动的实例
      AiUI::fonts.destroy();
      return 0;
    }
    req = std::move(merged);
  }

  // 资源管理器右键：只弹对话框，不创建主窗口
  if (req.explicitAction) {
    AppDispatchLaunch(req);
    AiUI::fonts.destroy();
    return 0;
  }

  // 浏览模式：无参数双击 exe 打开主窗口；带路径则定位过去
  // 先用一个独立的互斥体把"只允许一个浏览窗口"定住：
  // 原来的实现靠 FindWindow 判断，如果两个进程几乎同时启动，都在对方建窗之前
  // 查不到窗口，就会各自开一个主窗口。
  HANDLE browserLock = ::CreateMutexW(nullptr, FALSE, kBrowserMutex);
  const bool browserExists = browserLock && ::GetLastError() == ERROR_ALREADY_EXISTS;
  if (browserExists) {
    // 先启动的进程可能还没建完窗口，短暂等一下再转发
    const std::wstring path = req.paths.empty() ? L"" : req.paths[0];
    bool forwarded = false;
    for (int i = 0; i < 20 && !forwarded; ++i) {
      forwarded = ui::ForwardPathToRunningWindow(path);
      if (!forwarded) ::Sleep(50);
    }
    if (!forwarded) ui::ForwardPathToRunningWindow(L"");
    ::CloseHandle(browserLock);
    AiUI::fonts.destroy();
    return 0;
  }
  // 主窗口可能已存在但互斥体被上次崩溃遗留（极少见），补一次检查
  if (!req.paths.empty() && ui::ForwardPathToRunningWindow(req.paths[0])) {
    if (browserLock) ::CloseHandle(browserLock);
    AiUI::fonts.destroy();
    return 0;
  }

  // 初始目录：上次浏览的目录，其次用户目录，最后退回 C 盘根
  if (!g_app.settings.lastDir.empty() && util::IsDirectory(g_app.settings.lastDir)) {
    g_app.curDir = g_app.settings.lastDir;
  } else {
    PWSTR profile = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) {
      g_app.curDir = profile;
      ::CoTaskMemFree(profile);
    } else {
      g_app.curDir = L"C:\\";
    }
  }

  // 初始目录记为历史第一项，否则首次导航后 historyPos 停在 0，返回按钮永远禁用
  g_app.history.assign(1, g_app.curDir);
  g_app.historyPos = 0;

  // 主窗口交给 MainActivity：XML 布局与控件装配都在里面
  ui::MainActivity activity;
  if (!activity.create(nullptr)) {
    ShowFatal(L"主窗口创建失败。");
    AiUI::fonts.destroy();
    if (browserLock) ::CloseHandle(browserLock);
    return 1;
  }
  HWND mainWnd = static_cast<HWND>(activity.window()->handle());
  g_app.mainWnd = mainWnd;

  // 恢复上次的窗口尺寸并居中；超出工作区时收进来
  {
    const auto& s = g_app.settings;
    constexpr int kDefW = 1180;
    constexpr int kDefH = 760;
    const int minW = g_app.px(ui::kMinWidth);
    const int minH = g_app.px(ui::kMinHeight);
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
    activity.setInitialBounds(AiUI::Rect{x, y, x + w, y + h});
  }

  // 命令行带的路径：目录进入，压缩包直接浏览，其他文件定位到所在目录
  if (!req.paths.empty()) {
    const std::wstring& arg = req.paths[0];
    if (util::IsDirectory(arg)) {
      AppNavigateTo(arg);
    } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
      AppOpenArchive(arg);
    } else {
      AppNavigateTo(util::DirName(util::AbsolutePath(arg)));
    }
  } else {
    AppRefreshView();
  }

  activity.show();
  if (g_app.settings.winMax) ::ShowWindow(mainWnd, SW_SHOWMAXIMIZED);
  ::UpdateWindow(mainWnd);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // 快捷键由 MainActivity 通过 ViewRoot 的按键回调接管，这里只管派发。
    // 不用 IsDialogMessage：它会把回车吃掉，自绘控件收不到。
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  if (browserLock) ::CloseHandle(browserLock);
  return static_cast<int>(msg.wParam);
}

