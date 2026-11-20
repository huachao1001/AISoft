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
#include "AiUI/view_root.h"
#include "AiUI/widgets/side_panel.h"
#include "AiUI/window.h"

#include <string>
#include <vector>

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

// ── 离屏快照 ──
//
// 开发时要把界面「拍」下来看效果，但不能建可见窗口、不能抢焦点，更不能
// 移动用户正在用的鼠标键盘。做法是：照常建窗口（创建时即为不可见），
// 不 show、不进事件循环，直接把视图树离屏渲染成 PNG 后退出。
//
// 用法：
//   AiPic.exe --snapshot=<输出图> [图片] [--size=WxH]
//             [--click=x,y ...] [--hover=x,y ...]
//             [--collapse | --expand] [--tooltip=<视图id>]
//             [--present] [--wheel=x,y,delta] [--dclick=x,y]
// --size 是 96 DPI 下的设计尺寸。--click/--hover 的坐标是窗口客户区坐标，
// 按出现顺序依次施加，只走视图树的合成事件，完全不经过系统输入。
// --collapse/--expand 直接调控件 API：按钮坐标随 DPI 与排版变化，
// 写死坐标的脚本一改样式就失效。
// --present 进全屏演示；--wheel/--dclick 用来验证演示态是否真的锁住了画面。
struct SnapshotAction {
  enum class Kind {
    kClick,
    kHover,
    kCollapse,
    kTooltip,
    kPresent,
    kWheel,
    kDoubleClick,
    kRotateMidway
  } kind = Kind::kClick;
  AiUI::Point at;
  // kCollapse 用：true 表示收缩，false 表示展开
  bool flag = true;
  // kTooltip 用：目标视图 id（XML 里写的那个，窄字符用于 makeId）
  std::string viewId;
  // kWheel 用：滚轮增量，正数为放大
  int wheelDelta = 0;
};

struct SnapshotOptions {
  bool enabled = false;
  std::wstring outPath;
  std::wstring imagePath;
  int width = 0;
  int height = 0;
  std::vector<SnapshotAction> actions;
};

// 只认非负整数：命令行里的尺寸与坐标不会是负数，多一位就判失败
bool isInt(const std::wstring& text, int& out) {
  if (text.empty()) return false;
  int value = 0;
  for (wchar_t ch : text) {
    if (ch < L'0' || ch > L'9') return false;
    value = value * 10 + (ch - L'0');
    if (value > 100000) return false;
  }
  out = value;
  return true;
}

// 拆 "x,y"；解析失败返回 false
bool parsePoint(const std::wstring& text, AiUI::Point& out) {
  const size_t comma = text.find(L',');
  if (comma == std::wstring::npos) return false;
  int x = 0;
  int y = 0;
  if (!isInt(text.substr(0, comma), x)) return false;
  if (!isInt(text.substr(comma + 1), y)) return false;
  out = AiUI::Point{x, y};
  return true;
}

// 拆 "--key=value"
bool splitOption(const std::wstring& arg, const std::wstring& key, std::wstring& value) {
  const std::wstring prefix = key + L"=";
  if (arg.size() <= prefix.size()) return false;
  if (arg.compare(0, prefix.size(), prefix) != 0) return false;
  value = arg.substr(prefix.size());
  return true;
}

