// PhotoView 实现
#include "AiUI/widgets/photo_view.h"

#include "AiUI/platform.h"
#include "AiUI/theme.h"

#include <algorithm>
#include <cmath>

namespace AiUI {
namespace {

// 缩放下限/上限：再小就看不出是什么，再大只会让后端算一堆看不见的像素
constexpr float kMinZoom = 0.02f;
constexpr float kMaxZoom = 32.0f;
// 这个倍数以上按像素格画：用户凑这么近就是要看单个像素
constexpr float kNearestThreshold = 3.0f;

}  // namespace

PhotoView::PhotoView() {
  setClaimsPointer(true);
  setFocusable(true);
  setCursor(CursorShape::kHand);
}

bool PhotoView::loadFile(const std::wstring& path) {
  Image loaded = platform()->loadImage(path);
  if (!loaded.isValid()) {
    clearImage();
    return false;
  }
  // 旧图交还后端：这里换的是视图自己持有的那一张
  if (image_.isValid()) platform()->releaseImage(image_);
  image_ = loaded;
  path_ = path;
  sourceSize_ = platform()->imageSize(image_);
  rotation_ = 0;
  pan_ = Point{};
  zoomToFit();
  return true;
}

void PhotoView::setImage(const Image& image) {
  if (image_.isValid()) platform()->releaseImage(image_);
  image_ = image;
  sourceSize_ = image_.isValid() ? platform()->imageSize(image) : Size{};
  pan_ = Point{};
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::clearImage() {
  if (image_.isValid()) platform()->releaseImage(image_);
  image_ = Image{};
  path_.clear();
  sourceSize_ = Size{};
  zoom_ = 1.0f;
  fitToWindow_ = false;
  rotation_ = 0;
  pan_ = Point{};
  invalidate();
}

void PhotoView::setZoom(float zoom) {
  const float clamped = (std::min)(kMaxZoom, (std::max)(kMinZoom, zoom));
  // 用户自己定了倍数就不再「跟随窗口」：窗口一变就改回去等于无视他的操作
  fitToWindow_ = false;
  if (std::fabs(clamped - zoom_) < 0.0001f) return;
  zoom_ = clamped;
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::zoomBy(float factor) {
  if (factor <= 0.0f) return;
  setZoom(zoom_ * factor);
}

Size PhotoView::rotatedSize() const {
  if (rotation_ % 2 == 1) return Size{sourceSize_.height, sourceSize_.width};
  return sourceSize_;
}

void PhotoView::zoomToFit() {
  const Rect rc = clientRect();
  const Size rotated = rotatedSize();
  if (rotated.width <= 0 || rotated.height <= 0 || rc.isEmpty()) {
    zoom_ = 1.0f;
    fitToWindow_ = false;
    pan_ = Point{};
    return;
  }
  const float sx = static_cast<float>(rc.width()) / static_cast<float>(rotated.width);
  const float sy = static_cast<float>(rc.height()) / static_cast<float>(rotated.height);
  // 适应窗口不放大：小图放大到铺满整屏会糊，也让人误以为原图就那么大
  const float fit = (std::min)(sx, sy);
  zoom_ = (std::min)(1.0f, (std::max)(kMinZoom, fit));
  // 记住这个状态：窗口尺寸变化时再算一次，用户拖大窗口图会跟着长大
  fitToWindow_ = true;
  pan_ = Point{};
  invalidate();
  notifyChanged();
}

void PhotoView::zoomToActual() {
  zoom_ = 1.0f;
  // 100% 是「一个图像像素对一个屏幕像素」，与窗口多大无关，不跟随
  fitToWindow_ = false;
  pan_ = Point{};
  invalidate();
  notifyChanged();
}

void PhotoView::rotate(int quarterTurns) {
  if (sourceSize_.width <= 0) return;
  rotation_ = ((rotation_ + quarterTurns) % 4 + 4) % 4;
  // 转完宽高可能对调：「适应窗口」下要按新宽高重算，否则横图转成竖图会溢出
  if (fitToWindow_) {
    zoomToFit();
    return;
  }
  // 不跟随窗口时原来的平移量多半把图顶出边界，重新夹一次
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::setPan(Point pan) {
  pan_ = pan;
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::clampPan() {
  const Rect rc = clientRect();
  const Size rotated = rotatedSize();
  const int drawnW = static_cast<int>(rotated.width * zoom_);
  const int drawnH = static_cast<int>(rotated.height * zoom_);

  // 图比视图小的那个方向必须居中，否则用户会把它拖出屏幕再找不回来
  const int limitX = (std::max)(0, (drawnW - rc.width()) / 2);
  const int limitY = (std::max)(0, (drawnH - rc.height()) / 2);
  pan_.x = (std::max)(-limitX, (std::min)(limitX, pan_.x));
  pan_.y = (std::max)(-limitY, (std::min)(limitY, pan_.y));
}

Point PhotoView::centerPixel() const {
  if (sourceSize_.width <= 0) return Point{};
  // 视图中心 → 图像坐标：先反旋转再除以缩放；pan 是图像相对视图中心的位移
  double vx = -static_cast<double>(pan_.x);
  double vy = -static_cast<double>(pan_.y);
  for (int i = 0; i < rotation_; ++i) {
    // 顺时针转一格的逆运算：逆时针转一格
    const double nx = vy;
    const double ny = -vx;
    vx = nx;
    vy = ny;
  }
  const double ix = vx / zoom_;
  const double iy = vy / zoom_;
  return Point{static_cast<int>(sourceSize_.width / 2.0 + ix),
               static_cast<int>(sourceSize_.height / 2.0 + iy)};
}

void PhotoView::notifyChanged() {
  if (onChange_) onChange_();
}

void PhotoView::onPaint(Painter& painter, const Rect& rect) {
  if (!image_.isValid() || sourceSize_.width <= 0) {
    // 空状态也画个小图标，免得整块区域纯色一片让人以为没加载出来
    const int side = (std::min)(48, (std::min)(rect.width(), rect.height()));
    if (side > 0) {
      const int x = rect.left + (rect.width() - side) / 2;
      const int y = rect.top + (rect.height() - side) / 2;
      painter.drawIcon(IconId::kImage, Rect{x, y, x + side, y + side}, theme().textFaint);
    }
    return;
  }

  // 旋转与平移都交给绘制后端做变换：不重采样，任意倍数下都不额外损失
  const Point center = rect.center();
  painter.save();
  painter.translate(center.x + pan_.x, center.y + pan_.y);
  if (rotation_ != 0) painter.rotate(static_cast<float>(rotation_ * 90));

  const int w = static_cast<int>(sourceSize_.width * zoom_);
  const int h = static_cast<int>(sourceSize_.height * zoom_);
  const Rect dst{-w / 2, -h / 2, -w / 2 + w, -h / 2 + h};
  const Rect full{0, 0, sourceSize_.width, sourceSize_.height};
  // 放大到能看清像素时用最近邻：插值会把像素格抹成一团，那不是用户想看的
  const ImageSmoothing smoothing =
      zoom_ >= kNearestThreshold ? ImageSmoothing::kNearest : ImageSmoothing::kSmooth;
  painter.drawImageScaled(image_, dst, full, smoothing);
  painter.restore();
}

Size PhotoView::onMeasure(const Size& available) {
  // 看图控件永远撑满可用区域：尺寸由宿主布局决定，不跟图像大小走
  return available;
}

void PhotoView::onLayout(const Rect& frame) {
  // 「适应窗口」要在窗口变大变小时重算：用户点了适应窗口之后拖大窗口，
  // 图停在原来那个倍数上会显得很小，还得再点一次。
  if (!fitToWindow_) return;
  const Size rotated = rotatedSize();
  if (rotated.width <= 0 || rotated.height <= 0) return;
  const int w = frame.width();
  const int h = frame.height();
  if (w <= 0 || h <= 0) return;

  const float sx = static_cast<float>(w) / static_cast<float>(rotated.width);
  const float sy = static_cast<float>(h) / static_cast<float>(rotated.height);
  zoom_ = (std::min)(1.0f, (std::max)(kMinZoom, (std::min)(sx, sy)));
  pan_ = Point{};
  notifyChanged();
}

bool PhotoView::onMouseDown(const MouseEvent& e) {
  if (!image_.isValid() || e.button != MouseButton::kLeft) return false;
  dragging_ = true;
  dragOrigin_ = e.position;
  dragStartPan_ = pan_;
  setCursor(CursorShape::kHand);
  captureMouse();
  return true;
}

void PhotoView::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!dragging_) return;
  dragging_ = false;
  releaseMouse();
}

void PhotoView::onMouseMove(const MouseEvent& e) {
  if (!dragging_) return;
  pan_ = Point{dragStartPan_.x + (e.position.x - dragOrigin_.x),
               dragStartPan_.y + (e.position.y - dragOrigin_.y)};
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::onMouseLeave() {
  if (dragging_) {
    dragging_ = false;
    releaseMouse();
  }
}

bool PhotoView::onMouseWheel(const MouseEvent& e) {
  if (!image_.isValid() || e.wheelDelta == 0) return false;
  // 一格 120；按 1.15 的幂次缩放，滚得越多差得越远，符合看图手感
  const float steps = static_cast<float>(e.wheelDelta) / 120.0f;
  const float factor = std::pow(1.15f, steps);
  if (onWheelZoom_) onWheelZoom_(factor);
  else zoomBy(factor);
  return true;
}

void PhotoView::onDoubleClick(const MouseEvent& e) {
  (void)e;
  if (!image_.isValid()) return;
  // 双击在「整图」与「原始大小」之间切换：这两个是所有看图软件的共同预期
  if (zoom_ < 0.999f) {
    zoomToActual();
    return;
  }
  zoomToFit();
}

void PhotoView::onAttach() {
  // 挂载后才知道窗口 DPI，首次布局才能按物理像素算「适应窗口」
  if (image_.isValid() && sourceSize_.width > 0) zoomToFit();
}

Color PhotoView::backgroundColor() const {
  // 画布底色比窗口略深一点：白图在浅色主题下才看得出边界
  return blendColor(theme().body, theme().ink, 0.04f);
}

}  // namespace AiUI
