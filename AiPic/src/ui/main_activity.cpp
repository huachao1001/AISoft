// 主界面 Activity 实现：装配控件、响应交互、窗口级行为
#include "main_activity.h"

#include "app.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"
#include "AiUI/theme_registry.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/button_bar.h"
#include "AiUI/widgets/image_view.h"
#include "AiUI/widgets/photo_view.h"
#include "AiUI/widgets/status_bar.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ui {
namespace {

MainActivity* g_activity = nullptr;

void warn(const std::wstring& text, AiUI::NativeHandle owner) {
  AiUI::MessageBoxParams params;
  params.title = L"爱图";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kWarning;
  AiUI::platform()->showMessageBox(owner, params);
}

AiUI::ButtonSpec makeButtonSpec(const wchar_t* label, AiUI::IconId icon) {
  AiUI::ButtonSpec spec;
  spec.label = label;
  spec.icon = icon;
  spec.hasIcon = true;
  spec.placement = AiUI::IconPlacement::kLeft;
  spec.style = AiUI::ButtonStyle::kOutline;
  spec.height = 32;
  spec.iconSize = 18;
  spec.padX = 12;
  spec.iconGap = 6;
  spec.radius = 7;
  return spec;
}

// 另存的默认格式跟着源文件走：把 .jpg 另存成 .png 是用户没要求过的转换，
// 而 Windows 的保存框还会按「保存类型」把文件名后缀改掉，看上去像丢了原格式。
AiUI::ImageFormat formatForPath(const std::wstring& path) {
  const std::wstring ext = util::FileExtension(path);
  if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe") return AiUI::ImageFormat::kJpeg;
  if (ext == L".bmp") return AiUI::ImageFormat::kBmp;
  return AiUI::ImageFormat::kPng;
}

// 格式 → 对话框用的（后缀、显示名、通配符）
struct FormatDesc {
  const wchar_t* ext;
  const wchar_t* label;
  const wchar_t* pattern;
};

FormatDesc describe(AiUI::ImageFormat format) {
  switch (format) {
    case AiUI::ImageFormat::kJpeg: return {L"jpg", L"JPEG 图片 (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"};
    case AiUI::ImageFormat::kBmp: return {L"bmp", L"BMP 图片 (*.bmp)", L"*.bmp"};
    default: return {L"png", L"PNG 图片 (*.png)", L"*.png"};
  }
}

// 保存框的过滤器：默认格式排第一，PNG / JPEG / BMP 固定顺序跟在后面。
// 顺序固定才能让「另存为」每次的列表位置一致；把默认项提到第一只是为了
// 打开对话框时选中的就是用户要存的那种。
std::vector<std::wstring> saveFilters(AiUI::ImageFormat preferred) {
  std::vector<AiUI::ImageFormat> order = {AiUI::ImageFormat::kPng, AiUI::ImageFormat::kJpeg,
                                          AiUI::ImageFormat::kBmp};
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] != preferred) continue;
    std::swap(order[0], order[i]);
    break;
  }

  std::vector<std::wstring> filters;
  filters.reserve(order.size() * 2);
  for (AiUI::ImageFormat format : order) {
    const FormatDesc desc = describe(format);
    filters.push_back(desc.label);
    filters.push_back(desc.pattern);
  }
  return filters;
}

}  // namespace

MainActivity::MainActivity() {
  AiUI::WindowStyle style;
  style.topLevel = true;
  style.resizable = true;
  style.minimizable = true;
  style.maximizable = true;
  style.captionHeight = 0;
  setStyle(style);
  setMinSize(kMinWidth, kMinHeight);
  setTitle(L"爱图");
  // 主窗口不响应 Esc
  setEscapeCloses(false);
  // 主题名来自设置（themes.xml 里的 name）；查不到就落到浅色
  setTheme(&AiUI::ThemeRegistry::getOr(g_app.settings.theme, AiUI::kLightTheme));
}

MainActivity::~MainActivity() {
  if (g_activity == this) g_activity = nullptr;
}

// ───────────────────────── 装配 ─────────────────────────