SnapshotOptions parseSnapshot(const std::vector<std::wstring>& args) {
  SnapshotOptions opts;
  for (const std::wstring& arg : args) {
    std::wstring value;

    if (splitOption(arg, L"--snapshot", value)) {
      opts.enabled = true;
      opts.outPath = value;
      continue;
    }
    if (splitOption(arg, L"--size", value)) {
      const size_t x = value.find_first_of(L"xX*");
      if (x != std::wstring::npos) {
        int w = 0;
        int h = 0;
        if (isInt(value.substr(0, x), w) && isInt(value.substr(x + 1), h)) {
          opts.width = w;
          opts.height = h;
        }
      }
      continue;
    }
    if (splitOption(arg, L"--click", value)) {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kClick;
      if (parsePoint(value, action.at)) opts.actions.push_back(action);
      continue;
    }
    if (splitOption(arg, L"--hover", value)) {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kHover;
      if (parsePoint(value, action.at)) opts.actions.push_back(action);
      continue;
    }
    // 收缩/展开侧栏：直接调控件 API，不去点那颗按钮——按钮坐标随 DPI 与
    // 顶栏排版变化，写死坐标的脚本一改样式就失效。
    if (arg == L"--collapse" || arg == L"--expand") {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kCollapse;
      action.flag = arg == L"--collapse";
      opts.actions.push_back(action);
      continue;
    }
    if (splitOption(arg, L"--tooltip", value)) {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kTooltip;
      // 选项值是宽字符，id 在代码里按窄字符算，这里转一次
      action.viewId.assign(value.begin(), value.end());
      opts.actions.push_back(action);
      continue;
    }
    // 进演示态：验证「投影时画面钉住」必须能真的进入全屏，只点 F5 不行——
    // 快照里没有键盘，走 API 才可控
    if (arg == L"--present") {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kPresent;
      opts.actions.push_back(action);
      continue;
    }
    // "x,y,delta"：故意多一段反方向滚轮，用来看锁定是否真的生效
    if (splitOption(arg, L"--wheel", value)) {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kWheel;
      const size_t first = value.find(L',');
      const size_t second = first == std::wstring::npos ? first : value.find(L',', first + 1);
      if (second != std::wstring::npos && parsePoint(value.substr(0, second), action.at)) {
        // 缩小的增量是负数，不能走只认非负的 isInt
        const std::wstring delta = value.substr(second + 1);
        int magnitude = 0;
        const bool negative = !delta.empty() && delta[0] == L'-';
        if (isInt(negative ? delta.substr(1) : delta, magnitude)) {
          action.wheelDelta = negative ? -magnitude : magnitude;
          opts.actions.push_back(action);
        }
      }
      continue;
    }
    if (splitOption(arg, L"--dclick", value)) {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kDoubleClick;
      if (parsePoint(value, action.at)) opts.actions.push_back(action);
      continue;
    }
    // 旋转到一半停住再截：棋盘格溢出这类问题只在动画中途出现，
    // 截落位后的那一帧永远看不出来
    if (arg == L"--rotate-midway") {
      SnapshotAction action;
      action.kind = SnapshotAction::Kind::kRotateMidway;
      opts.actions.push_back(action);
      continue;
    }
    // 剩下的非选项参数里挑第一张能打开的图
    if (!arg.empty() && arg[0] != L'-' && opts.imagePath.empty() &&
        util::IsImageFile(arg) && util::PathExists(arg)) {
      opts.imagePath = arg;
    }
  }
  return opts;
}

// 快照模式：建窗口但不可见，合成事件只走视图树，渲染完立刻退出。
// 返回退出码；opts.enabled 为 false 时返回 -1 表示「不是快照模式」。
int runSnapshot(const SnapshotOptions& opts) {
  if (!opts.enabled || opts.outPath.empty()) return -1;

  ui::MainActivity activity;
  if (!activity.create(nullptr)) return 1;

  if (!opts.imagePath.empty() && !activity.openImage(opts.imagePath)) return 1;

  AiUI::Window* window = activity.window();
  AiUI::ViewRoot* root = window ? window->contentView()->root() : nullptr;
  if (!root) return 1;

  // --size 给的是 96 DPI 下的设计尺寸，与布局参数同一套单位；
  // 出图按当前 DPI 放大，拍出来的比例才跟真实窗口一致。
  const int d = root->dpi();
  const int designW = opts.width > 0 ? opts.width : 1180;
  const int designH = opts.height > 0 ? opts.height : 760;
  const AiUI::Size shotSize{(designW * d + 48) / 96, (designH * d + 48) / 96};

  AiUI::SidePanel* panel = activity.findView<AiUI::SidePanel>("sidePanel");

  // 动作要命中控件，必须先按最终尺寸排版：否则视图还是建窗时的（可能为 0）
  // 尺寸，点击与滚轮都落在空白处，测出来的「没反应」是假的
  root->layout(AiUI::Rect{0, 0, shotSize.width, shotSize.height});

  for (const SnapshotAction& action : opts.actions) {
    switch (action.kind) {
      case SnapshotAction::Kind::kHover:
        root->sendMouseMove(action.at);
        break;
      case SnapshotAction::Kind::kCollapse:
        // 不要动画：截的是一帧，走到中途就只是半收的样子
        if (panel) panel->setCollapsed(action.flag, false);
        break;
      case SnapshotAction::Kind::kTooltip:
        if (AiUI::View* view = activity.findView(action.viewId.c_str())) {
          root->showTooltipFor(view);
        }
        break;
      case SnapshotAction::Kind::kPresent:
        activity.setPresenting(true);
        break;
      case SnapshotAction::Kind::kWheel: {
        AiUI::MouseEvent e;
        e.position = action.at;
        e.wheelDelta = action.wheelDelta;
        root->onMouseWheel(e);
        break;
      }
      case SnapshotAction::Kind::kDoubleClick: {
        AiUI::MouseEvent e;
        e.position = action.at;
        root->onDoubleClick(e);
        break;
      }
      case SnapshotAction::Kind::kRotateMidway: {
        // 转 90° 只推到一半：进度按设计时长的一半算，落在这里截到的
        // 大约就是 45°，棋盘格溢出最明显的那一刻。
        // 单次定时器还没有排到，这里直接把时钟推过去再触发一次。
        if (AiUI::PhotoView* photo = activity.findView<AiUI::PhotoView>("photo")) {
          photo->setAnimationDuration(200);
          photo->rotate(1);
          AiUI::platform()->sleepMillis(100);
          root->onDelayTimer();
        }
        break;
      }
      default:
        root->sendMouseMove(action.at);
        root->sendMouseDown(action.at);
        root->sendMouseUp(action.at);
        break;
    }
  }

  AiUI::RgbaImage shot;
  if (!root->renderToImage(shot, shotSize)) return 1;
  return AiUI::platform()->saveImagePixels(opts.outPath, shot, AiUI::ImageFormat::kPng) ? 0 : 1;
}

