// 程序入口：单实例、命令行分发、主窗口
//
// 不出现任何平台 API：DPI、公共控件、消息循环、窗口状态都走 AiUI::Platform。
#include "app.h"
#include "config_client.h"
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

namespace {

// 浏览窗口的实例标识：以压缩包的规范化路径为准。
//   - 同一个包 → 同一个 id → 已有窗口被前置，不再开第二个
//   - 不同包   → 不同 id → 各开一个窗口
// 路径不区分大小写（Windows 语义），并先转成绝对路径，
// 免得 `abc.zip` 与 `.\abc.zip` 被当成两个包。
std::wstring BrowseInstanceId(const std::wstring& path) {
  if (path.empty()) return L"AIZip.Browser.home";
  const std::wstring full = util::AbsolutePath(path);
  const uint32_t hash = util::StableHash(util::ToLower(full));
  wchar_t suffix[16] = {};
  swprintf_s(suffix, L"%08X", hash);
  return std::wstring(L"AIZip.Browser.") + suffix;
}

// 系统集成的自愈：发现菜单或关联不对就调配置模块修一次。
//
// 三个前提都从「自己所在目录」推出，不写死任何路径：
//   - 安装目录 = 本 exe 所在目录（与安装器把组件放在这里一致）
//   - 宿主路径 = 本 exe
//   - 安装范围 = 安装时记录的标记；开发期没有标记，按当前用户处理
void RepairSystemIntegration() {
  const std::wstring hostExe = AiUI::platform()->executablePath();
  if (hostExe.empty()) return;

  const size_t slash = hostExe.find_last_of(L'\\');
  if (slash == std::wstring::npos) return;
  const std::wstring installDir = hostExe.substr(0, slash);

  const bool allUsers = shell::InstalledScope() == AiUI::InstallScope::kAllUsers;

  if (config_client::isReady(installDir, hostExe, allUsers)) return;
  config_client::apply(installDir, hostExe, allUsers);
}

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  g_app.settings = util::LoadSettings();
  g_app.dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(g_app.dpi);

  // 全局默认主题：对话框等没有单独指定主题的界面都从这里取。
  // 名字对应 themes.xml 里的 <Theme name="...">，找不到则用浅色。
  AiUI::Activity::setDefaultTheme(
      &AiUI::ThemeRegistry::getOr(g_app.settings.theme, AiUI::kLightTheme));

  // 右键菜单与文件关联的自愈：实现都在同目录的 aizip_config.dll 里。
  //
  // 为什么主程序也要做这件事：这两个能力挂在系统里，会被别的软件清掉、
  // 会因程序换位置而指向失效路径。让主程序每次启动校对一次，用户不必
  // 为了修好右键菜单而重装一遍。
  //
  // 失败一律静默：用户点的是「打开压缩文件」，不该因为写不了注册表被拦下。
  RepairSystemIntegration();

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
  // 浏览模式：按「打开的是哪个压缩包」区分实例。
  // 同一个包再次打开就复用已有窗口（用户是在找它，不是要第二份）；
  // 不同的包各开一个窗口（用户要并排比较两个包的内容）。
  const std::wstring browsePath = req.paths.empty() ? L"" : req.paths[0];
  const std::wstring instanceId = BrowseInstanceId(browsePath);
  if (!AiUI::platform()->claimSingleInstance(instanceId)) {
    // 先启动的进程可能还没建完窗口，短暂等一下再转发
    bool forwarded = false;
    for (int i = 0; i < 20 && !forwarded; ++i) {
      forwarded = AiUI::platform()->forwardToRunningInstance(instanceId, browsePath);
      if (!forwarded) AiUI::platform()->sleepMillis(50);
    }
    AiUI::fonts.destroy();
    return 0;
  }
  // 已接管实例：窗口收尾时释放锁，让同一个包下次还能重新打开
  g_app.browseInstanceId = instanceId;

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
    // 没带路径：停在首页，让用户拖包进来或点「打开压缩包」。
    //
    // 以前这里直接列上次浏览的目录：用户双击图标就想看某个包，却先看到一堆
    // 无关的文件，还得自己在里面找。主界面只服务「打开压缩包」这一件事。
    g_app.homeMode = true;
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

  // 包内条目落地用的镜像目录是纯缓存，退出时整棵清掉：
  // 留着只会让 temp 里堆一堆和用户当前操作无关的文件。
  AppClearNestedArchives();

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
