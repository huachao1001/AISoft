// 编辑器主界面实现
#include "editor_activity.h"

#include "editor/pixels.h"
#include "util.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"
#include "AiUI/view_group.h"
#include "AiUI/widgets/button.h"
#include "AiUI/layouts/linear_layout.h"
#include "AiUI/widgets/slider.h"
#include "AiUI/widgets/text_view.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

AiUI::Platform* platform() { return AiUI::platform(); }

// 面板里的行高与间距：96 DPI 设计值，实际按 dpi() 换算
constexpr int kRowGap = 14;
constexpr int kLabelHeight = 22;

// 数值转文本：滑块是 0..1 的比例，显示时换成用户能懂的量纲
std::wstring percentText(float v) {
  return std::to_wstring(static_cast<int>(std::lround(v * 100.0f))) + L"%";
}

// 扫一遍 alpha：绘制阶段靠它决定铺不铺棋盘格。全不透明是绝大多数情况，
// 扫到第一个半透明像素就停。
bool hasAlpha(const AiUI::RgbaImage& image) {
  for (AiUI::Color px : image.pixels) {
    if (AiUI::colorAlpha(px) < 255) return true;
  }
  return false;
}

}  // namespace

EditorActivity::EditorActivity() = default;
EditorActivity::~EditorActivity() = default;

// ───────────────────────── 装配 ─────────────────────────

void EditorActivity::onCreate() {
  photo_ = findView<AiUI::PhotoView>("photo");
  homePane_ = findView<AiUI::View>("homePane");
  toolTabs_ = findView<AiUI::LinearLayout>("toolTabs");
  panelBody_ = findView<AiUI::LinearLayout>("panelBody");
  btnApply_ = findView<AiUI::Button>("btnApply");
  btnReset_ = findView<AiUI::Button>("btnReset");
  btnHomeOpen_ = findView<AiUI::Button>("btnHomeOpen");

  attachTabs();
  attachActions();
  attachHome();

  setTitle(L"爱图编辑器");
  setMinSize(880, 560);

  refreshStage();
  rebuildPanel();
}

void EditorActivity::attachTabs() {
  if (!toolTabs_) return;

  struct TabDef {
    Tool tool;
    const wchar_t* label;
  };
  const TabDef defs[] = {{Tool::kCrop, L"裁剪"}, {Tool::kResize, L"缩放"}};

  tabs_.clear();
  for (const TabDef& def : defs) {
    AiUI::ButtonSpec spec;
    spec.label = def.label;
    spec.style = AiUI::ButtonStyle::kSubtle;
    spec.height = 30;
    auto* button = new AiUI::Button();
    button->setSpec(spec);
    const Tool tool = def.tool;
    button->setOnClick([this, tool] { setTool(tool); });
    // 标签按权重平分宽度：写死像素数时面板一变窄就会被挤出去
    AiUI::LayoutParams params;
    params.width = 0;
    params.height = AiUI::LayoutParams::kWrapContent;
    params.weight = 1.0f;
    toolTabs_->addView(button, params);
    tabs_.push_back(button);
  }
}

void EditorActivity::attachActions() {
  if (btnApply_) btnApply_->setOnClick([this] { applyTool(); });
  if (btnReset_) btnReset_->setOnClick([this] { resetTool(); });
}

void EditorActivity::attachHome() {
  if (btnHomeOpen_) btnHomeOpen_->setOnClick([this] { pickAndOpen(); });
}

// ───────────────────────── 打开 / 保存 ─────────────────────────

bool EditorActivity::openImage(const std::wstring& path) {
  if (path.empty() || !util::PathExists(path)) return false;

  AiUI::RgbaImage pixels;
  if (!platform()->loadImagePixels(path, pixels)) return false;

  original_ = std::move(pixels);
  pixels_ = original_;
  path_ = util::AbsolutePath(path);

  // 换图后裁剪框回到全图：上一张的比例套到新图上通常不是用户想要的
  cropLeft_ = 0.0f;
  cropTop_ = 0.0f;
  cropRight_ = 1.0f;
  cropBottom_ = 1.0f;
  resizeWidth_ = original_.width;
  resizeHeight_ = original_.height;

  presentPixels(pixels_);
  refreshStage();
  rebuildPanel();
  return true;
}

