// PhotoView 实现
//
// 只有两个坐标系，别再加第三个：
//   - 图像坐标：解码后的原图，左上 (0,0)，宽高 sourceSize_
//   - 视图坐标：控件客户区，左上 (0,0)
// 绘制链固定是「图像 × zoom，居中，再绕视图中心旋转，再平移」：
//   V = C + pan + Rot(图像坐标 × zoom − WH/2)
// WH 是原图宽高（不是旋转后的）：旋转是在画完之后整幅转的，
// 所以图像在「旋转前」的空间里永远是原始朝向。这一点决定了后面所有
// 裁剪与网格都该在原图坐标里算，只有最后一步才套 Rot。
#include "AiUI/widgets/photo_view.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"

#include <algorithm>
#include <cmath>

namespace AiUI {
namespace {

// 缩放下限：再小整张图只剩一个点。上限给到 256 倍：
// 一个像素能铺满半屏，任何像素都看得清。
constexpr float kMinZoom = 0.002f;
constexpr float kMaxZoom = 256.0f;
// 一格滚轮的倍数步进
constexpr float kWheelStep = 1.15f;
// 动画帧间隔；每帧都要重画一次，16ms 约 60 帧
constexpr int kFrameMs = 16;
// 缩略代理的最长边：2048 够铺满 4K 屏，解码代价小两个数量级
constexpr int kProxySide = 2048;
// 网格线太密会糊成一片灰，低于这个格边长不画线
constexpr float kGridLineMinCell = 5.0f;
// 一屏最多画多少个 RGB 文本格：再多也读不过来，纯属浪费绘制时间
constexpr int kGridTextMaxCells = 400;

// 缓动：先快后慢，末尾稳稳停住。线性会让人以为卡了一下才停。
float easeOutCubic(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

Size rotatedSizeOf(Size size, int turns) {
  if ((((turns % 4) + 4) % 4) % 2 != 0) return Size{size.height, size.width};
  return size;
}

int normalizeTurns(int turns) { return ((turns % 4) + 4) % 4; }

// 90° 倍数旋转在「y 轴向下」的屏幕坐标里就是整数换序，直接用查表，
// 不用三角函数——三角函数带浮点误差，网格线与像素边界会差一个像素。
void rotateOffset90(float x, float y, int turns, float& outX, float& outY) {
  switch (normalizeTurns(turns)) {
    case 1: outX = -y; outY = x; break;
    case 2: outX = -x; outY = -y; break;
    case 3: outX = y; outY = -x; break;
    default: outX = x; outY = y; break;
  }
}

// 逆变换：视图偏移 → 图像偏移
void unrotateOffset90(float x, float y, int turns, float& outX, float& outY) {
  switch (normalizeTurns(turns)) {
    case 1: outX = y; outY = -x; break;
    case 2: outX = -x; outY = -y; break;
    case 3: outX = -y; outY = x; break;
    default: outX = x; outY = y; break;
  }
}

// 顺时针转 turns 格后，原图 (x,y) 落在旋转后图像的哪个位置。
// 用连续坐标（不 −1），这样半开区间矩形换算不会多丢一行像素。
Point toRotatedPoint(int x, int y, Size src, int turns) {
  switch (normalizeTurns(turns)) {
    case 1: return Point{src.height - y, x};
    case 2: return Point{src.width - x, src.height - y};
    case 3: return Point{y, src.width - x};
    default: return Point{x, y};
  }
}

}  // namespace

PhotoView::PhotoView() {
  setClaimsPointer(true);
  setFocusable(true);
  // 悬停就提示「可以抓住拖」：手掌比箭头直接
  setCursor(CursorShape::kGrab);
}

PhotoView::~PhotoView() {
  if (image_.isValid()) platform()->releaseImage(image_);
  if (proxy_.isValid()) platform()->releaseImage(proxy_);
}

// ───────────────────────── 加载 ─────────────────────────

bool PhotoView::loadFile(const std::wstring& path) {
  if (path.empty()) {
    clearImage();
    return false;
  }

  Image loaded = platform()->loadImage(path);
  if (!loaded.isValid()) {
    clearImage();
    return false;
  }

  if (image_.isValid()) platform()->releaseImage(image_);
  if (proxy_.isValid()) platform()->releaseImage(proxy_);
  image_ = loaded;
  proxy_ = Image{};
  proxyRatio_ = 0.0f;

  sourceSize_ = platform()->imageSize(image_);
  if (sourceSize_.width <= 0 || sourceSize_.height <= 0) {
    clearImage();
    return false;
  }

  // 原图比代理上限还大才额外做一份：小图做代理纯属多解一次
  const int longest = (std::max)(sourceSize_.width, sourceSize_.height);
  if (longest > kProxySide) {
    Image thumb = platform()->loadImageThumbnail(path, kProxySide);
    if (thumb.isValid()) {
      const Size thumbSize = platform()->imageSize(thumb);
      if (thumbSize.width > 0 && thumbSize.height > 0) {
        proxy_ = thumb;
        proxyRatio_ =
            static_cast<float>(thumbSize.width) / static_cast<float>(sourceSize_.width);
      } else {
        platform()->releaseImage(thumb);
      }
    }
  }

  path_ = path;
  rotation_ = 0;
  displayAngle_ = 0.0f;
  pan_ = Point{};
  hasLastMouse_ = false;
  anim_ = Animation{};
  dropPixelCache();
  applyFitNow();
  invalidate();
  notifyChanged();
  return true;
}

void PhotoView::setImage(const Image& image) {
  if (image_.isValid()) platform()->releaseImage(image_);
  if (proxy_.isValid()) platform()->releaseImage(proxy_);
  image_ = image;
  proxy_ = Image{};
  proxyRatio_ = 0.0f;
  sourceSize_ = image_.isValid() ? platform()->imageSize(image) : Size{};
  pan_ = Point{};
  anim_ = Animation{};
  dropPixelCache();
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::clearImage() {
  if (image_.isValid()) platform()->releaseImage(image_);
  if (proxy_.isValid()) platform()->releaseImage(proxy_);
  image_ = Image{};
  proxy_ = Image{};
  proxyRatio_ = 0.0f;
  path_.clear();
  sourceSize_ = Size{};
  zoom_ = 1.0f;
  fitToWindow_ = false;
  rotation_ = 0;
  displayAngle_ = 0.0f;
  pan_ = Point{};
  anim_ = Animation{};
  hasLastMouse_ = false;
  dragging_ = false;
  dropPixelCache();
  invalidate();
}

void PhotoView::dropPixelCache() {
  pixelCache_ = RgbaImage{};
  pixelCacheRect_ = Rect{};
}

// ───────────────────────── 缩放 ─────────────────────────

Point PhotoView::zoomAnchor() const {
  if (hasLastMouse_) return lastMouse_;
  return clientRect().center();
}

void PhotoView::setZoom(float zoom) { setZoomAt(zoom, zoomAnchor()); }

void PhotoView::setZoomAt(float zoom, Point anchor) {
  const float clamped = (std::min)(kMaxZoom, (std::max)(kMinZoom, zoom));
  // 用户自己定了倍数就不再「跟随窗口」：窗口一变就改回去等于无视他的操作
  fitToWindow_ = false;

  // 以锚点为中心缩放也要动画：一次滚一格也走同一条路径，
  // 这样连续滚轮会不断改写目标倍数，看起来是顺滑地加速而不是一格一跳。
  beginZoomTo(clamped, anchor, animMs_, false);
}

void PhotoView::zoomBy(float factor) {
  if (factor <= 0.0f || sourceSize_.width <= 0) return;
  // 从「目标倍数」而不是「此刻倍数」乘：动画进行中 zoom_ 只是中途值，
  // 拿它乘出来的目标会比「上一格的目标 × factor」少一截，连续滚轮就越滚
  // 越不跟手（实测 14 格只到 5.8 倍，而 1.15^14 应是 7.1 倍）。
  // 目标值累计才不会丢格，同时动画依然顺滑地接力。
  setZoomAt(targetZoom() * factor, zoomAnchor());
}

float PhotoView::targetZoom() const {
  // 动画中锚点不变式逐帧反解 pan，所以从当前状态与从目标状态算出的目标
  // 平移是同一个值；倍数却必须从目标续算，否则每格都会少一点。
  return anim_.kind != AnimKind::kNone ? anim_.toZoom : zoom_;
}

Size PhotoView::rotatedSize() const { return rotatedSizeOf(sourceSize_, rotation_); }

float PhotoView::fitZoomFor(int rotation) const {
  const Rect rc = clientRect();
  const Size rotated = rotatedSizeOf(sourceSize_, rotation);
  if (rotated.width <= 0 || rotated.height <= 0 || rc.isEmpty()) return 1.0f;
  const float sx = static_cast<float>(rc.width()) / static_cast<float>(rotated.width);
  const float sy = static_cast<float>(rc.height()) / static_cast<float>(rotated.height);
  // 适应窗口不放大：小图放大到铺满整屏会糊，也让人误以为原图就那么大
  return (std::min)(1.0f, (std::max)(kMinZoom, (std::min)(sx, sy)));
}

void PhotoView::applyFitNow() {
  anim_ = Animation{};
  zoom_ = fitZoomFor(rotation_);
  fitToWindow_ = true;
  displayAngle_ = static_cast<float>(rotation_ * 90);
  pan_ = Point{};
  dropPixelCache();
}

void PhotoView::applyZoomAt(float zoom, Point anchor) {
  if (zoom_ <= 0.0f) zoom_ = 1.0f;
  const float ratio = zoom / zoom_;

  // 反解 pan，让锚点下的那个图像像素在缩放前后重合：
  //   缩放前 A = C + pan + Rot(P·zoom − WH/2)
  //   缩放后要求 A = C + pan' + Rot(P·zoom' − WH/2)
  // 两式相减，Rot 是线性的，约掉之后：
  //   pan' = (A − C) − (A − C − pan) · (zoom'/zoom)
  // 注意这里不出现 rotation_：旋转作用在同一个「图像 × zoom」向量上，
  // 缩放前后它不变，所以锚点锚定与当前转到多少度无关。
  const Point center = clientRect().center();
  const float ax = static_cast<float>(anchor.x - center.x);
  const float ay = static_cast<float>(anchor.y - center.y);
  const float dx = ax - static_cast<float>(pan_.x);
  const float dy = ay - static_cast<float>(pan_.y);

  zoom_ = zoom;
  pan_.x = static_cast<int>(std::lround(ax - dx * ratio));
  pan_.y = static_cast<int>(std::lround(ay - dy * ratio));

  clampPan();
  dropPixelCache();
}

void PhotoView::zoomToFit() {
  if (sourceSize_.width <= 0 || clientRect().isEmpty()) {
    zoom_ = 1.0f;
    fitToWindow_ = false;
    pan_ = Point{};
    invalidate();
    return;
  }

  const float target = fitZoomFor(rotation_);
  const bool alreadyThere =
      std::fabs(target - zoom_) < 0.0005f && pan_.x == 0 && pan_.y == 0 &&
      std::fabs(displayAngle_ - static_cast<float>(rotation_ * 90)) < 0.001f;
  if (animMs_ <= 0 || alreadyThere) {
    applyFitNow();
    invalidate();
    notifyChanged();
    return;
  }

  // 以视图中心为锚往目标倍数走；落位后 refit 会按当时的尺寸再算一次，
  // 这样动画期间窗口又变过也不会停在一个过时的倍数上。
  beginZoomTo(target, clientRect().center(), animMs_, true);
}

void PhotoView::zoomToActual() { setZoomAt(1.0f, zoomAnchor()); }

// ───────────────────────── 旋转 ─────────────────────────

void PhotoView::rotate(int quarterTurns) {
  if (sourceSize_.width <= 0 || quarterTurns == 0) return;
  const int turns = normalizeTurns(quarterTurns);
  if (turns == 0) return;

  beginRotate(turns);
}

void PhotoView::beginRotate(int turns) {
  // 上一段动画先落位：两个动画共用 zoom_/pan_/displayAngle_，
  // 叠加着跑会让目标值互相污染。
  finishAnimation();
  rotation_ = normalizeTurns(rotation_ + turns);

  if (animMs_ <= 0) {
    displayAngle_ = static_cast<float>(rotation_ * 90);
    pan_ = Point{};
    if (fitToWindow_) applyFitNow();
    dropPixelCache();
    invalidate();
    notifyChanged();
    return;
  }

  Animation anim;
  anim.kind = AnimKind::kRotate;
  anim.durationMs = animMs_;
  anim.fromZoom = zoom_;
  anim.toZoom = zoom_;
  anim.fromAngle = displayAngle_;
  // 只往用户按的那个方向转，不能从 270° 一路倒回 −90°
  anim.toAngle = displayAngle_ + static_cast<float>(turns * 90);
  anim.fromPan = pan_;
  // 宽高对调后旧平移量没有意义，转到一半就回中
  anim.toPan = Point{};
  // 适应窗口状态下，转完要按新宽高重算倍数
  anim.refit = fitToWindow_;
  beginAnimation(anim);
}

void PhotoView::setPan(Point pan) {
  pan_ = pan;
  clampPan();
  dropPixelCache();
  invalidate();
  notifyChanged();
}

// ───────────────────────── 动画 ─────────────────────────

void PhotoView::beginZoomTo(float zoom, Point anchor, int durationMs, bool refit) {
  // 先把上一段动画落位再算目标：目标平移是从「当前 pan」反解的，
  // 若等算完再落位，反解用的就是中途值、落位后又被覆盖，锚点会跳一下。
  finishAnimation();

  const float clamped = (std::min)(kMaxZoom, (std::max)(kMinZoom, zoom));
  const Point center = clientRect().center();
  const float anchorX = static_cast<float>(anchor.x - center.x);
  const float anchorY = static_cast<float>(anchor.y - center.y);

  if (zoom_ <= 0.0f) zoom_ = 1.0f;
  const float ratio = clamped / zoom_;
  // d = 锚点相对图像中心的 视图偏移 − 当前平移
  const float dx = anchorX - static_cast<float>(pan_.x);
  const float dy = anchorY - static_cast<float>(pan_.y);
  const Point targetPan{static_cast<int>(std::lround(anchorX - dx * ratio)),
                        static_cast<int>(std::lround(anchorY - dy * ratio))};

  if (durationMs <= 0 || std::fabs(clamped - zoom_) < 0.0001f) {
    // 倍数没变也可能是平移量该变，仍按目标落一次位
    zoom_ = clamped;
    pan_ = targetPan;
    displayAngle_ = static_cast<float>(rotation_ * 90);
    clampPan();
    dropPixelCache();
    invalidate();
    notifyChanged();
    return;
  }

  Animation anim;
  anim.kind = AnimKind::kZoom;
  anim.durationMs = durationMs;
  anim.fromZoom = zoom_;
  anim.toZoom = clamped;
  anim.fromAngle = displayAngle_;
  anim.toAngle = displayAngle_;
  anim.fromPan = pan_;
  anim.toPan = targetPan;
  anim.anchorX = anchorX;
  anim.anchorY = anchorY;
  anim.deltaX = dx;
  anim.deltaY = dy;
  anim.refit = refit;
  beginAnimation(anim);
}

void PhotoView::beginAnimation(const Animation& anim) {
  anim_ = anim;
  anim_.startMs = platform()->monotonicMillis();
  // 平台只有一只单次定时器：按帧间隔排下一拍，onDelayed 里再排下一拍
  postDelayed(kFrameMs);
  invalidate();
  notifyChanged();
}

void PhotoView::tickAnimation() {
  if (anim_.kind == AnimKind::kNone) return;

  const int64_t elapsed = platform()->monotonicMillis() - anim_.startMs;
  const float raw = anim_.durationMs > 0
                        ? static_cast<float>(elapsed) / static_cast<float>(anim_.durationMs)
                        : 1.0f;
  if (raw >= 1.0f) {
    finishAnimation();
    return;
  }

  const float t = easeOutCubic(raw);
  zoom_ = lerp(anim_.fromZoom, anim_.toZoom, t);
  displayAngle_ = lerp(anim_.fromAngle, anim_.toAngle, t);

  if (anim_.kind == AnimKind::kZoom) {
    // 逐帧按「锚点下的像素不动」反解 pan：直线插值 fromPan→toPan 也能到位，
    // 但前提是两条路径都在同一倍数序列上——直接反解才与最终状态严格一致，
    // 而且中途每一次重绘都自然满足锚定，不需要额外修正。
    const float ratio = anim_.fromZoom > 0.0f ? zoom_ / anim_.fromZoom : 1.0f;
    pan_.x = static_cast<int>(std::lround(anim_.anchorX - anim_.deltaX * ratio));
    pan_.y = static_cast<int>(std::lround(anim_.anchorY - anim_.deltaY * ratio));
  } else {
    pan_.x = static_cast<int>(std::lround(
        lerp(static_cast<float>(anim_.fromPan.x), static_cast<float>(anim_.toPan.x), t)));
    pan_.y = static_cast<int>(std::lround(
        lerp(static_cast<float>(anim_.fromPan.y), static_cast<float>(anim_.toPan.y), t)));
  }

  dropPixelCache();
  invalidate();
  postDelayed(kFrameMs);
}

void PhotoView::finishAnimation() {
  if (anim_.kind == AnimKind::kNone) return;

  zoom_ = anim_.toZoom;
  displayAngle_ = anim_.toAngle;
  pan_ = anim_.toPan;
  const bool refit = anim_.refit;
  anim_ = Animation{};

  // 角度必须落回 90° 的整数倍：命中与像素格都按 rotation_ 算，
  // 停在 87° 会让之后每次判断都偏一点。
  displayAngle_ = static_cast<float>(rotation_ * 90);
  if (refit) {
    zoom_ = fitZoomFor(rotation_);
    pan_ = Point{};
    fitToWindow_ = true;
  }
  dropPixelCache();
  clampPan();
  invalidate();
  notifyChanged();
}

void PhotoView::onDelayed() { tickAnimation(); }

// ───────────────────────── 几何 ─────────────────────────

void PhotoView::clampPan() {
  const Rect rc = clientRect();
  if (rc.isEmpty()) return;
  const Size rotated = rotatedSize();
  const int drawnW = static_cast<int>(std::lround(rotated.width * zoom_));
  const int drawnH = static_cast<int>(std::lround(rotated.height * zoom_));

  // 图比视图大：夹在 ±(drawn − view)/2，保证边缘不漏出空白。
  // 图比视图小：允许它离开居中位置，上限是「图心最多移到视图边上」。
  // 这里不能一律用 (view − drawn)/2 把它按在正中——锚点缩放要求光标下的
  // 像素钉住不动，图必然要偏离中心；强行居中会在放大时把图从光标底下拽走，
  // 「鼠标在哪放大哪」就废了。代价是图小时可能露出一块空白，这是对的：
  // 那个像素本来就该在光标底下。
  const int limitX = drawnW > rc.width() ? (drawnW - rc.width()) / 2 : rc.width() / 2;
  const int limitY = drawnH > rc.height() ? (drawnH - rc.height()) / 2 : rc.height() / 2;
  pan_.x = (std::max)(-limitX, (std::min)(limitX, pan_.x));
  pan_.y = (std::max)(-limitY, (std::min)(limitY, pan_.y));
}

Point PhotoView::toSource(int u, int v) const {
  // 旋转后坐标 → 原图坐标（toRotatedPoint 的逆）
  switch (normalizeTurns(rotation_)) {
    case 1: return Point{v, sourceSize_.height - u};
    case 2: return Point{sourceSize_.width - u, sourceSize_.height - v};
    case 3: return Point{sourceSize_.width - v, u};
    default: return Point{u, v};
  }
}

Point PhotoView::toRotated(int x, int y) const { return toRotatedPoint(x, y, sourceSize_, rotation_); }

Point PhotoView::centerPixel() const {
  if (sourceSize_.width <= 0 || zoom_ <= 0.0f) return Point{};
  // 视图中心 → 图像坐标：先反旋转、去掉平移，再除掉缩放并加回图像中心
  float lx = 0.0f;
  float ly = 0.0f;
  unrotateOffset90(-static_cast<float>(pan_.x), -static_cast<float>(pan_.y), rotation_, lx, ly);
  const double px = sourceSize_.width / 2.0 + lx / zoom_;
  const double py = sourceSize_.height / 2.0 + ly / zoom_;
  return Point{static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py))};
}

Rect PhotoView::visibleSourceRect(const Rect& view) const {
  return visibleSourceRectAt(view, static_cast<float>(rotation_ * 90));
}

Rect PhotoView::visibleSourceRectAt(const Rect& view, float angleDeg) const {
  if (sourceSize_.width <= 0 || sourceSize_.height <= 0 || zoom_ <= 0.0f) return Rect{};

  const Point center = view.center();
  const float cx = static_cast<float>(center.x + pan_.x);
  const float cy = static_cast<float>(center.y + pan_.y);

  // 反解绘制链 V = C + pan + R(角度)·(P·zoom − WH/2)：
  // 先把视图四角平移到图像原点，反旋，再除缩放并加回图像中心。
  // 用连续的 sin/cos 而不是 90° 查表：动画中途角度不是整倍数，
  // 查表会把可见区域算歪，裁出来的块就不够覆盖屏幕。
  float s = 0.0f;
  float c = 1.0f;
  if (std::fabs(angleDeg) > 0.001f) {
    const float rad = -angleDeg * 3.14159265358979323846f / 180.0f;
    s = std::sin(rad);
    c = std::cos(rad);
  }

  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  bool first = true;
  const Point corners[4] = {{view.left, view.top}, {view.right, view.top},
                            {view.left, view.bottom}, {view.right, view.bottom}};
  for (const Point& corner : corners) {
    const float ox = static_cast<float>(corner.x) - cx;
    const float oy = static_cast<float>(corner.y) - cy;
    const float lx = ox * c - oy * s;
    const float ly = ox * s + oy * c;
    const float ix = lx / zoom_ + sourceSize_.width / 2.0f;
    const float iy = ly / zoom_ + sourceSize_.height / 2.0f;
    if (first) {
      minX = maxX = ix;
      minY = maxY = iy;
      first = false;
    } else {
      minX = (std::min)(minX, ix);
      maxX = (std::max)(maxX, ix);
      minY = (std::min)(minY, iy);
      maxY = (std::max)(maxY, iy);
    }
  }

  // 多留一格：半像素落在边界上时不会因为取整被切掉一条
  const Rect rough{static_cast<int>(std::floor(minX)) - 1, static_cast<int>(std::floor(minY)) - 1,
                   static_cast<int>(std::ceil(maxX)) + 1, static_cast<int>(std::ceil(maxY)) + 1};
  return rough.intersect(Rect{0, 0, sourceSize_.width, sourceSize_.height});
}

void PhotoView::pickBitmap(Image& out, float& ratio) const {
  // 代理的有效分辨率是原图的 proxyRatio_ 倍：屏幕上一个代理像素被放大到
  // zoom_/proxyRatio_ 之前都看不出差别，那之前没理由付全尺寸的代价。
  // 动画期间一律用代理——每帧重采样一张巨图会把帧率打到个位数。
  if (proxy_.isValid() && (anim_.kind != AnimKind::kNone || zoom_ <= proxyRatio_ * 1.05f)) {
    out = proxy_;
    ratio = proxyRatio_;
    return;
  }
  out = image_;
  ratio = 1.0f;
}

bool PhotoView::isPixelGridVisible() const {
  // 动画期间不画：网格要贴死在像素边界上，整幅图在转的时候它必然错位
  return sourceSize_.width > 0 && zoom_ >= gridZoom_ && anim_.kind == AnimKind::kNone;
}

// ───────────────────────── 像素格 ─────────────────────────

void PhotoView::paintPixelGrid(Painter& painter, const Rect& view) {
  if (!pixelProvider_ || sourceSize_.width <= 0) return;
  const float cell = zoom_;
  if (cell < kGridLineMinCell) return;

  // 只取可见的那一块像素：一屏可能只覆盖几十个像素，为此解出整张巨图没意义
  const Rect region = visibleSourceRect(view);
  if (region.isEmpty()) return;
  if (pixelCacheRect_ != region || pixelCache_.isEmpty()) {
    RgbaImage got;
    if (!pixelProvider_(region, got) || got.isEmpty()) {
      dropPixelCache();
      return;
    }
    pixelCache_ = std::move(got);
    pixelCacheRect_ = region;
  }
  if (pixelCache_.isEmpty()) return;

  const Point center = view.center();
  const float cx = static_cast<float>(center.x + pan_.x);
  const float cy = static_cast<float>(center.y + pan_.y);
  const float halfW = sourceSize_.width * cell / 2.0f;
  const float halfH = sourceSize_.height * cell / 2.0f;
  const int turns = normalizeTurns(rotation_);

  // 图像坐标 (x, y)（已乘 zoom）→ 视图坐标
  auto toView = [&](float x, float y, int& outX, int& outY) {
    float rx = 0.0f;
    float ry = 0.0f;
    rotateOffset90(x * cell - halfW, y * cell - halfH, turns, rx, ry);
    outX = static_cast<int>(std::lround(cx + rx));
    outY = static_cast<int>(std::lround(cy + ry));
  };

  // 网格线：只在格边长够大时画，太密会糊成一片灰。
  // 半透明黑压在任意像素上都看得见，不必按图片明暗换色。
  //
  // 线的长度必须夹在「图像在视图里占的那一块」之内：整行整列地画到
  // view.top/view.bottom 会让线跑出图片，铺到画布背景上——那不是网格，
  // 是几十条横贯窗口的灰线。
  const Color lineColor = makeColor(0x10, 0x10, 0x10, 0x66);
  for (int x = region.left; x <= region.right; ++x) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    toView(static_cast<float>(x), static_cast<float>(region.top), x0, y0);
    toView(static_cast<float>(x), static_cast<float>(region.bottom), x1, y1);
    if (x0 == x1) {
      if (x0 < view.left || x0 >= view.right) continue;
      const int top = (std::max)(view.top, (std::min)(y0, y1));
      const int bottom = (std::min)(view.bottom, (std::max)(y0, y1));
      if (bottom > top) painter.drawVLine(top, bottom, x0, lineColor);
    } else {
      if (y0 < view.top || y0 >= view.bottom) continue;
      const int left = (std::max)(view.left, (std::min)(x0, x1));
      const int right = (std::min)(view.right, (std::max)(x0, x1));
      if (right > left) painter.drawHLine(left, right, y0, lineColor);
    }
  }
  for (int y = region.top; y <= region.bottom; ++y) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    toView(static_cast<float>(region.left), static_cast<float>(y), x0, y0);
    toView(static_cast<float>(region.right), static_cast<float>(y), x1, y1);
    if (y0 == y1) {
      if (y0 < view.top || y0 >= view.bottom) continue;
      const int left = (std::max)(view.left, (std::min)(x0, x1));
      const int right = (std::min)(view.right, (std::max)(x0, x1));
      if (right > left) painter.drawHLine(left, right, y0, lineColor);
    } else {
      if (x0 < view.left || x0 >= view.right) continue;
      const int top = (std::max)(view.top, (std::min)(y0, y1));
      const int bottom = (std::min)(view.bottom, (std::max)(y0, y1));
      if (bottom > top) painter.drawVLine(top, bottom, x0, lineColor);
    }
  }

  // 竖向放不下一行字的格子一律不写：这比「格边长 < 40」之类的固定阈值准，
  // 因为字号随 DPI 变，阈值也该跟着变。
  const Font& font = fonts.uiSmall;
  const int textH = painter.textHeight(font);
  if (cell < textH + 2) return;
  const int inner = 2;
  int drawn = 0;
  for (int y = region.top; y < region.bottom && drawn < kGridTextMaxCells; ++y) {
    for (int x = region.left; x < region.right && drawn < kGridTextMaxCells; ++x) {
      int x0 = 0;
      int y0 = 0;
      int x1 = 0;
      int y1 = 0;
      toView(static_cast<float>(x), static_cast<float>(y), x0, y0);
      toView(static_cast<float>(x + 1), static_cast<float>(y + 1), x1, y1);
      const Rect cellRect{(std::min)(x0, x1), (std::min)(y0, y1), (std::max)(x0, x1),
                          (std::max)(y0, y1)};
      if (cellRect.intersect(view).isEmpty()) continue;
      // 竖向放不下一行字就别硬塞，半截数字比不画更难看
      if (cellRect.height() < textH + inner) continue;

      const size_t at = static_cast<size_t>(y - pixelCacheRect_.top) *
                            static_cast<size_t>(pixelCache_.width) +
                        static_cast<size_t>(x - pixelCacheRect_.left);
      if (at >= pixelCache_.pixels.size()) continue;
      const Color px = pixelCache_.pixels[at];

      const std::wstring text = std::to_wstring(colorRed(px)) + L"," +
                                std::to_wstring(colorGreen(px)) + L"," +
                                std::to_wstring(colorBlue(px));
      // 横向也要放得下：宁可这一格不写字，也不能让数字压到隔壁像素上
      if (painter.measureText(text, font) + inner > cellRect.width()) continue;

      // 亮像素用深字、暗像素用浅字：否则总有一半格子读不出来
      const int luma = (colorRed(px) * 299 + colorGreen(px) * 587 + colorBlue(px) * 114) / 1000;
      const Color ink = luma > 140 ? makeColor(0x12, 0x12, 0x12) : makeColor(0xf6, 0xf6, 0xf6);
      painter.drawText(cellRect.inset(1, 1, 1, 1), text, ink, font, TextAlign::kCenter);
      ++drawn;
    }
  }
}