void MainActivity::onCreate() {
  g_activity = this;

  toolBar_ = findView<AiUI::ButtonBar>("toolBar");
  photo_ = findView<AiUI::PhotoView>("photo");
  status_ = findView<AiUI::StatusBar>("status");
  homePane_ = findView<AiUI::View>("homePane");

  attachToolBar();
  attachHome();
  attachPhoto();
  attachStatusBar();

  // 拖放与单实例转发挂在 AiUI 建的顶层窗口上
  g_app.mainWnd = nativeWindow();
  AttachHostHooks();

  refreshStage();
  refreshStatusBar();
  refreshToolBar();
}

AiUI::NativeHandle MainActivity::nativeWindow() const {
  return window() ? window()->handle() : nullptr;
}

void MainActivity::attachToolBar() {
  if (!toolBar_) return;

  // 分组按「一组同类的动作」摆：缩放、倍数、旋转各占一组，
  // 另存为单独一组并靠右的视觉重量收尾。
  btnZoomIn_ = toolBar_->addButton(makeButtonSpec(L"放大", AiUI::IconId::kZoomIn), 0);
  if (btnZoomIn_) btnZoomIn_->setOnClick([this] { zoomBy(1.25f); });

  btnZoomOut_ = toolBar_->addButton(makeButtonSpec(L"缩小", AiUI::IconId::kZoomOut), 0);
  if (btnZoomOut_) btnZoomOut_->setOnClick([this] { zoomBy(1.0f / 1.25f); });

  btnFit_ = toolBar_->addButton(makeButtonSpec(L"适应窗口", AiUI::IconId::kFitWindow), 1);
  if (btnFit_) btnFit_->setOnClick([this] { zoomToFit(); });

  btnActual_ = toolBar_->addButton(makeButtonSpec(L"100%", AiUI::IconId::kNone), 1);
  if (btnActual_) btnActual_->setOnClick([this] { zoomToActual(); });

  btnRotateLeft_ = toolBar_->addButton(makeButtonSpec(L"左转", AiUI::IconId::kRotateLeft), 2);
  if (btnRotateLeft_) btnRotateLeft_->setOnClick([this] { rotate(-1); });

  btnRotateRight_ = toolBar_->addButton(makeButtonSpec(L"右转", AiUI::IconId::kRotateRight), 2);
  if (btnRotateRight_) btnRotateRight_->setOnClick([this] { rotate(1); });

  AiUI::ButtonSpec saveSpec = makeButtonSpec(L"另存为", AiUI::IconId::kSave);
  saveSpec.style = AiUI::ButtonStyle::kPrimary;
  btnSaveAs_ = toolBar_->addButton(saveSpec, 3);
  if (btnSaveAs_) btnSaveAs_->setOnClick([this] { saveAs(); });

  refreshToolBar();
}

void MainActivity::attachHome() {
  if (auto* open = findView<AiUI::Button>("btnHomeOpen")) {
    open->setOnClick([this] { pickAndOpen(); });
  }

  if (auto* icon = findView<AiUI::ImageView>("homeIcon")) {
    // 颜色取主题 accent 再往 body 收一点：全强度 accent 在 96px 的大面积上
    // 太扎眼，和主按钮抢注意力；收一点既点出「这里是重点」，又不会喧宾夺主。
    // 必须在这里算而不是写 XML 的 tint——tint 只认十六进制固定色，深色主题
    // 下会变成一个和背景打架的颜色。
    const AiUI::Theme* t = appTheme() ? appTheme() : AiUI::Activity::defaultThemePtr();
    icon->setIcon(AiUI::IconId::kImage);
    if (t) icon->setPlaceholderColor(AiUI::blendColor(t->accent, t->body, 0.28f));
  }
}

void MainActivity::attachPhoto() {
  if (!photo_) return;
  // 缩放/平移任一变化都要刷状态栏的倍数与像素位置
  photo_->setChangeHandler([this] { refreshStatusBar(); });
  // 放大到像素级时控件会索要可见区域的原图像素来画 RGB 值。
  // 只给它可见那一块，巨图不必整张再解一遍。
  photo_->setPixelRegionProvider(
      [](const AiUI::Rect& region, AiUI::RgbaImage& out) { return AppReadPixelRegion(region, out); });
}

