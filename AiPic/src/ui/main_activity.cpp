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
#include "AiUI/layouts/linear_layout.h"
#include "AiUI/widgets/photo_view.h"
#include "AiUI/widgets/side_panel.h"
#include "AiUI/widgets/status_bar.h"
#include "AiUI/widgets/thumbnail_strip.h"
#include "AiUI/widgets/title_bar.h"

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

// 工具栏按钮：纯图标、无边框。图标按钮靠悬停提示补回文字含义，
// 没有提示的话一排图标就成了猜谜。
AiUI::ButtonSpec makeIconSpec(const wchar_t* label, AiUI::IconId icon) {
  AiUI::ButtonSpec spec;
  spec.label = label;  // 只用于提示，不画出来
  spec.icon = icon;
  spec.hasIcon = icon != AiUI::IconId::kNone;
  spec.placement = AiUI::IconPlacement::kOnly;
  spec.style = AiUI::ButtonStyle::kSubtle;
  spec.height = 32;
  spec.iconSize = 19;
  spec.padX = 8;
  spec.radius = 6;
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
  thumbs_ = findView<AiUI::ThumbnailStrip>("thumbs");
  sidePanel_ = findView<AiUI::SidePanel>("sidePanel");
  homePane_ = findView<AiUI::View>("homePane");

  attachToolBar();
  attachHome();
  attachPhoto();
  attachStatusBar();
  attachSidePanel();
  attachThumbs();
  attachEdit();

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

  // 八个按钮整组居中：它们只对当前图片动手，横向铺满时靠左会显得偏沉
  toolBar_->setCentered(true);
  // 纯图标工具栏不画分组竖线：一排放下来已经够清楚，多几道线只是噪音
  toolBar_->setGroupDividers(false);

  // 分组按「一组同类的动作」摆：缩放、倍数、旋转各占一组，
  // 另存为单独一组压阵。
  btnZoomIn_ = toolBar_->addButton(makeIconSpec(L"放大", AiUI::IconId::kZoomIn), 0);
  if (btnZoomIn_) {
    btnZoomIn_->setTooltip(L"放大");
    btnZoomIn_->setOnClick([this] { zoomBy(1.25f); });
  }

  btnZoomOut_ = toolBar_->addButton(makeIconSpec(L"缩小", AiUI::IconId::kZoomOut), 0);
  if (btnZoomOut_) {
    btnZoomOut_->setTooltip(L"缩小");
    btnZoomOut_->setOnClick([this] { zoomBy(1.0f / 1.25f); });
  }

  btnFit_ = toolBar_->addButton(makeIconSpec(L"适应窗口", AiUI::IconId::kFitWindow), 1);
  if (btnFit_) {
    btnFit_->setTooltip(L"适应窗口");
    btnFit_->setOnClick([this] { zoomToFit(); });
  }

  btnActual_ = toolBar_->addButton(makeIconSpec(L"实际大小", AiUI::IconId::kZoomActual), 1);
  if (btnActual_) {
    btnActual_->setTooltip(L"实际大小 100%");
    btnActual_->setOnClick([this] { zoomToActual(); });
  }

  // 旋转只留向右一格：两个方向按钮并排时，用户要先想「哪边是左」，
  // 而看图时「转过来」几乎总是同一个方向，连按两下比认两个图标快
  btnRotateRight_ = toolBar_->addButton(makeIconSpec(L"向右旋转", AiUI::IconId::kRotateRight), 2);
  if (btnRotateRight_) {
    btnRotateRight_->setTooltip(L"向右旋转");
    btnRotateRight_->setOnClick([this] { rotate(1); });
  }

  btnPresent_ = toolBar_->addButton(makeIconSpec(L"演示", AiUI::IconId::kFullscreen), 3);
  if (btnPresent_) {
    btnPresent_->setTooltip(L"全屏演示");
    btnPresent_->setOnClick([this] { setPresenting(true); });
  }

  AiUI::ButtonSpec saveSpec = makeIconSpec(L"另存为", AiUI::IconId::kSaveAs);
  btnSaveAs_ = toolBar_->addButton(saveSpec, 4);
  if (btnSaveAs_) {
    btnSaveAs_->setTooltip(L"另存为");
    btnSaveAs_->setOnClick([this] { saveAs(); });
  }

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

void MainActivity::attachEdit() {
  // 编辑入口放在标题栏的行动区：它不属于「对当前图片的查看操作」，
  // 与缩放、旋转那些工具按钮不是一组；放标题栏右侧还紧邻系统按钮，
  // 一眼能看出这是「把图带到别处去」的出口。
  AiUI::Window* win = window();
  AiUI::TitleBar* bar = win ? win->titleBar() : nullptr;
  if (!bar || !bar->actions()) return;

  AiUI::ButtonSpec spec;
  spec.style = AiUI::ButtonStyle::kPrimary;
  spec.label = L"编辑";
  spec.icon = AiUI::IconId::kEdit;
  spec.hasIcon = true;
  spec.iconSize = 14;
  spec.iconGap = 5;
  spec.width = 86;
  spec.height = 26;
  spec.padX = 10;
  spec.radius = 6;

  btnEdit_ = new AiUI::Button();
  btnEdit_->setSpec(spec);
  btnEdit_->setTooltip(L"用编辑器打开这张图片");
  btnEdit_->setOnClick([this] { openInEditor(); });

  // 右边留一点空隙，别贴死系统按钮
  AiUI::LayoutParams params;
  params.width = AiUI::LayoutParams::kWrapContent;
  params.height = AiUI::LayoutParams::kWrapContent;
  params.marginRight = 10;
  params.align(AiUI::Gravity::kCenter, AiUI::Gravity::kCenter);
  bar->actions()->addView(btnEdit_, params);
}

void MainActivity::attachPhoto() {
  if (!photo_) return;
  // 缩放/平移任一变化都要刷状态栏的倍数与像素位置
  photo_->setChangeHandler([this] { refreshStatusBar(); });
  // 放大到像素级时控件会索要可见区域的原图像素来画 RGB 值。
  // 只给它可见那一块，巨图不必整张再解一遍。
  photo_->setPixelRegionProvider(
      [](const AiUI::Rect& region, AiUI::RgbaImage& out) { return AppReadPixelRegion(region, out); });
  // 网格模式下右键复制该像素颜色。文本用 CSS 能直接用的 rgba(...)，
  // 取的就是网格上显示的那三个数，不会出现「看到 86、复制到别的」。
  // 返回要提示的文本；空字符串表示没取到，控件据此不弹提示。
  photo_->setColorCopyHandler([](AiUI::Point pixel) -> std::wstring {
    AiUI::RgbaImage one;
    if (!AppReadPixelRegion(AiUI::Rect{pixel.x, pixel.y, pixel.x + 1, pixel.y + 1}, one) ||
        one.isEmpty()) {
      return std::wstring();
    }
    const AiUI::Color c = one.pixels.front();
    const std::wstring text = L"rgba(" + std::to_wstring(AiUI::colorRed(c)) + L", " +
                              std::to_wstring(AiUI::colorGreen(c)) + L", " +
                              std::to_wstring(AiUI::colorBlue(c)) + L", " +
                              std::to_wstring(AiUI::colorAlpha(c)) + L")";
    if (!AiUI::platform()->setClipboardText(text)) return std::wstring();
    return L"已复制 " + text;
  });
}

void MainActivity::attachStatusBar() {
  if (!status_) return;
  AiUI::StatusStyle style;
  style.padX = 16;
  status_->setStyle(style);
  refreshStatusBar();
}

void MainActivity::attachSidePanel() {
  if (!sidePanel_) return;
  // 展开宽度就用布局里给的那个值：侧栏宽度是布局决定的，不该在两个地方各写一份。
  // 之后宽度只由收缩动画改。
  //
  // 不需要在收缩后手动 zoomToFit：PhotoView 处于「适应窗口」时会在每次
  // 布局变化时自己重算，而用户手动调过的倍数不该因为收个侧栏就被重置。
  const int width = sidePanel_->layoutParams().width;
  if (width > 0) sidePanel_->setExpandedWidth(width);

  // 开关按钮在状态栏最左边（由布局创建）。入口固定在左下角，不管侧栏收没
  // 收都在同一处；侧栏里也就不必再留一条只为放按钮的栏。
  panelToggle_ = findView<AiUI::PanelToggle>("panelToggle");
  if (!panelToggle_) return;
  // 箭头直接跟侧栏状态走，不必在每条改动路径上都手动同步
  panelToggle_->setCollapsedSource([this] { return sidePanel_ && sidePanel_->isCollapsed(); });
  panelToggle_->setOnToggle([this] {
    if (!sidePanel_) return;
    sidePanel_->setCollapsed(!sidePanel_->isCollapsed());
    // 动画只改宽度不重画按钮，这里主动刷一次让箭头立刻掉头
    if (panelToggle_) panelToggle_->invalidate();
  });
}

void MainActivity::attachThumbs() {
  if (!thumbs_) return;
  // 点缩略图等同于翻到那一张：走同一条 openImage 路径，状态栏与标题才会一起更新
  thumbs_->setOnSelect([this](int index) {
    if (presenting_) return;
    const auto& siblings = AppSiblingImages();
    if (index < 0 || index >= static_cast<int>(siblings.size())) return;
    if (index == AppSiblingIndex()) return;
    openImage(siblings[static_cast<size_t>(index)]);
  });
}

// ───────────────────────── 状态同步 ─────────────────────────

void MainActivity::refreshStage() {
  const bool has = AppHasImage();
  if (homePane_) homePane_->show(!has);
  if (photo_) photo_->show(has);
  // 侧栏只在有图、且不在演示态时有意义：空状态页与投影都要独占整块宽度。
  // 翻页会走这里，所以演示判断不能只放在 setPresenting 里。
  // 开关按钮跟着一起隐：侧栏都不在，按钮点了也没有可见结果。
  const bool showPanel = has && !presenting_;
  if (sidePanel_) sidePanel_->show(showPanel);
  if (panelToggle_) panelToggle_->show(showPanel);
}

void MainActivity::refreshThumbs() {
  if (!thumbs_) return;
  thumbs_->setItems(AppSiblingImages());
  thumbs_->setSelected(AppSiblingIndex());
  thumbs_->scrollToItem(AppSiblingIndex());
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
  // 分辨率、文件大小、第几张：三样都是「关于这张图」的规格，跟格式标签
  // 并排成胶囊比塞在标题栏更合适——标题栏本来就不该堆信息。
  fields.badges.push_back(std::to_wstring(size.width) + L"×" + std::to_wstring(size.height));

  // 文件大小：读不到就不给这个胶囊，比显示 0 B 诚实
  const uint64_t bytes = AiUI::platform()->fileSize(path);
  if (bytes > 0) fields.badges.push_back(util::FormatBytes(bytes));

  // 第几张 / 共几张：只在同目录有多张时才有意义
  const int count = static_cast<int>(AppSiblingImages().size());
  const int index = AppSiblingIndex();
  if (count > 1 && index >= 0) {
    fields.badges.push_back(std::to_wstring(index + 1) + L"/" + std::to_wstring(count));
  }

  std::wstring right;
  if (photo_) {
    right = util::FormatZoom(photo_->zoom());
    // 像素坐标放状态栏右侧：放大到像素级时用户要能报出「我看的是哪个点」。
    //
    // 优先用指针下的像素——用户问的是「我指着的是哪个」，不是一个跟平移
    // 自己跑的数字。指针移出图外（或还没进过控件）才退回视图中心，那时
    // 中心那个数是唯一有意义的「当前看着哪儿」。
    AiUI::Point pixel;
    if (!photo_->mousePixel(pixel)) pixel = photo_->centerPixel();
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
       {btnZoomIn_, btnZoomOut_, btnFit_, btnActual_, btnRotateRight_, btnPresent_,
        btnSaveAs_, btnEdit_}) {
    if (button) button->setEnabled(has);
  }
}

void MainActivity::updateTitle() {
  AiUI::Window* win = window();
  AiUI::TitleBar* bar = win ? win->titleBar() : nullptr;

  if (!AppHasImage()) {
    setTitle(L"爱图");
    // 无图时标题就是应用名，不该再被当成文件名截断
    if (bar) bar->setTitleMiddleEllipsis(false);
    return;
  }
  // 标题只放文件名：分辨率、大小、序号都在状态栏，重复写反而挤
  setTitle(util::BaseName(AppImagePath()));
  // 文件名必须中段省略：尾部省略会把扩展名裁掉，而那正是要认的信息
  if (bar) bar->setTitleMiddleEllipsis(true);
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
  // loadFile 会复位棋盘格状态（新图的 alpha 未知），这里补上
  photo_->setImageHasAlpha(AppImageHasAlpha());

  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  refreshThumbs();
  updateTitle();
  // 演示态下打开新图同样要铺满：loadFile 默认最多 100%
  if (presenting_ && photo_) photo_->zoomToFit();
  return true;
}

void MainActivity::closeImage() {
  // 演示态下关图会把界面留在「没有图」的演示布局里，先退出来
  if (presenting_) setPresenting(false);
  AppCloseImage();
  if (photo_) photo_->clearImage();
  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  refreshThumbs();
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
  // 无论前进还是回退，位图和像素都已换成当前这张，棋盘格状态要跟上
  if (photo_) photo_->setImageHasAlpha(AppImageHasAlpha());
  // 演示态下换图也要铺满屏幕：loadFile 会回到「最多 100%」
  if (presenting_ && photo_) photo_->zoomToFit();
  refreshStage();
  refreshStatusBar();
  refreshToolBar();
  refreshThumbs();
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

// 编辑器是同一目录下的另一个 exe。把当前图的路径传过去，
// 用户在编辑器里改的是「这张图」，不用再选一次文件。
void MainActivity::openInEditor() {
  if (!AppHasImage() || AppImagePath().empty()) return;

  const std::wstring self = AiUI::platform()->executablePath();
  if (self.empty()) return;

  // 同目录：把文件名换掉即可，不猜安装位置
  std::wstring dir = util::DirName(self);
  if (dir.empty()) return;
  const std::wstring editorPath = util::JoinPath(dir, L"AiPicEditor.exe");

  if (!util::PathExists(editorPath)) {
    warn(L"找不到编辑器：\n" + editorPath + L"\n\n请确认 AiPicEditor.exe 与看图程序在同一目录。",
         nativeWindow());
    return;
  }

  // 参数要加引号：路径里带空格时不然会被拆成两段
  const std::wstring args = L"\"" + AppImagePath() + L"\"";
  if (!AiUI::platform()->launchDetached(editorPath, args, dir)) {
    warn(L"启动编辑器失败：\n" + editorPath, nativeWindow());
  }
}

void MainActivity::zoomBy(float factor) {
  // 演示态锁定画面：倍数一变，投影出去的那一帧就跟用户讲的对不上了
  if (presenting_) return;
  if (photo_ && AppHasImage()) photo_->zoomBy(factor);
}

void MainActivity::zoomToFit() {
  if (presenting_) return;
  if (photo_ && AppHasImage()) photo_->zoomToFit();
}

void MainActivity::zoomToActual() {
  if (presenting_) return;
  if (photo_ && AppHasImage()) photo_->zoomToActual();
}

void MainActivity::rotate(int quarterTurns) {
  if (presenting_ || !AppHasImage()) return;
  // 旋转要记在 App 上：它会改变另存出去的内容。PhotoView 的那份只管绘制，
  // 两边必须同步转，否则用户看到的方向和存下来的不一致。
  AppSetRotation(AppRotation() + quarterTurns);
  if (photo_) photo_->rotate(quarterTurns);
  refreshStatusBar();
}

void MainActivity::saveAs() {
  // 演示态不该弹保存框：投影出去的画面里出现文件对话框是明显的失误
  if (presenting_ || !AppHasImage()) return;

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

void MainActivity::setPresenting(bool presenting) {
  if (presenting_ == presenting) return;
  if (presenting && !AppHasImage()) return;
  presenting_ = presenting;

  // 演示态只留图片：工具栏、缩略图条与状态栏都是「编辑时」的辅助，
  // 投影出去只会占掉画面。退出时按各控件本身的状态恢复。
  if (toolBar_) toolBar_->show(!presenting);
  if (status_) status_->show(!presenting);
  // 编辑按钮在标题栏里，演示态虽然会收起标题栏，但按钮自身状态也要复位，
  // 否则退出演示后可能与预期不一致
  if (btnEdit_) btnEdit_->show(!presenting);
  if (sidePanel_) sidePanel_->show(!presenting && AppHasImage());
  if (panelToggle_) panelToggle_->show(!presenting && AppHasImage());
  if (homePane_) homePane_->show(!AppHasImage());

  // 标题栏与系统边框一起收起：留着标题栏就不是「全屏」了
  if (AiUI::Window* win = window()) {
    if (presenting) {
      titleBarVisible_ = win->isTitleBarVisible();
      win->setTitleBarVisible(false);
    } else {
      win->setTitleBarVisible(titleBarVisible_);
    }
    if (AiUI::PlatformWindow* pw = win->platformWindow()) pw->setFullscreen(presenting);
  }

  // 演示态允许放大到铺满：投影时观众离得远，宁可糊一点也要占满屏幕；
  // 退出时恢复「最多 100%」，否则小图会在正常窗口里被撑大。
  if (photo_) {
    photo_->setFitAllowsUpscale(presenting);
    if (presenting) {
      // 先拍快照再适应：退出演示要回到用户调好的倍数与位置
      savedView_ = photo_->saveViewState();
      // 换过可用区域就重新适应一次：演示时整屏都是画布，用户要看最大的图
      photo_->zoomToFit();
      // 适应之后再锁：顺序反了的话这次适应会开始一段动画，
      // 锁定会把它直接落位，投影上就闪一下中途的倍数
      photo_->setInteractive(false);
    } else {
      photo_->setInteractive(true);
      photo_->restoreViewState(savedView_);
    }
  }
}

// ───────────────────────── 窗口级行为 ─────────────────────────

bool MainActivity::onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) {
  if (mods.ctrl) {
    switch (key) {
      case AiUI::Key::kO:
        // 演示态下打开新图等于换掉投影内容，还会在画面上弹文件对话框
        if (presenting_) return false;
        pickAndOpen();
        return true;
      case AiUI::Key::kS:
        if (presenting_) return false;
        saveAs();
        return true;
      default:
        return false;
    }
  }

  // 翻页用左右键：这与资源管理器的方向一致，不必记任何修饰键
  switch (key) {
    case AiUI::Key::kLeft:
    case AiUI::Key::kUp:
    case AiUI::Key::kPageUp:
      if (!AppHasImage()) return false;
      stepImage(-1);
      return true;
    case AiUI::Key::kRight:
    case AiUI::Key::kDown:
    case AiUI::Key::kPageDown:
      if (!AppHasImage()) return false;
      stepImage(1);
      return true;
    case AiUI::Key::kF5:
      if (!AppHasImage()) return false;
      setPresenting(true);
      return true;
    case AiUI::Key::kEscape:
      // Esc 只用于退出演示；正常窗口下不抢这个键（系统可能留给别的用途）
      if (!presenting_) return false;
      setPresenting(false);
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