void EditorActivity::closeImage() {
  original_ = AiUI::RgbaImage{};
  pixels_ = AiUI::RgbaImage{};
  path_.clear();
  presentPixels(AiUI::RgbaImage{});
  refreshStage();
  rebuildPanel();
}

void EditorActivity::saveAs() {
  if (pixels_.isEmpty()) return;

  const std::wstring target =
      platform()->pickSaveFile(nativeWindow(), L"另存为", util::DirName(path_),
                               L"edited.png", L"png",
                               {L"图片", L"*.png;*.jpg;*.jpeg;*.bmp"});
  if (target.empty()) return;

  // 后缀决定编码格式；不认识的一律按 PNG 存，不静默丢内容
  const std::wstring ext = util::FileExtension(target);
  const AiUI::ImageFormat format = ext == L".jpg" || ext == L".jpeg"
                                       ? AiUI::ImageFormat::kJpeg
                                       : (ext == L".bmp" ? AiUI::ImageFormat::kBmp
                                                         : AiUI::ImageFormat::kPng);
  platform()->saveImagePixels(target, pixels_, format);
}

// ───────────────────────── 工具切换 ─────────────────────────

void EditorActivity::setTool(Tool tool) {
  if (tool_ == tool) return;
  tool_ = tool;
  rebuildPanel();
}

void EditorActivity::rebuildPanel() {
  if (!panelBody_) return;

  // 标签高亮：选中的那个换成主按钮样式
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const bool active = static_cast<int>(i) == static_cast<int>(tool_);
    AiUI::ButtonSpec spec = tabs_[i]->spec();
    spec.style = active ? AiUI::ButtonStyle::kPrimary : AiUI::ButtonStyle::kSubtle;
    tabs_[i]->setSpec(spec);
  }

  panelBody_->removeAllViews();
  if (pixels_.isEmpty()) {
    addLabel(L"还没有打开图片", true, panelBody_);
    refreshActions();
    return;
  }

  switch (tool_) {
    case Tool::kCrop: buildCropPanel(); break;
    case Tool::kResize: buildResizePanel(); break;
  }
  refreshActions();
}

void EditorActivity::buildCropPanel() {
  addLabel(L"拖动滑块调整裁切范围", true, panelBody_);

  // 四条边各一个滑块：比例值直接映射到图像坐标
  auto edge = [this](const wchar_t* label, float initial, float& target, bool isLeft) {
    AiUI::Slider* slider = addSliderRow(label, initial, percentText(initial), panelBody_);
    if (!slider) return;
    float* ref = &target;
    slider->setOnChange([this, ref, isLeft](float v) {
      // 左右边不能交叉：左/上边最多推到对边前 1%，否则裁出空图
      if (isLeft) {
        *ref = (std::min)(v, (isLeft ? cropRight_ : cropBottom_) - 0.01f);
      } else {
        *ref = (std::max)(v, (isLeft ? cropLeft_ : cropTop_) + 0.01f);
      }
      refreshPreview();
    });
  };

  edge(L"左边", cropLeft_, cropLeft_, true);
  edge(L"上边", cropTop_, cropTop_, true);
  edge(L"右边", cropRight_, cropRight_, false);
  edge(L"下边", cropBottom_, cropBottom_, false);

  // 当前裁切后的尺寸，给用户一个明确的手感
  const int w = static_cast<int>(std::lround((cropRight_ - cropLeft_) * pixels_.width));
  const int h = static_cast<int>(std::lround((cropBottom_ - cropTop_) * pixels_.height));
  addLabel(L"结果：" + std::to_wstring((std::max)(1, w)) + L" × " +
               std::to_wstring((std::max)(1, h)),
           true, panelBody_);
}void EditorActivity::buildResizePanel() {
  addLabel(L"目标宽高（像素）", true, panelBody_);

  // 用两个滑块表示宽高比例，范围取当前尺寸的 10%..200%：
  // 缩放到两倍以上几乎总是误操作，上限收着点更好拖。
  // 基准取 pixels_（当前待处理的图）而不是 original_：裁剪过之后两者尺寸不同。
  const int baseW = (std::max)(1, pixels_.width);
  const int baseH = (std::max)(1, pixels_.height);
  auto axis = [this](const wchar_t* label, float initial, int& target, int base) {
    AiUI::Slider* slider = addSliderRow(label, initial, std::to_wstring(target), panelBody_);
    if (!slider) return;
    int* ref = &target;
    const int src = base;
    slider->setOnChange([this, ref, src](float v) {
      const float scale = 0.1f + v * 1.9f;  // 10%..200%
      *ref = (std::max)(1, static_cast<int>(std::lround(src * scale)));
      refreshPreview();
    });
  };

  const float wv = (static_cast<float>(resizeWidth_) / baseW - 0.1f) / 1.9f;
  const float hv = (static_cast<float>(resizeHeight_) / baseH - 0.1f) / 1.9f;
  axis(L"宽度", (std::max)(0.0f, (std::min)(1.0f, wv)), resizeWidth_, baseW);
  axis(L"高度", (std::max)(0.0f, (std::min)(1.0f, hv)), resizeHeight_, baseH);

  addLabel(L"结果：" + std::to_wstring((std::max)(1, resizeWidth_)) + L" × " +
               std::to_wstring((std::max)(1, resizeHeight_)),
           true, panelBody_);
}