void MainActivity::attachStatusBar() {
  if (!status_) return;
  AiUI::StatusStyle style;
  style.padX = 16;
  status_->setStyle(style);
  refreshStatusBar();
}

// ───────────────────────── 状态同步 ─────────────────────────

void MainActivity::refreshStage() {
  const bool has = AppHasImage();
  if (homePane_) homePane_->show(!has);
  if (photo_) photo_->show(has);
}

void MainActivity::refreshStatusBar() {
  if (!status_) return;

  AiUI::StatusFields fields;
  if (!AppHasImage()) {
    fields.left = L"没有打开图片";
    fields.right = L"拖入图片或用 Ctrl+O 打开";
    status_->setFields(fields);
    return;
  }

  const std::wstring& path = AppImagePath();
  const AiUI::Size size = AppImageSize();
  fields.left = path;
  fields.tag = util::FormatLabel(path);
  // 尺寸用乘号而不是「x」：和状态栏里其它数字排在一起更像规格
  fields.size = std::to_wstring(size.width) + L" × " + std::to_wstring(size.height);

  std::wstring right;
  if (photo_) {
    right = util::FormatZoom(photo_->zoom());
    // 像素坐标放状态栏右侧：放大到像素级时用户要能报出「我看的是哪个点」
    const AiUI::Point pixel = photo_->centerPixel();
    if (pixel.x >= 0 && pixel.y >= 0) {
      right += L"  ·  " + std::to_wstring(pixel.x) + L"," + std::to_wstring(pixel.y);
    }
  }
  if (const int turns = AppRotation(); turns != 0) {
    if (!right.empty()) right += L"  ·  ";
    right += L"旋转 " + std::to_wstring(turns * 90) + L"°";
  }
  fields.right = right;
  status_->setFields(fields);
}

void MainActivity::refreshToolBar() {
  const bool has = AppHasImage();
  for (AiUI::Button* button :
       {btnZoomIn_, btnZoomOut_, btnFit_, btnActual_, btnRotateLeft_, btnRotateRight_,
        btnSaveAs_}) {
    if (button) button->setEnabled(has);
  }
}

void MainActivity::updateTitle() {
  if (!AppHasImage()) {
    setTitle(L"爱图");
    return;
  }
  setTitle(util::BaseName(AppImagePath()) + L" — 爱图");
}

// ───────────────────────── 动作 ─────────────────────────

bool MainActivity::openImage(const std::wstring& path) {
  // 两处解码是刻意的：App 要像素（另存为、旋转），PhotoView 要位图（绘制）。
  // 平台没有「位图取像素」的入口，合成一个解码器只会把两边的生命周期绑死。
  if (!AppOpenImage(path)) return false;
  if (!photo_ || !photo_->loadFile(AppImagePath())) {
    // 位图没能加载：像素留着也用不上，退回空状态比显示一半更诚实
    AppCloseImage();
    return false;
  }

  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  updateTitle();
  return true;
}

void MainActivity::closeImage() {
  AppCloseImage();
  if (photo_) photo_->clearImage();
  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  updateTitle();
}

void MainActivity::stepImage(int delta) {
  if (!AppHasImage() || delta == 0) return;
  const std::wstring previous = AppImagePath();
  if (!AppStepImage(delta)) return;
  if (!photo_ || !photo_->loadFile(AppImagePath())) {
    // 新的一张解不出来：退回原来那张，别把用户正在看的图弄丢
    AppOpenImage(previous);
    if (photo_) photo_->loadFile(previous);
  }
  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  updateTitle();
}

