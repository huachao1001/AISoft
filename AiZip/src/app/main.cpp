// 程序入口：单实例、命令行分发、主窗口
//
// 不出现任何平台 API：DPI、公共控件、消息循环、窗口状态都走 AiUI::Platform。
#include "app.h"
#include "main_activity.h"
#include "dlg_progress.h"
#include "shell_menu.h"
#include "single_instance.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/icon.h"
#include "AiUI/platform.h"
#include "AiUI/theme_registry.h"

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  // 安装程序的注册通道：装好文件后由它拉起本程序，写完注册表就退出。
  // 放在读配置、建窗口之前，退出码就是注册结果，安装器据此判断成败。
  // --scope=all-users 由安装器按用户选择传入：注册表写 HKLM 还是 HKCU
  // 只有安装那一刻知道，程序自己猜不出来。
  {
    const auto args = AiUI::platform()->commandLineArgs();
    bool registering = false;
    for (const auto& arg : args) {
      if (arg == L"--register-shell-menu") registering = true;
      else if (arg == L"--scope=all-users") AiUI::setCurrentInstallScope(AiUI::InstallScope::kAllUsers);
      else if (arg == L"--scope=current-user") {
        AiUI::setCurrentInstallScope(AiUI::InstallScope::kCurrentUser);
      }
    }
    if (registering) {
      // 文件关联失败不算致命：右键菜单才是主功能，关联只是「打开方式」
      // 里的候选。只要菜单注册成功就报成功，否则用户会看到一个装完却
      // 打不开的提示，而实际上程序完全可用。
      std::wstring err = shell::RegisterContextMenu(AiUI::currentInstallScope());
      shell::RegisterFileAssociations();
      AiUI::shutdownPlatform();
      return err.empty() ? 0 : 1;
    }
  }

  g_app.settings = util::LoadSettings();
  g_app.dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(g_app.dpi);

  // 全局默认主题：对话框等没有单独指定主题的界面都从这里取。
  // 名字对应 themes.xml 里的 <Theme name="...">，找不到则用浅色。
  AiUI::Activity::setDefaultTheme(
      &AiUI::ThemeRegistry::getOr(g_app.settings.theme, AiUI::kLightTheme));

  // 右键菜单默认注册到当前用户（不需要管理员，也不改任何文件关联）。
  // 程序换位置后旧菜单会指向失效路径，所以每次启动都校对一遍。
  // 安装版按安装时选的范围注册，否则「所有用户」装的程序会又往 HKCU 写一份。
  //
  // 这里是自愈性质的后台动作，失败一律静默：用户点的是「打开压缩文件」，
  // 不该因为写不了注册表而被弹窗拦住。注册失败的真实报错由安装器的
  // --register-shell-menu 通道通过退出码上报。
  AiUI::setCurrentInstallScope(shell::InstalledScope());
  if (!shell::IsContextMenuRegistered(AiUI::currentInstallScope())) {
    shell::RegisterContextMenu(AiUI::currentInstallScope());
  }
  // 文件关联同样是自愈性质：登记为受支持后缀的候选打开方式。
  // 静默处理，失败不影响主流程。
  if (!shell::AreFileAssociationsRegistered()) {
    shell::RegisterFileAssociations();
  }

  LaunchRequest req = ParseLaunchArgs();

  // 右键菜单发起的动作：静态动词没有 %*，多选时系统为每个文件各起
  // 一个进程。先在收集窗口里把同批进程合并成一次操作，再往下走。
  // 「解压到当前目录」同样要合并，否则多个包会叠出多个进度框。
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
  // 浏览模式：每个进程独立建窗口，不转发到已有窗口。
  // 用户打开两个压缩包时就是要两个窗口同时对比，把第二个塞进第一个
  // 反而会丢掉前一个的浏览位置。

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
  // 双击压缩包拉起的是新进程：系统可能把焦点留在资源管理器，
  // 窗口只闪一下任务栏。这里显式置顶，保证用户立刻看到内容。
  if (activity.window()) AiUI::platform()->bringToFront(activity.window()->handle());

  const int code = AiUI::platform()->runEventLoop();

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
