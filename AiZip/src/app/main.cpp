// 程序入口：单实例、命令行分发、主窗口
//
// 不出现任何平台 API：DPI、公共控件、消息循环、窗口状态都走 AiUI::Platform。
#include "app.h"
#include "main_activity.h"
#include "shell_menu.h"
#include "single_instance.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/icon.h"
#include "AiUI/platform.h"

namespace {

// 无参数启动且已有主窗口时，把路径转过去；返回是否应退出本进程
bool forwardIfRunning(const std::wstring& path) {
  return ui::ForwardPathToRunningWindow(path);
}

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  g_app.settings = util::LoadSettings();
  g_app.dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(g_app.dpi);

  // 右键菜单默认注册到当前用户（不需要管理员，也不改任何文件关联）。
  // 程序换位置后旧菜单会指向失效路径，所以每次启动都校对一遍。
  if (!shell::IsContextMenuRegistered()) {
    const std::wstring err = shell::RegisterContextMenu();
    if (!err.empty()) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = err;
      params.icon = AiUI::MessageBoxIcon::kWarning;
      AiUI::platform()->showMessageBox(nullptr, params);
    }
  }

  LaunchRequest req = ParseLaunchArgs();

  // 右键菜单发起的压缩类动作：静态动词没有 %*，多选时系统为每个文件各起
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

  // 浏览模式：已有主窗口时把路径转过去。用平台层的单实例锁判断，
  // 避免两个进程几乎同时启动时都在对方建窗之前查不到窗口。
  const std::wstring targetPath = req.paths.empty() ? L"" : req.paths[0];
  if (!AiUI::platform()->claimSingleInstance(L"AIZip.Browser.v1")) {
    // 先启动的进程可能还没建完窗口，短暂等一下再转发
    bool forwarded = false;
    for (int i = 0; i < 20 && !forwarded; ++i) {
      forwarded = forwardIfRunning(targetPath);
      if (!forwarded) AiUI::platform()->sleepMillis(50);
    }
    if (!forwarded) forwardIfRunning(L"");
    AiUI::fonts.destroy();
    return 0;
  }

  // 初始目录：上次浏览的目录，其次用户目录，最后退回根目录
  if (!g_app.settings.lastDir.empty() && util::IsDirectory(g_app.settings.lastDir)) {
    g_app.curDir = g_app.settings.lastDir;
  } else {
    const std::wstring home = AiUI::platform()->specialFolder(AiUI::SpecialFolder::kHome);
    g_app.curDir = home.empty() ? AiUI::platform()->executablePath() : home;
    g_app.curDir = util::DirName(g_app.curDir);
  }

  // 初始目录记为历史第一项，否则首次导航后 historyPos 停在 0，返回按钮永远禁用
  g_app.history.assign(1, g_app.curDir);
  g_app.historyPos = 0;

  // 主窗口交给 MainActivity：XML 布局与控件装配都在里面
  ui::MainActivity activity;
  if (!activity.create(nullptr)) {
    AiUI::reportFatalError(L"主窗口创建失败。");
    AiUI::fonts.destroy();
    return 1;
  }
  g_app.mainWnd = activity.window()->handle();

  // 恢复上次的窗口尺寸并居中；超出工作区时收进来
  {
    const auto& s = g_app.settings;
    constexpr int kDefW = 1180;
    constexpr int kDefH = 760;
    const int minW = g_app.px(ui::kMinWidth);
    const int minH = g_app.px(ui::kMinHeight);
    int w = (s.winW >= minW) ? s.winW : g_app.px(kDefW);
    int h = (s.winH >= minH) ? s.winH : g_app.px(kDefH);

    const AiUI::Rect work = AiUI::platform()->workArea();
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
  if (g_app.settings.winMax && activity.window()) {
    if (AiUI::PlatformWindow* pw = activity.window()->platformWindow()) pw->toggleMaximize();
  }

  const int code = AiUI::platform()->runEventLoop();

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