// ───────────────────────── 绘制 ─────────────────────────

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

  Image bitmap = image_;
  float ratio = 1.0f;
  pickBitmap(bitmap, ratio);
  const Size bmpSize = platform()->imageSize(bitmap);
  if (bmpSize.width <= 0 || bmpSize.height <= 0) return;

  const float angle = displayAngle_;
  // 源矩形：按可见区域裁，巨图只让后端处理屏幕内那一块。
  // 动画期间也必须裁：此时整幅图被放大到几百万像素宽，照整张画会让
  // GDI+ 收到一个几百万元素的 dst 矩形，直接画不出东西（旋转时画面空白
  // 就是这么来的）。可见区域按当前 displayAngle_ 反解，逐帧算一次不贵。
  Rect source{0, 0, bmpSize.width, bmpSize.height};
  if (ratio > 0.0f) {
    const Rect visible = visibleSourceRectAt(rect, angle);
    if (!visible.isEmpty()) {
      const Rect scaled{
          static_cast<int>(std::floor(visible.left * ratio)),
          static_cast<int>(std::floor(visible.top * ratio)),
          static_cast<int>(std::ceil(visible.right * ratio)),
          static_cast<int>(std::ceil(visible.bottom * ratio))};
      const Rect clipped = scaled.intersect(Rect{0, 0, bmpSize.width, bmpSize.height});
      if (!clipped.isEmpty()) source = clipped;
    }
  }

  const Point center = rect.center();
  const double fullW = static_cast<double>(sourceSize_.width) * zoom_;
  const double fullH = static_cast<double>(sourceSize_.height) * zoom_;

  // 源子矩形换算成它在整幅图里的位置。直接把它拉伸到整幅 dst 上是错的——
  // 那会把一小块放大成整张。bitmap 是原图的 ratio 倍，像素 b 对应原图 b/ratio。
  const Rect dstSub{
      static_cast<int>(std::floor(static_cast<double>(source.left) / ratio * zoom_ - fullW / 2.0)),
      static_cast<int>(std::floor(static_cast<double>(source.top) / ratio * zoom_ - fullH / 2.0)),
      static_cast<int>(std::ceil(static_cast<double>(source.right) / ratio * zoom_ - fullW / 2.0)),
      static_cast<int>(
          std::ceil(static_cast<double>(source.bottom) / ratio * zoom_ - fullH / 2.0))};

  painter.save();
  painter.translate(center.x + pan_.x, center.y + pan_.y);
  if (std::fabs(angle) > 0.001f) painter.rotate(angle);

  // 放大到能看清像素时用最近邻：插值会把像素格抹成一团，那不是用户想看的
  const ImageSmoothing smoothing =
      zoom_ >= gridZoom_ ? ImageSmoothing::kNearest : ImageSmoothing::kSmooth;
  painter.drawImageScaled(bitmap, dstSub, source, smoothing);
  painter.restore();

  // 像素格与 RGB 值叠在图之上：只在静止且放大够多时画
  if (isPixelGridVisible()) paintPixelGrid(painter, rect);
}

