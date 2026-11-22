#include "pyui/widgets/image_view.h"

#include <algorithm>

namespace pyui {
namespace {

// 等比缩放，取较小比例以保证整张图放得下
float fitScale(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return 1.0f;
  const float sx = static_cast<float>(dstWidth) / static_cast<float>(srcWidth);
  const float sy = static_cast<float>(dstHeight) / static_cast<float>(srcHeight);
  return std::min(sx, sy);
}

}  // namespace

// ── 内容 ──

void ImageView::setImage(const Image& image) {
  image_ = image;
  // 位图与图标互斥：同一个控件里同时有两个东西，调用方永远不知道最后画的是谁
  if (image.isValid()) icon_ = IconId::kNone;
  invalidate();
}

void ImageView::setIcon(IconId icon) {
  if (icon_ == icon) return;
  icon_ = icon;
  if (icon != IconId::kNone) image_ = Image{};
  invalidate();
}

void ImageView::clearImage() {
  if (!hasImage()) return;
  image_ = Image{};
  icon_ = IconId::kNone;
  invalidate();
}

void ImageView::setSourceSize(Size size) {
  if (sourceSize_.width == size.width && sourceSize_.height == size.height) return;
  sourceSize_ = size;
  requestLayout();
  invalidate();
}

void ImageView::setScale(ImageScale scale) {
  if (scale_ == scale) return;
  scale_ = scale;
  invalidate();
}

void ImageView::setIconColor(Color color) {
  if (iconColor_ == color) return;
  iconColor_ = color;
  invalidate();
}

// ── 绘制 ──

Rect ImageView::targetRect(const Rect& inner) const {
  if (inner.isEmpty()) return Rect{};

  switch (scale_) {
    case ImageScale::kStretch:
      return inner;

    case ImageScale::kCenter: {
      // 原始大小：放不下时按边界裁掉，而不是缩——kCenter 的语义是「别动像素」
      const int width = std::min(sourceSize_.width > 0 ? sourceSize_.width : inner.width(),
                                 inner.width());
      const int height = std::min(sourceSize_.height > 0 ? sourceSize_.height : inner.height(),
                                  inner.height());
      const int x = inner.left + (inner.width() - width) / 2;
      const int y = inner.top + (inner.height() - height) / 2;
      return Rect{x, y, x + width, y + height};
    }

    case ImageScale::kFill:
    case ImageScale::kFit: {
      // 原始尺寸未知时无法等比算，退化成拉伸：这是唯一不丢内容的兜底。
      // 静默画成 0×0 会让用户以为图加载失败，比稍微变形更难查。
      if (sourceSize_.width <= 0 || sourceSize_.height <= 0) return inner;

      const float fit = fitScale(sourceSize_.width, sourceSize_.height, inner.width(),
                                 inner.height());
      int width = static_cast<int>(static_cast<float>(sourceSize_.width) * fit);
      int height = static_cast<int>(static_cast<float>(sourceSize_.height) * fit);

      if (scale_ == ImageScale::kFill) {
        // 等比放大到「整块被覆盖」，多余部分由 drawImageScaled 的 source 裁掉。
        // 这里先放大目标矩形，绘制时用裁剪把溢出挡住。
        const float cover = std::max(
            static_cast<float>(inner.width()) / static_cast<float>(sourceSize_.width),
            static_cast<float>(inner.height()) / static_cast<float>(sourceSize_.height));
        width = static_cast<int>(static_cast<float>(sourceSize_.width) * cover);
        height = static_cast<int>(static_cast<float>(sourceSize_.height) * cover);
      }

      const int x = inner.left + (inner.width() - width) / 2;
      const int y = inner.top + (inner.height() - height) / 2;
      return Rect{x, y, x + width, y + height};
    }
  }
  return inner;
}

Color ImageView::resolveIconColor() const {
  if (iconColor_ != kNoColor) return iconColor_;
  return theme().text;
}

void ImageView::onPaint(Painter& painter, const Rect& rect) {
  const Rect inner = rect;
  if (inner.isEmpty()) return;

  if (icon_ != IconId::kNone) {
    // 矢量图标按墨色绘制，边长取短边保证不变形
    const int side = std::min(inner.width(), inner.height());
    const int x = inner.left + (inner.width() - side) / 2;
    const int y = inner.top + (inner.height() - side) / 2;
    painter.drawIcon(icon_, Rect::at(x, y, side, side), resolveIconColor());
    return;
  }

  if (!image_.isValid()) return;

  const Rect target = targetRect(inner);
  if (target.isEmpty()) return;

  // kFill 时 target 比 inner 大：裁剪到 inner，避免盖到邻居
  if (scale_ == ImageScale::kFill) {
    painter.save();
    painter.clipRect(inner);
    painter.drawImage(image_, target);
    painter.restore();
    return;
  }
  painter.drawImage(image_, target);
}

// ── 测量 ──

Size ImageView::onMeasure(const Size& available) {
  // 位图：已知原始尺寸时按原始尺寸，否则退回可用宽高。
  // 不按 available 硬撑：图标类视图撑满会让标题栏里的应用图标变成一条横条。
  if (image_.isValid() && sourceSize_.width > 0 && sourceSize_.height > 0) {
    return sourceSize_;
  }
  if (icon_ != IconId::kNone) {
    // 矢量图标没有固定尺寸，取可用空间的短边但不超过 16（默认图标档位）
    const int side = std::min(px(16), std::min(available.width, available.height));
    if (side > 0) return Size{side, side};
    return Size{px(16), px(16)};
  }
  return Size{};
}

}  // namespace pyui