AiUI::TextView* EditorActivity::addLabel(const std::wstring& text, bool muted,
                                          AiUI::LinearLayout* body) {
  auto* label = new AiUI::TextView();
  label->setText(text);
  AiUI::LayoutParams params;
  params.width = AiUI::LayoutParams::kMatchParent;
  params.height = kLabelHeight * dpi() / 96;
  params.marginBottom = 6 * dpi() / 96;
  body->addView(label, params);
  (void)muted;
  return label;
}

AiUI::Slider* EditorActivity::addSliderRow(const std::wstring& label, float value,
                                            const std::wstring& valueText,
                                            AiUI::LinearLayout* body) {
  auto* row = new AiUI::LinearLayout();
  row->setOrientation(AiUI::Orientation::kVertical);
  AiUI::LayoutParams rowParams;
  rowParams.width = AiUI::LayoutParams::kMatchParent;
  rowParams.height = AiUI::LayoutParams::kWrapContent;
  rowParams.marginBottom = kRowGap * dpi() / 96;
  body->addView(row, rowParams);

  auto* caption = new AiUI::TextView();
  caption->setText(label + L"  " + valueText);
  AiUI::LayoutParams capParams;
  capParams.width = AiUI::LayoutParams::kMatchParent;
  capParams.height = kLabelHeight * dpi() / 96;
  row->addView(caption, capParams);

  auto* slider = new AiUI::Slider();
  slider->setValue(value);
  AiUI::LayoutParams sliderParams;
  sliderParams.width = AiUI::LayoutParams::kMatchParent;
  sliderParams.height = AiUI::LayoutParams::kWrapContent;
  row->addView(slider, sliderParams);
  return slider;
}

// ───────────────────────── 预览与状态 ─────────────────────────

void EditorActivity::refreshPreview() {
  if (!photo_ || pixels_.isEmpty()) return;

  // 预览始终基于「已应用的像素」重算，不在上一次结果上叠加：
  // 滑块来回拖时反复累积重采样会越拖越糊
  AiUI::RgbaImage preview = pixels_;

  // 基准必须是 pixels_ 而不是 original_：应用过一次裁剪后两者尺寸已经不同，
  // 用原始尺寸去乘比例会裁错地方、甚至裁出空图
  if (tool_ == Tool::kCrop) {
    const AiUI::Rect region{
        static_cast<int>(std::lround(cropLeft_ * pixels_.width)),
        static_cast<int>(std::lround(cropTop_ * pixels_.height)),
        static_cast<int>(std::lround(cropRight_ * pixels_.width)),
        static_cast<int>(std::lround(cropBottom_ * pixels_.height))};
    preview = editor::crop(preview, region);
  } else if (tool_ == Tool::kResize) {
    preview = editor::resample(preview, resizeWidth_, resizeHeight_,
                               editor::Resample::kBilinear);
  }

  presentPixels(preview);
}