Size PhotoView::onMeasure(const Size& available) {
  // 看图控件永远撑满可用区域：尺寸由宿主布局决定，不跟图像大小走
  return available;
}

void PhotoView::onLayout(const Rect& frame) {
  // 「适应窗口」要在窗口变大变小时重算：用户点了适应窗口之后拖大窗口，
  // 图停在原来那个倍数上会显得很小，还得再点一次。
  if (!fitToWindow_ || anim_.kind != AnimKind::kNone) return;
  if (sourceSize_.width <= 0 || frame.width() <= 0 || frame.height() <= 0) return;
  const float target = fitZoomFor(rotation_);
  if (std::fabs(target - zoom_) < 0.0005f && pan_.x == 0 && pan_.y == 0) return;
  applyFitNow();
  invalidate();
  notifyChanged();
}

// ───────────────────────── 鼠标 ─────────────────────────

bool PhotoView::onMouseDown(const MouseEvent& e) {
  if (!image_.isValid() || e.button != MouseButton::kLeft) return false;
  // 拖动与动画抢同一份状态：按下就先把动画落位，否则拖到一半会被动画拉回去
  finishAnimation();

  // 不放大也能拖：拖拽是看图的默认动作，不需要先滚轮放大才「解锁」。
  // 图比视图小时 clampPan 会把平移夹回 0，所以这里不必判断倍数。
  dragging_ = true;
  dragOrigin_ = e.position;
  dragStartPan_ = pan_;
  lastMouse_ = e.position;
  hasLastMouse_ = true;
  // 按下与松开必须能分辨，否则用户看不出这次按下有没有抓住
  setCursor(CursorShape::kGrabbing);
  captureMouse();
  return true;
}

