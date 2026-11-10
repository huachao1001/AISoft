// ImageView 实现
#include "AiUI/widgets/image_view.h"

#include "AiUI/theme.h"

#include <algorithm>

namespace AiUI {
namespace {

// 等比缩放：取长边比例，保证整张图放得下
float fitScale(int srcW, int srcH, int dstW, int dstH) {
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return 1.0f;
  const float sx = static_cast<float>(dstW) / static_cast<float>(srcW);
  const float sy = static_cast<float>(dstH) / static_cast<float>(srcH);
  return (std::min)(sx, sy);
}

}  // namespace

bool ImageView::loadFile(const std::wstring& path) {
  image_ = platform()->loadImage(path);
  icon_ = IconId::kNone;
  sourceSize_ = Size{};
  if (!image_.isValid()) {
    invalidate();
    return false;
  }
  // 图片原始尺寸在绘制时按后端能力读取，这里不复测
  invalidate();
  return true;
}

void ImageView::setImage(const Image& image) {
  image_ = image;
  icon_ = IconId::kNone;
  invalidate();
}

void ImageView::setIcon(IconId icon) {
  icon_ = icon;
  image_ = Image{};
  invalidate();
}

void ImageView::clearImage() {
  image_ = Image{};
  icon_ = IconId::kNone;
  invalidate();
}

void ImageView::setScale(ImageScale scale) {
  scale_ = scale;
  invalidate();
}

void ImageView::setPlaceholderColor(Color color) {
  placeholderColor_ = color;
  invalidate();
}

Rect ImageView::targetRect(const Rect& inner) const {
  if (scale_ == ImageScale::kStretch) return inner;
  if (scale_ == ImageScale::kCenter) {
    const int w = (std::min)(sourceSize_.width, inner.width());
    const int h = (std::min)(sourceSize_.height, inner.height());
    const int x = inner.left + (inner.width() - w) / 2;
    const int y = inner.top + (inner.height() - h) / 2;
    return Rect{x, y, x + w, y + h};
  }
  if (!sourceSize_.width || !sourceSize_.height) return inner;

  const float k = fitScale(sourceSize_.width, sourceSize_.height, inner.width(), inner.height());
  const int w = static_cast<int>(sourceSize_.width * k);
  const int h = static_cast<int>(sourceSize_.height * k);
  const int x = inner.left + (inner.width() - w) / 2;
  const int y = inner.top + (inner.height() - h) / 2;
  return Rect{x, y, x + w, y + h};
}

void ImageView::onPaint(Painter& painter, const Rect& rect) {
  if (icon_ != IconId::kNone) {
    // 矢量图标按墨迹框贴合，边长取短边保证不变形
    const int side = (std::min)(rect.width(), rect.height());
    const int x = rect.left + (rect.width() - side) / 2;
    const int y = rect.top + (rect.height() - side) / 2;
    painter.drawIcon(icon_, Rect{x, y, x + side, y + side}, placeholderColor_);
    return;
  }
  if (!image_.isValid()) return;
  painter.drawImage(image_, targetRect(rect));
}

Size ImageView::onMeasure(const Size& available) {
  if (sourceSize_.width > 0 && sourceSize_.height > 0) return sourceSize_;
  // 没有原始尺寸时，撑满可用空间
  return available;
}

}  // namespace AiUI
