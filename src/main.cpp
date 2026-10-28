// 程序入口：单实例、DPI、命令行分发
#include "app.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

namespace {

constexpr const wchar_t* kMutexName = L"Local\\AIZip.SingleInstance.v1";

// 解析命令行：返回第一个非选项参数（文件或目录）
std::wstring ParseFirstArgument() {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return L"";
  std::wstring first;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (!a.empty() && a[0] == L'-') continue;
    first = a;
    break;
  }
  ::LocalFree(argv);
  return first;
}

void ShowFatal(const wchar_t* message) {
  ::MessageBoxW(nullptr, message, L"AIZip", MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  util::EnableDpiAwareness();

  // 单实例：已有进程时把参数转发过去后退出
  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
  if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = ::FindWindowW(L"AIZipMainWindow", nullptr);
    if (existing) {
      const std::wstring arg = ParseFirstArgument();
      if (!arg.empty()) {
        COPYDATASTRUCT cds{};
        cds.dwData = 1;
        cds.cbData = static_cast<DWORD>((arg.size() + 1) * sizeof(wchar_t));
        cds.lpData = const_cast<wchar_t*>(arg.c_str());
        ::SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
      }
      ::SetForegroundWindow(existing);
      ::ShowWindow(existing, SW_RESTORE);
    }
    if (mutex) ::CloseHandle(mutex);
    return 0;
  }

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                               ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                               ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  ::InitCommonControlsEx(&icc);

  g_app.instance = inst;
  g_app.settings = util::LoadSettings();

  // 初始目录
  if (!g_app.settings.lastDir.empty() && util::IsDirectory(g_app.settings.lastDir)) {
    g_app.curDir = g_app.settings.lastDir;
  } else {
    wchar_t profile[MAX_PATH];
    if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) == S_OK) {
      g_app.curDir = profile;
    } else {
      g_app.curDir = L"C:\\";
    }
  }

  // 启动横幅用的字体需在窗口创建前按 DPI 生成
  g_app.dpi = util::DpiForWindow(nullptr);
  ui::fonts.Create(g_app.dpi);

  if (!ui::RegisterMainWindow(inst)) {
    ShowFatal(L"窗口类注册失败。");
    ui::fonts.Destroy();
    if (mutex) ::CloseHandle(mutex);
    return 1;
  }

  HWND mainWnd = ui::CreateMainWindow(inst);
  if (!mainWnd) {
    ShowFatal(L"主窗口创建失败。");
    ui::fonts.Destroy();
    if (mutex) ::CloseHandle(mutex);
    return 1;
  }
  g_app.mainWnd = mainWnd;

  // 命令行参数：压缩包则直接浏览，目录则进入，其他文件则打开所在目录并选中
  const std::wstring arg = ParseFirstArgument();
  if (!arg.empty() && util::PathExists(arg)) {
    if (util::IsDirectory(arg)) {
      AppNavigateTo(arg);
    } else if (arc::Archive::IsArchiveFile(arg)) {
      AppOpenArchive(arg);
    } else {
      AppNavigateTo(util::DirName(util::AbsolutePath(arg)));
    }
  } else {
    AppRefreshView();
  }

  ::ShowWindow(mainWnd, g_app.settings.winMax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  ::UpdateWindow(mainWnd);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (ui::HandleMainKeyMessage(&msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
      continue;
    }
    if (!::IsDialogMessageW(mainWnd, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  ui::ClearIconCache();
  ui::fonts.Destroy();
  if (mutex) ::CloseHandle(mutex);
  return static_cast<int>(msg.wParam);
}
