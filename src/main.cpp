// 程序入口：单实例、DPI、命令行分发
#include "app.h"
#include "shell_menu.h"
#include "single_instance.h"
#include "ui.h"
#include "util.h"

#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  util::EnableDpiAwareness();

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                               ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                               ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  ::InitCommonControlsEx(&icc);

  g_app.instance = inst;
  g_app.settings = util::LoadSettings();
  g_app.dpi = util::DpiForWindow(nullptr);
  ui::fonts.Create(g_app.dpi);

  // 右键菜单默认注册到当前用户（不需要管理员，也不改任何文件关联）。
  // 程序换位置后旧菜单会指向失效路径，所以每次启动都校对一遍。
  if (!shell::IsContextMenuRegistered()) {
    const std::wstring err = shell::RegisterContextMenu();
    if (!err.empty()) {
      ::MessageBoxW(nullptr, err.c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
    }
  }

  LaunchRequest req = ParseLaunchArgs();
  if (req.paths.empty()) {
    ::MessageBoxW(nullptr,
                  L"用法：在文件或压缩包上点右键选 AIZip，\n"
                  L"或把文件拖到 AIZip.exe 上。",
                  L"AIZip", MB_OK | MB_ICONINFORMATION);
  } else {
    // 静态动词没有 %*：多选时 shell 为每个文件各起一个进程。压缩类动作先聚合
    // 成一次操作（一个包），解压则各管各的。
    if (req.action != LaunchRequest::Action::kExtract) {
      LaunchRequest merged;
      if (!inst::Collect(req, merged)) return 0;   // 已转交给先启动的实例
      req = std::move(merged);
    }
    AppDispatchLaunch(req);
  }

  ui::fonts.Destroy();
  return 0;
}
