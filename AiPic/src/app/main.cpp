// 程序入口：单实例、命令行、主窗口
//
// 不出现任何平台 API：DPI、窗口状态、消息循环都走 AiUI::Platform。
#include "app.h"
#include "main_activity.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/icon.h"
#include "AiUI/platform.h"
#include "AiUI/theme_registry.h"

namespace {

// 看图只开一个窗口：左右键翻页的前提是「当前有一张图」，多开窗口会让
// 翻页落到哪个窗口变得不确定。再次打开别的图就让它切换过去。
constexpr wchar_t kInstanceId[] = L"AIPic.Main";

// 命令行里第一张能显示的图。目录或别的文件一律忽略：
// 看图程序没有「浏览目录」这回事，替用户猜一张只是浪费时间。
std::wstring firstImageArg() {
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    if (util::IsImageFile(arg) && util::PathExists(arg)) return arg;
  }
  return {};
}

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  g_app.settings = util::LoadSettings();
  g_app.dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(g_app.dpi);

  // 全局默认主题：没有单独指定主题的界面都从这里取。
  // 名字对应 themes.xml 里的 <Theme name="...">，找不到则用浅色。
  AiUI::Activity::setDefaultTheme(
      &AiUI::ThemeRegistry::getOr(g_app.settings.theme, AiUI::kLightTheme));

  const std::wstring initial = firstImageArg();

  // 单实例：第二个进程把路径递给已运行窗口后立即退出。
  // 先启动的进程可能还没建完窗口，短暂等一下再转发。
  if (!AiUI::platform()->claimSingleInstance(kInstanceId)) {
    for (int i = 0; i < 20; ++i) {
      if (AiUI::platform()->forwardToRunningInstance(kInstanceId, initial)) break;
      AiUI::platform()->sleepMillis(50);
    }
    AiUI::fonts.destroy();
    return 0;
  }

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

  // 没有带图就停在首页，让用户拖一张进来或点「打开图片」。
  if (!initial.empty()) {
    if (!activity.openImage(initial)) {
      AiUI::MessageBoxParams params;
      params.title = L"爱图";
      params.message = L"打不开这张图片：\n" + initial;
      params.icon = AiUI::MessageBoxIcon::kWarning;
      AiUI::platform()->showMessageBox(g_app.mainWnd, params);
    }
  }

  activity.show();
  if (g_app.settings.winMax && activity.window()) {
    if (AiUI::PlatformWindow* pw = activity.window()->platformWindow()) pw->toggleMaximize();
  }
  // 双击图片拉起的是新进程：系统可能把焦点留在资源管理器，
  // 窗口只闪一下任务栏。这里显式置顶，保证用户立刻看到图。
  if (activity.window()) AiUI::platform()->bringToFront(activity.window()->handle());

  const int code = AiUI::platform()->runEventLoop();

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