void EditorActivity::refreshStage() {
  const bool has = !pixels_.isEmpty();
  if (homePane_) homePane_->show(!has);
  if (photo_) photo_->show(has);
}

// 把像素交给 PhotoView：它要的是 Image 句柄，旧的必须释放，
// 否则每拖一次滑块就漏一张位图
void EditorActivity::presentPixels(const AiUI::RgbaImage& pixels) {
  if (!photo_) return;
  if (previewImage_.isValid()) {
    platform()->releaseImage(previewImage_);
    previewImage_ = AiUI::Image{};
  }
  if (pixels.isEmpty()) {
    photo_->clearImage();
    return;
  }
  previewImage_ = platform()->createImage(pixels);
  if (previewImage_.isValid()) {
    photo_->setImage(previewImage_);
    photo_->setImageHasAlpha(hasAlpha(pixels));
  }
}

void EditorActivity::refreshActions() {
  const bool has = !pixels_.isEmpty();
  if (btnApply_) btnApply_->setEnabled(has);
  if (btnReset_) btnReset_->setEnabled(has);
}

// ───────────────────────── 动作 ─────────────────────────

void EditorActivity::applyTool() {
  if (pixels_.isEmpty()) return;

  if (tool_ == Tool::kCrop) {
    const AiUI::Rect region{
        static_cast<int>(std::lround(cropLeft_ * pixels_.width)),
        static_cast<int>(std::lround(cropTop_ * pixels_.height)),
        static_cast<int>(std::lround(cropRight_ * pixels_.width)),
        static_cast<int>(std::lround(cropBottom_ * pixels_.height))};
    pixels_ = editor::crop(pixels_, region);
    // 裁完之后就是新的全图，比例回到 0..1
    cropLeft_ = 0.0f;
    cropTop_ = 0.0f;
    cropRight_ = 1.0f;
    cropBottom_ = 1.0f;
  } else {
    pixels_ = editor::resample(pixels_, resizeWidth_, resizeHeight_,
                               editor::Resample::kBilinear);
    resizeWidth_ = pixels_.width;
    resizeHeight_ = pixels_.height;
  }

  presentPixels(pixels_);
  rebuildPanel();
}

void EditorActivity::resetTool() {
  // 重置回「本次打开时的原图」：用户连调几次发现不对，一条路退回去
  if (original_.isEmpty()) return;
  pixels_ = original_;
  cropLeft_ = 0.0f;
  cropTop_ = 0.0f;
  cropRight_ = 1.0f;
  cropBottom_ = 1.0f;
  resizeWidth_ = original_.width;
  resizeHeight_ = original_.height;
  presentPixels(pixels_);
  rebuildPanel();
}

void EditorActivity::pickAndOpen() {
  const std::wstring picked = platform()->pickOpenFile(
      nativeWindow(), L"打开图片", std::wstring(),
      {L"图片", L"*.png;*.jpg;*.jpeg;*.bmp"});
  if (!picked.empty()) openImage(picked);
}

AiUI::NativeHandle EditorActivity::nativeWindow() const {
  return window() ? window()->handle() : nullptr;
}

void EditorActivity::paintForSnapshot(AiUI::Painter& painter, const AiUI::Rect& area) {
  AiUI::View* root = contentView();
  if (!root) return;
  // 先布局再画：快照在 onCreate 之后紧接着执行，此时布局还没跑过，
  // 直接 draw 会画出一堆零尺寸控件（之前踩过，出图全是空白）
  root->layout(area);
  painter.fillRect(area, AiUI::makeColor(0xff, 0xff, 0xff, 255));
  root->draw(painter, area);
}

// ───────────────────────── 快捷键 ─────────────────────────

bool EditorActivity::onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) {
  if (key == AiUI::Key::kS && mods.ctrl) {
    saveAs();
    return true;
  }
  if (key == AiUI::Key::kO && mods.ctrl) {
    pickAndOpen();
    return true;
  }
  if (key == AiUI::Key::kEnter) {
    applyTool();
    return true;
  }
  return false;
}

}  // namespace ui