// 图标预览：把全部手绘矢量图标放大画成一张对照图。
// 调图标时用它一眼看完所有造型，不必每次从整窗截图里裁。
// 与快照一样走离屏渲染，不建可见窗口、不碰真实鼠标键盘。
int runIconPreview(const std::wstring& outPath, int cell) {
  if (outPath.empty() || cell <= 0) return 1;

  struct Item {
    AiUI::IconId id;
    const wchar_t* name;
  };
  const Item items[] = {
      {AiUI::IconId::kZoomIn, L"zoomIn"},
      {AiUI::IconId::kZoomOut, L"zoomOut"},
      {AiUI::IconId::kFitWindow, L"fit"},
      {AiUI::IconId::kZoomActual, L"actual"},
      {AiUI::IconId::kRotateLeft, L"rotateLeft"},
      {AiUI::IconId::kRotateRight, L"rotateRight"},
      {AiUI::IconId::kFullscreen, L"present"},
      {AiUI::IconId::kSaveAs, L"saveAs"},
  };
  const int count = static_cast<int>(sizeof(items) / sizeof(items[0]));

  // 每格上方画图标，下方留一条写名字
  const int labelH = cell / 5;
  const AiUI::Size area{cell * count, cell + labelH};
  AiUI::RgbaImage shot;
  const bool ok = AiUI::platform()->renderOffscreen(
      AiUI::Rect{0, 0, area.width, area.height},
      [&](AiUI::Painter& painter) {
        painter.fillRect(AiUI::Rect{0, 0, area.width, area.height},
                         AiUI::makeColor(255, 255, 255, 255));
        // makeColor 的参数顺序是 (r, g, b, a)
        const AiUI::Color ink = AiUI::makeColor(0x20, 0x24, 0x2A, 255);
        for (int i = 0; i < count; ++i) {
          const AiUI::Rect box{i * cell + cell / 8, cell / 8, (i + 1) * cell - cell / 8,
                               cell - cell / 8};
          painter.drawIcon(items[i].id, box, ink);
          painter.drawText(AiUI::Rect{i * cell, cell, (i + 1) * cell, cell + labelH},
                           items[i].name, AiUI::makeColor(0, 0, 0, 255), AiUI::fonts.uiSmall);
        }
      },
      shot);
  if (!ok) return 1;
  return AiUI::platform()->saveImagePixels(outPath, shot, AiUI::ImageFormat::kPng) ? 0 : 1;
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

  // 快照模式要在单实例与可见窗口之前分流：它既不该把路径转给已开的窗口，
  // 也不该去抢前台
  {
    const SnapshotOptions shot = parseSnapshot(AiUI::platform()->commandLineArgs());
    if (shot.enabled) {
      const int code = runSnapshot(shot);
      AiUI::fonts.destroy();
      AiUI::shutdownPlatform();
      return code;
    }
  }

  // 图标预览同样要在单实例之前分流
  {
    std::wstring iconOut;
    int cell = 0;
    for (const std::wstring& arg : AiUI::platform()->commandLineArgs()) {
      std::wstring value;
      if (splitOption(arg, L"--icons", value)) iconOut = value;
      else if (splitOption(arg, L"--cell", value)) isInt(value, cell);
    }
    if (!iconOut.empty()) {
      const int code = runIconPreview(iconOut, cell > 0 ? cell : 160);
      AiUI::fonts.destroy();
      AiUI::shutdownPlatform();
      return code;
    }
  }

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