void MainActivity::pickAndOpen() {
  // 起点用上次打开/另存的目录：这是用户最可能放图的地方
  const std::wstring start = g_app.settings.lastDir.empty()
                                 ? AiUI::platform()->specialFolder(AiUI::SpecialFolder::kHome)
                                 : g_app.settings.lastDir;
  const std::wstring picked = AiUI::platform()->pickOpenFile(
      nativeWindow(), L"打开图片", start,
      {L"图片 (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp)",
       L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp", L"所有文件 (*.*)", L"*.*"});
  if (picked.empty()) return;
  if (!openImage(picked)) warn(L"打不开这张图片：\n" + picked, nativeWindow());
}

void MainActivity::zoomBy(float factor) {
  if (photo_ && AppHasImage()) photo_->zoomBy(factor);
}

void MainActivity::zoomToFit() {
  if (photo_ && AppHasImage()) photo_->zoomToFit();
}

void MainActivity::zoomToActual() {
  if (photo_ && AppHasImage()) photo_->zoomToActual();
}

void MainActivity::rotate(int quarterTurns) {
  if (!AppHasImage()) return;
  // 旋转要记在 App 上：它会改变另存出去的内容。PhotoView 的那份只管绘制，
  // 两边必须同步转，否则用户看到的方向和存下来的不一致。
  AppSetRotation(AppRotation() + quarterTurns);
  if (photo_) photo_->rotate(quarterTurns);
  refreshStatusBar();
}

void MainActivity::saveAs() {
  if (!AppHasImage()) return;

  const std::wstring source = AppImagePath();
  const std::wstring dir =
      g_app.settings.lastDir.empty() ? util::DirName(source) : g_app.settings.lastDir;

  // 默认格式跟源文件走：把 jpg 另存成 png 是用户没要求过的转换。
  // 源后缀本身不可写（gif/webp 这类）时才退回 PNG。
  const AiUI::ImageFormat preferred = formatForPath(source);
  const FormatDesc desc = describe(preferred);
  const std::wstring defaultName = util::FileStem(source) + L"." + desc.ext;

  const std::wstring target = AiUI::platform()->pickSaveFile(
      nativeWindow(), L"另存为", dir, defaultName, desc.ext, saveFilters(preferred));
  if (target.empty()) return;

  const AiUI::RgbaImage pixels = AppExportPixels();
  if (pixels.isEmpty()) {
    warn(L"当前图片没有可保存的像素。", nativeWindow());
    return;
  }
  // 最终格式按用户实际给的后缀定：他可能手动把名字改成了别的后缀
  if (!AiUI::platform()->saveImagePixels(target, pixels, formatForPath(target))) {
    warn(L"保存失败：\n" + target, nativeWindow());
    return;
  }

  g_app.settings.lastDir = util::DirName(util::AbsolutePath(target));
}

// ───────────────────────── 窗口级行为 ─────────────────────────

bool MainActivity::onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) {
  if (mods.ctrl) {
    switch (key) {
      case AiUI::Key::kO:
        pickAndOpen();
        return true;
      case AiUI::Key::kS:
        saveAs();
        return true;
      default:
        return false;
    }
  }

  // 翻页用左右键：这与资源管理器的方向一致，不必记任何修饰键
  switch (key) {
    case AiUI::Key::kLeft:
      if (!AppHasImage()) return false;
      stepImage(-1);
      return true;
    case AiUI::Key::kRight:
      if (!AppHasImage()) return false;
      stepImage(1);
      return true;
    // 键位映射只到数字键：+/- 在键盘上是 OEM 键，后端不转成 Key，
    // 所以整理成「0 整图、1 原大」这两个人人记得住的数字。
    case AiUI::Key::k0:
      if (!AppHasImage()) return false;
      zoomToFit();
      return true;
    case AiUI::Key::k1:
      if (!AppHasImage()) return false;
      zoomToActual();
      return true;
    default:
      return false;
  }
}

bool MainActivity::onCloseRequest() {
  // 关闭前保存窗口状态；正常尺寸取还原态，最大化时也记下来
  if (AiUI::Window* win = window()) {
    if (AiUI::PlatformWindow* pw = win->platformWindow()) {
      g_app.settings.winMax = pw->isMaximized();
      if (!g_app.settings.winMax) {
        const AiUI::Rect rc = pw->normalBounds();
        g_app.settings.winW = rc.right - rc.left;
        g_app.settings.winH = rc.bottom - rc.top;
      }
    }
  }
  util::SaveSettings(g_app.settings);
  return true;
}

MainActivity* mainActivity() { return g_activity; }

}  // namespace ui
