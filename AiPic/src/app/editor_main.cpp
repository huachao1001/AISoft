// AiPicEditor 程序入口
//
// 与 AiPic 分开一个 exe：看图是「打开就走」的高频动作，编辑是「坐下来改」的
// 低频动作。合成一个程序会让看图那边多出一堆用不上的控件与状态。
//
// 这里不出现任何平台 API：DPI、窗口、消息循环都走 AiUI::Platform。
#include "editor_activity.h"
#include "util.h"

#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme_registry.h"
#include "AiUI/window.h"

#include <string>

namespace {

// 命令行里第一张能编辑的图
std::wstring firstImageArg() {
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    if (util::IsImageFile(arg) && util::PathExists(arg)) return arg;
  }
  return {};
}

// 离屏截图：把界面渲染成 PNG 后退出，不建可见窗口、不抢焦点、不动鼠标键盘。
// 开发时核对布局靠它，不要靠抓屏。
std::wstring snapshotOption() {
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    constexpr auto kPrefix = L"--snapshot=";
    const size_t n = std::wcslen(kPrefix);
    if (arg.size() > n && arg.compare(0, n, kPrefix) == 0) return arg.substr(n);
  }
  return {};
}

// 从 --size=WxH 取设计尺寸（96 DPI）
AiUI::Size snapshotSize() {
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    constexpr auto kPrefix = L"--size=";
    const size_t n = std::wcslen(kPrefix);
    if (arg.size() <= n || arg.compare(0, n, kPrefix) != 0) continue;
    const std::wstring value = arg.substr(n);
    const size_t x = value.find(L'x');
    if (x == std::wstring::npos) continue;
    const int w = std::stoi(value.substr(0, x));
    const int h = std::stoi(value.substr(x + 1));
    if (w > 0 && h > 0) return AiUI::Size{w, h};
  }
  return AiUI::Size{1080, 720};
}

// --tool=resize 之类：快照时直接切到某个工具，省得为了看图写输入脚本
bool toolOption(bool& outResize) {
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    if (arg == L"--tool=resize") { outResize = true; return true; }
    if (arg == L"--tool=crop") { outResize = false; return true; }
  }
  return false;
}

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  const int dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(dpi);

  ui::EditorActivity activity;
  if (!activity.create(nullptr)) {
    AiUI::reportFatalError(L"编辑窗口创建失败。");
    AiUI::fonts.destroy();
    return 1;
  }

  // 窗口尺寸：固定初始值后居中，不记住上次大小——编辑窗口的合适大小
  // 取决于当前这张图，记住一个数字反而经常不对
  {
    constexpr int kDefW = 1080;
    constexpr int kDefH = 720;
    int w = dpi * kDefW / 96;
    int h = dpi * kDefH / 96;

    const AiUI::Rect work = AiUI::platform()->workArea();
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    if (w > workW) w = workW;
    if (h > workH) h = workH;

    const int x = work.left + (workW - w) / 2;
    const int y = work.top + (workH - h) / 2;
    activity.setInitialBounds(AiUI::Rect{x, y, x + w, y + h});
  }

  const std::wstring initial = firstImageArg();
  if (!initial.empty()) {
    if (!activity.openImage(initial)) {
      AiUI::MessageBoxParams params;
      params.title = L"爱图编辑器";
      params.message = L"打不开这张图片：\n" + initial;
      params.icon = AiUI::MessageBoxIcon::kWarning;
      AiUI::platform()->showMessageBox(nullptr, params);
    }
  }

  // 快照模式：不走事件循环，渲染成 PNG 后直接退出
  const std::wstring shotPath = snapshotOption();
  if (!shotPath.empty()) {
    // 指定了工具就在快照前切过去：布局重建要在渲染之前发生
    bool toResize = false;
    if (toolOption(toResize)) {
      activity.setTool(toResize ? ui::Tool::kResize : ui::Tool::kCrop);
    }
    activity.show(false);
    const AiUI::Size design = snapshotSize();
    const int scale = dpi;
    const AiUI::Rect area{0, 0, design.width * scale / 96, design.height * scale / 96};
    AiUI::RgbaImage shot;
    const bool ok = AiUI::platform()->renderOffscreen(
        area, [&](AiUI::Painter& painter) { activity.paintForSnapshot(painter, area); }, shot);
    const bool saved =
        ok && AiUI::platform()->saveImagePixels(shotPath, shot, AiUI::ImageFormat::kPng);
    AiUI::fonts.destroy();
    AiUI::shutdownPlatform();
    return saved ? 0 : 1;
  }

  activity.show();

  const int code = AiUI::platform()->runEventLoop();

  AiUI::releaseFileIcons();
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