void PhotoView::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!dragging_) return;
  dragging_ = false;
  releaseMouse();
  // 拖动过程不夹，松手补一次：既保住手感，又不让图被留在画面外
  clampPan();
  invalidate();
  notifyChanged();
  setCursor(CursorShape::kGrab);
}

void PhotoView::onMouseMove(const MouseEvent& e) {
  // 随时记鼠标位置：工具栏的放大按钮也以它为中心
  lastMouse_ = e.position;
  hasLastMouse_ = true;

  if (!dragging_) {
    if (image_.isValid()) setCursor(CursorShape::kGrab);
    return;
  }
  pan_ = Point{dragStartPan_.x + (e.position.x - dragOrigin_.x),
               dragStartPan_.y + (e.position.y - dragOrigin_.y)};
  // 拖动过程中不做 clamp：一旦夹住，用户往回拖时要等鼠标走完这一段才动，
  // 手感像卡住。松手时再夹。
  dropPixelCache();
  invalidate();
  notifyChanged();
}

void PhotoView::onMouseLeave() {
  hasLastMouse_ = false;
  if (dragging_) {
    dragging_ = false;
    releaseMouse();
    clampPan();
    setCursor(CursorShape::kGrab);
    invalidate();
    notifyChanged();
  }
}

bool PhotoView::onMouseWheel(const MouseEvent& e) {
  if (!image_.isValid() || e.wheelDelta == 0) return false;
  // 锚在光标位置：滚轮放大时那个像素必须留在原地
  lastMouse_ = e.position;
  hasLastMouse_ = true;
  const float steps = static_cast<float>(e.wheelDelta) / 120.0f;
  const float factor = std::pow(kWheelStep, steps);
  if (onWheelZoom_) onWheelZoom_(factor);
  else zoomBy(factor);
  return true;
}

void PhotoView::onDoubleClick(const MouseEvent& e) {
  lastMouse_ = e.position;
  hasLastMouse_ = true;
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
  if (image_.isValid() && sourceSize_.width > 0 && fitToWindow_) applyFitNow();
}
Color PhotoView::backgroundColor() const {
  // 画布底色比窗口略深一点：白图在浅色主题下才看得出边界
  return blendColor(theme().body, theme().ink, 0.04f);
}

void PhotoView::notifyChanged() {
  if (onChange_) onChange_();
}

}  // namespace AiUI
