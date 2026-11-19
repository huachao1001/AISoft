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
#include "AiUI/view_root.h"

#include <algorithm>
#include <cmath>

namespace AiUI {
namespace {

// 缩放下限：再小整张图只剩一个点。上限给到 256 倍：
// 一个像素能铺满半屏，任何像素都看得清。
constexpr float kMinZoom = 0.002f;
constexpr float kMaxZoom = 256.0f;
// 图像短边不得小于这个尺寸（设备像素）：再小就看不出这是张图了。
// 比这还小的图会被强制放大到刚好这么多——否则一打开就是一小撮点。
constexpr int kMinImageSidePx = 30;
// 一格滚轮的倍数步进
constexpr float kWheelStep = 1.15f;
// 动画帧间隔；每帧都要重画一次，16ms 约 60 帧
constexpr int kFrameMs = 16;
// 缩略代理的最长边：2048 够铺满 4K 屏，解码代价小两个数量级
constexpr int kProxySide = 2048;
// 网格线太密会糊成一片灰，低于这个格边长不画线
constexpr float kGridLineMinCell = 5.0f;
// 格子里 RGB 数字的 em 像素上限。字号按格子大小的三分之一取（三行各占
// 一格的三分之一），超过这个上限就没必要再大了。
constexpr int kCellTextMaxPx = 30;
// 显示数字的最小格边长（设备像素）：低于它三行数字挤成一团，读不出任何
// 信息，不如不画。两个常量之间做淡入，避免放大跨过阈值时字突然冒出来。
// 28 像素时 em 约 9，是数字仍可辨认的下界。
constexpr float kCellValueMinCell = 28.0f;
constexpr float kCellValueFullCell = 40.0f;
// 网格/锐化的淡入淡出时长。短于缩放动画：它是缩放到位后顺带的一层，
// 拖太久会显得黏。
constexpr int kFadeMs = 140;
// 滚轮跟随的时间常数（毫秒）：每过这么久，剩余距离衰减到约 37%。
// 90ms 时 180ms 已走掉 87%，300ms 走掉 96%——既跟手又有可见收尾。
constexpr float kFollowTauMs = 90.0f;

// 缩放比浮层：跑道形半透明黑底，白字。尺寸按 96 DPI 设计，绘制时按实际 DPI 放大。
constexpr int kBadgeTextPx = 26;
constexpr int kBadgePadX = 18;
constexpr int kBadgePadY = 8;
// 底色不透明度：0x99 在深色画布上能压住图，又不会挡住底下在缩放的内容
constexpr std::uint8_t kBadgeBgAlpha = 0x99;
constexpr std::uint8_t kBadgeFgAlpha = 0xff;
// 浮层淡入淡出时长：比网格略快，它是个读数，不该黏在屏幕上
constexpr int kBadgeFadeMs = 120;

// 缓动：两端都收住，中间才快。
//
// 原来用 easeOutCubic：它起步就有 24% 的行程（第 1 帧 t≈0.09 已到 0.24），
// 大幅缩放时第一帧就直接弹过去一大块，然后剩下一百多毫秒只是慢慢挪——
// 观感是「先猛跳一下，再磨蹭」，也就是用户说的生硬。
// 换成 smoothstep：起步与收尾都有加速度过渡，全程速度连续，
// 没有哪一帧的位移明显大于它的邻居。
float easeInOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// 格子里数字与逐格填充的可见程度。
// 格子太小时三行数字会糊成一团，读不出任何信息；逐格填充在这个尺寸也没必要
// （网格线本身就占了 1 px，线旁的 1 px 漂移看不出来）。两者共用同一条渐变，
// 免得出现「有填充没数字」或反之的错位感。
float cellValueFade(float cell) {
  if (cell <= kCellValueMinCell) return 0.0f;
  if (cell >= kCellValueFullCell) return 1.0f;
  return (cell - kCellValueMinCell) / (kCellValueFullCell - kCellValueMinCell);
}

// 平移上限（返回「平移量绝对值最多多少」）。
//
// 两条约束在这里打架：
//   · 图比视图大时不露白 → 上限 (drawn − view) / 2
//   · 锚点缩放要求光标下的像素钉住 → 小图必须允许把图心推离视图中心
// 旧版按「drawn > view」一刀切成两段：小图给 view/2，大图给 (drawn−view)/2。
// 两段在刚好铺满处不连续——余量从 view/2 直接掉到 0，滚轮一跨过那一格整幅图
// 就横跳几十像素（实测 2 倍处 x 跳 25px、4 倍处 y 跳 46px）。
//
// 改成取两者的较大值：在 drawn == view 处两边都等于 view/2，函数连续；
// 图再大时 (drawn−view)/2 接管，逐渐回到「不露白」的严格规则。
// 全程不小于 view/2，所以锚点反解出的平移一定在范围内，不会被夹回去。
float panLimit(float drawn, float view) {
  if (view <= 0.0f) return 0.0f;
  return (std::max)(view * 0.5f, (drawn - view) * 0.5f);
}

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
  // 悬停保持箭头：只有真的按住拖拽才换成握起的手，否则满屏手掌太吵
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
  pan_ = PanF{};
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
  pan_ = PanF{};
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
  pan_ = PanF{};
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
  // 工具栏按钮也走这里：鼠标停在画布空白处时以它当锚点会把图推走，
  // 所以只认「落在图上的」鼠标位置，否则退到视图中心。
  if (hasLastMouse_ && isOnImage(lastMouse_)) return lastMouse_;
  return clientRect().center();
}

void PhotoView::setZoom(float zoom) { setZoomAt(zoom, zoomAnchor()); }

void PhotoView::setZoomAt(float zoom, Point anchor) {
  const float clamped = clampZoom(zoom);
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

void PhotoView::wheelZoomBy(float factor) {
  if (factor <= 0.0f || sourceSize_.width <= 0) return;
  const float target = clampZoom(targetZoom() * factor);
  fitToWindow_ = false;
  // 已经在这条跟随链上时不要重开：重开会把速度清零，正是「顿挫」的来源。
  // 只把目标往前推，当前倍数与速度都留着，下一帧自然加速过去。
  if (anim_.kind != AnimKind::kNone && anim_.follow) {
    const Point anchor = zoomAnchor();
    const Point center = clientRect().center();
    const float anchorX = static_cast<float>(anchor.x - center.x);
    const float anchorY = static_cast<float>(anchor.y - center.y);
    // 目标和锚点一起更新：连续滚动时锚点可能移动（用户一边滚一边移鼠标）
    anim_.toZoom = target;
    anim_.anchorX = anchorX;
    anim_.anchorY = anchorY;
    anim_.deltaX = anchorX - pan_.x;
    anim_.deltaY = anchorY - pan_.y;
    anim_.fromZoom = zoom_;
    anim_.startMs = platform()->monotonicMillis();
    notifyChanged();
    return;
  }
  beginZoomTo(target, zoomAnchor(), animMs_, false, true);
}

float PhotoView::targetZoom() const {
  // 动画中锚点不变式逐帧反解 pan，所以从当前状态与从目标状态算出的目标
  // 平移是同一个值；倍数却必须从目标续算，否则每格都会少一点。
  return anim_.kind != AnimKind::kNone ? anim_.toZoom : zoom_;
}

Size PhotoView::rotatedSize() const { return rotatedSizeOf(sourceSize_, rotation_); }

float PhotoView::clampZoom(float zoom) const {
  return (std::min)(kMaxZoom, (std::max)(minZoom(), zoom));
}

// 最低倍数由「图像短边至少 kMinImageSidePx」反推：
//   zoom * minSide >= 30  →  zoom >= 30 / minSide
// 天然很小的图（短边不到 30）这个下限会大于 1，打开就是放大状态。
float PhotoView::minZoom() const {
  const Size rotated = rotatedSizeOf(sourceSize_, rotation_);
  const int minSide = (std::min)(rotated.width, rotated.height);
  if (minSide <= 0) return kMinZoom;
  const float bySide =
      static_cast<float>(kMinImageSidePx) / static_cast<float>(minSide);
  return (std::max)(kMinZoom, bySide);
}

float PhotoView::fitZoomFor(int rotation) const {
  const Rect rc = clientRect();
  const Size rotated = rotatedSizeOf(sourceSize_, rotation);
  if (rotated.width <= 0 || rotated.height <= 0 || rc.isEmpty()) return 1.0f;
  const float sx = static_cast<float>(rc.width()) / static_cast<float>(rotated.width);
  const float sy = static_cast<float>(rc.height()) / static_cast<float>(rotated.height);
  // 适应窗口通常不放大：小图放大到铺满整屏会糊，也让人误以为原图就那么大。
  // 但短边不足 30px 时仍要放大到那个尺寸，否则一打开就是一小撮点。
  // 演示态例外：投影时观众离得远，图小到一半屏幕才是更糟的结果。
  const float minSide = minZoom();
  const float fill = (std::max)(minSide, (std::min)(sx, sy));
  // 允许放大时直接铺满；否则封顶在 100%
  return fitAllowsUpscale_ ? fill : (std::min)((std::max)(minSide, 1.0f), fill);
}

void PhotoView::applyFitNow() {
  anim_ = Animation{};
  zoom_ = fitZoomFor(rotation_);
  fitToWindow_ = true;
  displayAngle_ = static_cast<float>(rotation_ * 90);
  pan_ = PanF{};
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
  const float dx = ax - pan_.x;
  const float dy = ay - pan_.y;

  zoom_ = zoom;
  // 保留小数：这里取整会让整幅图每帧抖 1 像素，放大后很明显
  pan_.x = ax - dx * ratio;
  pan_.y = ay - dy * ratio;

  clampPan();
  dropPixelCache();
}

void PhotoView::zoomToFit() {
  if (sourceSize_.width <= 0 || clientRect().isEmpty()) {
    zoom_ = 1.0f;
    fitToWindow_ = false;
    pan_ = PanF{};
    invalidate();
    return;
  }

  const float target = fitZoomFor(rotation_);
  const bool alreadyThere =
      std::fabs(target - zoom_) < 0.0005f && std::fabs(pan_.x) < 0.01f &&
      std::fabs(pan_.y) < 0.01f &&
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
    pan_ = PanF{};
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
  anim.toPan = PanF{};
  // 适应窗口状态下，转完要按新宽高重算倍数
  anim.refit = fitToWindow_;
  beginAnimation(anim);
}

void PhotoView::setPan(Point pan) {
  pan_ = PanF{static_cast<float>(pan.x), static_cast<float>(pan.y)};
  clampPan();
  dropPixelCache();
  invalidate();
  notifyChanged();
}

PhotoView::ViewState PhotoView::saveViewState() const {
  ViewState state;
  state.zoom = zoom_;
  state.panX = pan_.x;
  state.panY = pan_.y;
  state.fitToWindow = fitToWindow_;
  return state;
}

void PhotoView::restoreViewState(const ViewState& state) {
  anim_ = Animation{};
  fitToWindow_ = state.fitToWindow;
  if (fitToWindow_) {
    // 当时是「跟随窗口」，回来要按当前尺寸重算：旧倍数属于旧窗口
    applyFitNow();
  } else {
    zoom_ = clampZoom(state.zoom);
    pan_ = PanF{state.panX, state.panY};
    clampPan();
  }
  displayAngle_ = static_cast<float>(rotation_ * 90);
  dropPixelCache();
  invalidate();
  notifyChanged();
}

// ───────────────────────── 动画 ─────────────────────────

void PhotoView::beginZoomTo(float zoom, Point anchor, int durationMs, bool refit,
                            bool follow) {
  // 不调用 finishAnimation：那会把画面直接弹到上一段动画的目标位置，
  // 连续滚轮时每一格都弹一下，看起来就是「突变」。这里只清掉动画、
  // 保留当前这一帧的 zoom_/pan_ 当起点，位置因此是连续的。
  // 累计格数由 targetZoom() 在进入本函数前读出，所以清掉动画不丢格。
  anim_ = Animation{};

  const float clamped = clampZoom(zoom);
  const Point center = clientRect().center();
  const float anchorX = static_cast<float>(anchor.x - center.x);
  const float anchorY = static_cast<float>(anchor.y - center.y);

  if (zoom_ <= 0.0f) zoom_ = 1.0f;
  const float ratio = clamped / zoom_;
  // d = 锚点相对图像中心的视图偏移 − 当前平移
  const float dx = anchorX - pan_.x;
  const float dy = anchorY - pan_.y;
  const PanF targetPan{anchorX - dx * ratio, anchorY - dy * ratio};

  if (durationMs <= 0 || std::fabs(clamped - zoom_) < 0.0001f) {
    // 倍数没变也可能是平移量该变，仍按目标落一次位
    zoom_ = clamped;
    pan_ = targetPan;
    displayAngle_ = static_cast<float>(rotation_ * 90);
    clampPan();
    dropPixelCache();
    // 没有动画时也要在这里对齐网格目标，否则只能等下一次绘制
    updateGridFade();
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
  anim.follow = follow;
  beginAnimation(anim);
}

void PhotoView::beginAnimation(const Animation& anim) {
  anim_ = anim;
  anim_.startMs = platform()->monotonicMillis();
  // 动画的 kind 可能刚变（进入旋转要立刻关网格），先把网格目标对齐
  updateGridFade();
  // 平台只有一只单次定时器：按帧间隔排下一拍，onDelayed 里再排下一拍
  postDelayed(kFrameMs);
  invalidate();
  notifyChanged();
}

void PhotoView::tickAnimation() {
  if (anim_.kind == AnimKind::kNone) return;

  if (anim_.follow) {
    tickFollow();
    return;
  }

  const int64_t elapsed = platform()->monotonicMillis() - anim_.startMs;
  const float raw = anim_.durationMs > 0
                        ? static_cast<float>(elapsed) / static_cast<float>(anim_.durationMs)
                        : 1.0f;
  if (raw >= 1.0f) {
    finishAnimation();
    return;
  }

  const float t = easeInOut(raw);
  zoom_ = lerp(anim_.fromZoom, anim_.toZoom, t);
  displayAngle_ = lerp(anim_.fromAngle, anim_.toAngle, t);

  if (anim_.kind == AnimKind::kZoom) {
    // 逐帧按「锚点下的像素不动」反解 pan：直线插值 fromPan→toPan 也能到位，
    // 但前提是两条路径都在同一倍数序列上——直接反解才与最终状态严格一致，
    // 而且中途每一次重绘都自然满足锚定，不需要额外修正。
    // 保留小数：取整会让整幅图逐帧抖 ±1 像素，放大后非常明显。
    const float ratio = anim_.fromZoom > 0.0f ? zoom_ / anim_.fromZoom : 1.0f;
    pan_.x = anim_.anchorX - anim_.deltaX * ratio;
    pan_.y = anim_.anchorY - anim_.deltaY * ratio;
  } else {
    pan_.x = lerp(anim_.fromPan.x, anim_.toPan.x, t);
    pan_.y = lerp(anim_.fromPan.y, anim_.toPan.y, t);
  }

  // 倍数一跨过网格阈值就启动淡入。放在这里而不是等动画结束：
  // 否则网格要等缩放完全停住才整层冒出来，那一下最刺眼。
  updateGridFade();
  tickGridFade();

  dropPixelCache();
  invalidate();
  postDelayed(kFrameMs);
}

// 滚轮的逐帧跟随：每帧把「剩余的距离」按指数衰减走掉一部分。
//
// 与固定时长动画的根本区别是「剩余距离」的定义：固定时长动画插值一条
// 早就定好的 from→to 线段，连续滚动时每格都要丢弃旧线段、从当前点重新起一段，
// 速度于是每格归零再拉起，形成一串加速—刹车的锯齿，这就是顿挫感的来源。
//
// 这里只认「当前倍数」与「目标倍数」两个值：目标随滚轮不断前移，
// 每帧朝它收敛固定比例。连续滚动时剩余距离持续变大，速度自然跟着变大；
// 手一停目标不再动，剩余距离按同一条指数曲线衰减，于是收尾自动出现。
//
// 用真实帧间隔而不是固定 16ms：定时器精度不足时帧间隔会抖动，按固定步长
// 推进会让轨迹忽快忽慢，那也是「不丝滑」的来源之一。
void PhotoView::tickFollow() {
  const int64_t now = platform()->monotonicMillis();
  const int64_t elapsed = anim_.lastTickMs > 0 ? now - anim_.lastTickMs : kFrameMs;
  anim_.lastTickMs = now;

  // 夹住 dt：窗口消息卡顿时一帧走完会跳；太小则几乎不动。
  const float dt = static_cast<float>((std::max)(int64_t{1}, (std::min)(elapsed, int64_t{64})));
  const float alpha = 1.0f - std::exp(-dt / kFollowTauMs);

  // 在对数倍数上收敛：滚轮一格是乘法，对数空间里等距收敛让放大/缩小
  // 的快慢观感一致（线性收敛时缩小那一侧会显得拖沓）。
  const float fromLog = std::log(zoom_ > 0.0f ? zoom_ : minZoom());
  const float toLog = std::log(anim_.toZoom);
  const float nextLog = fromLog + (toLog - fromLog) * alpha;
  zoom_ = std::exp(nextLog);

  // 锚定不变式：当前倍数下锚点底下的像素必须钉住。每帧用「此刻的 pan」
  // 反解，而不是累乘一个历史 delta——累乘会把每帧的浮点误差叠成可见漂移。
  const float ratio = anim_.fromZoom > 0.0f ? zoom_ / anim_.fromZoom : 1.0f;
  pan_.x = anim_.anchorX - anim_.deltaX * ratio;
  pan_.y = anim_.anchorY - anim_.deltaY * ratio;

  // 剩余量小到看不出来就落位：继续排帧只是空耗一次重绘
  if (std::fabs(toLog - nextLog) < 0.0005f) {
    finishAnimation();
    return;
  }

  updateGridFade();
  tickGridFade();
  dropPixelCache();
  invalidate();
  postDelayed(kFrameMs);
}

void PhotoView::finishAnimation() {
  if (anim_.kind == AnimKind::kNone) return;

  const bool follow = anim_.follow;
  zoom_ = anim_.toZoom;
  displayAngle_ = anim_.toAngle;
  // 跟随模式的 toPan 只是「开这一格时的估算」，连续滚动后早已过时；
  // 落位时用同一套锚定不变式重算，否则收尾这一帧画面会平移一下。
  if (follow) {
    const float ratio = anim_.fromZoom > 0.0f ? anim_.toZoom / anim_.fromZoom : 1.0f;
    pan_.x = anim_.anchorX - anim_.deltaX * ratio;
    pan_.y = anim_.anchorY - anim_.deltaY * ratio;
  } else {
    pan_ = anim_.toPan;
  }
  const bool refit = anim_.refit;
  anim_ = Animation{};

  // 角度必须落回 90° 的整数倍：命中与像素格都按 rotation_ 算，
  // 停在 87° 会让之后每次判断都偏一点。
  displayAngle_ = static_cast<float>(rotation_ * 90);
  if (refit) {
    zoom_ = fitZoomFor(rotation_);
    pan_ = PanF{};
    fitToWindow_ = true;
  }
  dropPixelCache();
  clampPan();
  // 落位后网格的目标可能变了（比如刚跨过阈值），从这里接着淡入
  updateGridFade();
  tickGridFade();
  invalidate();
  notifyChanged();
}

void PhotoView::onDelayed() {
  tickAnimation();
  // 缩放动画结束后淡入可能还没走完：继续按帧推进，直到它停下
  bool more = false;
  if (anim_.kind == AnimKind::kNone) {
    if (tickGridFade()) more = true;
    if (tickZoomBadge()) more = true;
  }
  if (more) {
    invalidate();
    postDelayed(kFrameMs);
  }
}

// ───────────────────────── 几何 ─────────────────────────

void PhotoView::clampPan() {
  const Rect rc = clientRect();
  if (rc.isEmpty()) return;
  const Size rotated = rotatedSize();
  const float drawnW = rotated.width * zoom_;
  const float drawnH = rotated.height * zoom_;
  const float limitX = panLimit(drawnW, static_cast<float>(rc.width()));
  const float limitY = panLimit(drawnH, static_cast<float>(rc.height()));
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

bool PhotoView::mousePixel(Point& outPixel) const {
  // 光标没进过控件、或已经从控件里出去：没有「指着哪个像素」可言
  if (!hasLastMouse_) return false;
  return sourcePixelAt(lastMouse_, outPixel);
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

// 网格是否处于「该显示」的状态。这里看的是目标而不是当前 alpha：
// 跨过阈值就立即为真，之后在 kFadeMs 内淡入。若返回的是当前 alpha，
// 「1000% 就有网格」会在刚跨过的那一瞬间被判成 false。
bool PhotoView::isPixelGridVisible() const {
  return sourceSize_.width > 0 && zoom_ >= gridZoom_ && anim_.kind != AnimKind::kRotate;
}

float PhotoView::gridFade() const {
  // 网格该不该显示只由 zoom_ 与旋转动画决定，看到的值由「跨越时刻过了多久」
  // 解析算出：
  //   - 目标在阈值处就翻到 1，所以 1000% 一到就是满格，而不是半透明；
  //   - 翻的那一帧起点是当时的值，之后 kFadeMs 内走过去，不是啪一下出现。
  // 不逐帧累加：未挂到窗口时 postDelayed 是空操作，累加会让它永远停在 0。
  if (!gridFading_) return gridAlphaTo_;

  const int64_t elapsed = platform()->monotonicMillis() - gridFadeStartMs_;
  const float raw = kFadeMs > 0 ? static_cast<float>(elapsed) / static_cast<float>(kFadeMs) : 1.0f;
  if (raw >= 1.0f) return gridAlphaTo_;
  // 与缩放动画同一条缓动曲线：两者同时在跑时观感才是统一的
  return lerp(gridAlphaFrom_, gridAlphaTo_, easeInOut(raw));
}

void PhotoView::updateGridFade() {
  // 旋转动画期间一律关掉：网格按 rotation_ 的 ×90° 算坐标，而那时图像是按
  // displayAngle_ 画的，两者不同步，网格会整体错位。这里直接钉到 0，
  // 不走淡出——错位的网格多留一帧都不该有，而且旋转本身的动作已经占满视觉。
  if (anim_.kind == AnimKind::kRotate) {
    gridAlphaFrom_ = 0.0f;
    gridAlphaTo_ = 0.0f;
    gridFading_ = false;
    return;
  }

  const bool want = sourceSize_.width > 0 && zoom_ >= gridZoom_;
  const float target = want ? 1.0f : 0.0f;
  // 目标没变就什么都不做。注意不能拿 gridFade() 当「当前值」判等：它随时间
  // 在动，每帧都不同，会被当成新目标反复重启动。
  if (std::fabs(target - gridAlphaTo_) < 0.0001f) return;

  // 起点取「此刻的值」而不是 0：连续滚轮时目标反复翻转，
  // 从当前值接着走才是连续的，否则每次翻转都会跳回透明。
  gridAlphaFrom_ = gridFade();
  gridAlphaTo_ = target;
  gridFadeStartMs_ = platform()->monotonicMillis();
  if (gridFading_) return;  // 已经在逐帧推进，改起点就够了
  gridFading_ = true;
  // 自己没有定时器：借动画那条单次定时器链路，tick 里再排下一拍
  postDelayed(kFrameMs);
}

bool PhotoView::tickGridFade() {
  if (!gridFading_) return false;
  // 到点就收尾，把状态定死，之后 gridFade() 直接返回目标值
  if (platform()->monotonicMillis() - gridFadeStartMs_ >= kFadeMs) {
    gridFading_ = false;
    return false;
  }
  return true;
}

bool PhotoView::sourcePixelAt(Point viewPoint, Point& outPixel) const {
  if (sourceSize_.width <= 0 || sourceSize_.height <= 0 || zoom_ <= 0.0f) return false;
  // 与 visibleSourceRectAt 同一套反解：视图点 → 图像坐标。
  // 绘制用的是整数平移（translate 只收 int），这里也用同一个取整，否则
  // 图边缘那半个像素上会出现「看着在图上、却判成图外」的不一致。
  const Point center = clientRect().center();
  const float ox = static_cast<float>(viewPoint.x) -
                   static_cast<float>(static_cast<int>(std::lround(
                       static_cast<float>(center.x) + pan_.x)));
  const float oy = static_cast<float>(viewPoint.y) -
                   static_cast<float>(static_cast<int>(std::lround(
                       static_cast<float>(center.y) + pan_.y)));
  float lx = 0.0f;
  float ly = 0.0f;
  unrotateOffset90(ox, oy, rotation_, lx, ly);
  const float ix = lx / zoom_ + sourceSize_.width / 2.0f;
  const float iy = ly / zoom_ + sourceSize_.height / 2.0f;
  if (ix < 0.0f || iy < 0.0f || ix >= static_cast<float>(sourceSize_.width) ||
      iy >= static_cast<float>(sourceSize_.height)) {
    return false;
  }
  outPixel = Point{static_cast<int>(std::floor(ix)), static_cast<int>(std::floor(iy))};
  return true;
}

bool PhotoView::isOnImage(Point viewPoint) const {
  Point ignored;
  return sourcePixelAt(viewPoint, ignored);
}

// ───────────────────────── 像素格 ─────────────────────────

void PhotoView::paintPixelGrid(Painter& painter, const Rect& view, float alpha) {
  if (!pixelProvider_ || sourceSize_.width <= 0 || alpha <= 0.0f) return;
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
    // 缓存矩形必须按「实际拿到的尺寸」记，不能直接记请求的 region：
    // 宿主那一侧通常会把 region 裁到图片边界内（AiPic 的
    // AppReadPixelRegion 就是先 intersect 再拷贝），返回的块可能比请求的小。
    // 两者一旦不一致，下面的下标 (x−left) 就会越界，越界的格子被 continue
    // 掉——表现正是「屏幕上一部分格子有字、一部分没有」，而且越界还可能是
    // 静默读到下一行的错误像素。
    if (got.width != region.width() || got.height != region.height()) {
      got.width = (std::min)(got.width, region.width());
      got.height = (std::min)(got.height, region.height());
      if (got.width <= 0 || got.height <= 0 ||
          got.pixels.size() < static_cast<size_t>(got.width) * static_cast<size_t>(got.height)) {
        dropPixelCache();
        return;
      }
    }
    pixelCache_ = std::move(got);
    // 取块的左上角与请求的 region 对齐（宿主是按 region.left/top 起的）
    pixelCacheRect_ =
        Rect{region.left, region.top, region.left + pixelCache_.width,
             region.top + pixelCache_.height};
  }
  if (pixelCache_.isEmpty()) return;

  const Point center = view.center();
  // 与 onPaint 的 translate 保持完全一致：网格线必须落在实际绘制出的像素
  // 边界上，否则网格一出现就会整体偏半个像素——看起来就是「开了网格图就
  // 跳了一下」。两处都按同一个整数基准取整。
  const float cx = static_cast<float>(
      static_cast<int>(std::lround(static_cast<float>(center.x) + pan_.x)));
  const float cy = static_cast<float>(
      static_cast<int>(std::lround(static_cast<float>(center.y) + pan_.y)));
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

  // 格子底色：逐格填实。
  //
  // 不能指望上面那张缩放位图与网格线对齐：后端拿到的是整数 dst 矩形，非整数
  // 倍时内部步长是 dstW/srcW，格内边界与 toView() 的 lround 最多差 1 像素——
  // 网格线外侧就漏出邻居颜色的一条细边，看着像「填充跟格线偏了」。
  // 逐格填色与网格线共用同一套 toView()，边界严丝合缝；填色又和格子里的
  // RGB 数字取自同一像素，不会再出现「数字是 86、看到的却是别的色」。
  //
  // 格子还小的时候不填：此时格子只有十来个像素，逐格 fillRect 一屏上万次，
  // 换来的只是「和缩放位图看不出的差别」，白花绘制时间。
  const float valueFade = cellValueFade(cell);
  if (valueFade > 0.0f) {
    const std::uint8_t cellAlpha =
        static_cast<std::uint8_t>(static_cast<float>(alpha) * valueFade * 255.0f + 0.5f);
    for (int y = pixelCacheRect_.top; y < pixelCacheRect_.bottom; ++y) {
      for (int x = pixelCacheRect_.left; x < pixelCacheRect_.right; ++x) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        toView(static_cast<float>(x), static_cast<float>(y), x0, y0);
        toView(static_cast<float>(x + 1), static_cast<float>(y + 1), x1, y1);
        const Rect rc{(std::min)(x0, x1), (std::min)(y0, y1), (std::max)(x0, x1),
                      (std::max)(y0, y1)};
        if (rc.isEmpty() || rc.intersect(view).isEmpty()) continue;

        const size_t at =
            static_cast<size_t>(y - pixelCacheRect_.top) *
                static_cast<size_t>(pixelCache_.width) +
            static_cast<size_t>(x - pixelCacheRect_.left);
        if (at >= pixelCache_.pixels.size()) continue;
        const Color px = pixelCache_.pixels[at];
        painter.fillRect(rc, makeColor(colorRed(px), colorGreen(px), colorBlue(px), cellAlpha));
      }
    }
  }

  // 网格线：只在格边长够大时画，太密会糊成一片灰。
  // 半透明黑压在任意像素上都看得见，不必按图片明暗换色。
  //
  // 线的长度必须夹在「图像在视图里占的那一块」之内：整行整列地画到
  // view.top/view.bottom 会让线跑出图片，铺到画布背景上——那不是网格，
  // 是几十条横贯窗口的灰线。
  //
  // alpha 跟着倍数淡入：整层网格一起从透明到实色，不是啪一下出现。
  const Color lineColor = makeColor(0x10, 0x10, 0x10,
                                    static_cast<std::uint8_t>(
                                        static_cast<float>(0x66) * alpha + 0.5f));
  for (int x = region.left; x <= region.right; ++x) {    int x0 = 0;
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

  // 格子里的 RGB 值：三行竖直排（R、G、B），字号跟着格子大小走。
  //
  // 为什么不横排成 "255,255,255"：横向要六七十像素，而格子在 1000% 时只有
  // 十来个像素宽，永远放不进去，结果就是「有字的地方没几格」。
  // 竖排后每行只要 3 位数字，宽度需求降到三分之一。
  paintCellValues(painter, view, alpha);
}

void PhotoView::paintCellValues(Painter& painter, const Rect& view, float alpha) {
  if (!pixelProvider_ || pixelCache_.isEmpty() || alpha <= 0.0f) return;

  const float cell = zoom_;
  // 格子不够大就不画数字：三行各占一格的三分之一，格子十来个像素时 em 只有
  // 三四个像素，糊成一片灰点，读不出任何值——那还不如留干净的格子。
  // 放大跨过阈值时按格子边长淡入，不会突然冒出一屏数字。
  const float valueFade = cellValueFade(cell);
  if (valueFade <= 0.0f) return;

  const Point center = view.center();
  const float cx = static_cast<float>(
      static_cast<int>(std::lround(static_cast<float>(center.x) + pan_.x)));
  const float cy = static_cast<float>(
      static_cast<int>(std::lround(static_cast<float>(center.y) + pan_.y)));
  const float halfW = sourceSize_.width * cell / 2.0f;
  const float halfH = sourceSize_.height * cell / 2.0f;
  const int turns = normalizeTurns(rotation_);

  auto toView = [&](float x, float y, int& outX, int& outY) {
    float rx = 0.0f;
    float ry = 0.0f;
    rotateOffset90(x * cell - halfW, y * cell - halfH, turns, rx, ry);
    outX = static_cast<int>(std::lround(cx + rx));
    outY = static_cast<int>(std::lround(cy + ry));
  };

  // 字号与行距：格子竖着放三行数字（R、G、B）。
  //
  // 行距不能直接用 cell/3：数字的可见字高只有约 0.7em，按 1em 行距排下来，
  // 行与行之间空出 0.3em，三行读不成一个三元组，看着就是「散」。
  // 取 0.86em 让行间只留一条窄缝，同时不重叠。
  int textPx = static_cast<int>(cell / 3.0f);
  textPx = (std::min)(kCellTextMaxPx, textPx);
  const Font& font = fonts.forPixelSize(textPx);
  if (!font.isValid()) return;

  // 三行整体作为一块在格内居中：块高就是三行行框之和。
  // 不能写成「两段行距 + 一行字高」：每行的行框都是 pitch 高，三行实际占
  // pitch*3，用别的算式居中会让整块偏上或偏下几个像素。
  const int pitch = (std::max)(1, static_cast<int>(std::lround(static_cast<float>(textPx) * 0.86f)));
  const int blockHeight = pitch * 3;
  // 行框内后端按「行高（含降部）」居中，而数字没有降部，字形整体偏上约
  // 0.07em。三行叠起来视觉重心就偏离格心，这里按字号补回去。
  const int visualShift = static_cast<int>(std::lround(static_cast<float>(textPx) * 0.07f));

  for (int y = pixelCacheRect_.top; y < pixelCacheRect_.bottom; ++y) {
    for (int x = pixelCacheRect_.left; x < pixelCacheRect_.right; ++x) {
      int x0 = 0;
      int y0 = 0;
      int x1 = 0;
      int y1 = 0;
      toView(static_cast<float>(x), static_cast<float>(y), x0, y0);
      toView(static_cast<float>(x + 1), static_cast<float>(y + 1), x1, y1);
      const Rect cellRect{(std::min)(x0, x1), (std::min)(y0, y1), (std::max)(x0, x1),
                          (std::max)(y0, y1)};
      // 整格落在视图外就跳过，避免画到边界外的无效文字
      if (cellRect.intersect(view).isEmpty()) continue;

      const size_t at =
          static_cast<size_t>(y - pixelCacheRect_.top) *
              static_cast<size_t>(pixelCache_.width) +
          static_cast<size_t>(x - pixelCacheRect_.left);
      if (at >= pixelCache_.pixels.size()) continue;
      const Color px = pixelCache_.pixels[at];

      // 亮像素用深字、暗像素用浅字：否则总有一半格子读不出来。
      // 文字跟着 alpha 淡入，与网格线同步，不会出现「线到了字没到」。
      // valueFade 让文字随格子放大一起变实，小格子时几乎看不见。
      const int luma =
          (colorRed(px) * 299 + colorGreen(px) * 587 + colorBlue(px) * 114) / 1000;
      const Color base = luma > 140 ? makeColor(0x12, 0x12, 0x12) : makeColor(0xf6, 0xf6, 0xf6);
      const Color ink =
          makeColor(colorRed(base), colorGreen(base), colorBlue(base),
                    static_cast<std::uint8_t>(static_cast<float>(colorAlpha(base)) * alpha *
                                                  valueFade + 0.5f));

      // 三行按固定行距竖直排，整块在格内居中。行框高 = 行距，
      // 后端在行框里垂直居中字形，所以每条数字都落在自己的行距格中央。
      const int rows[3] = {colorRed(px), colorGreen(px), colorBlue(px)};
      const int blockTop =
          cellRect.top + (cellRect.height() - blockHeight) / 2 + visualShift;
      for (int i = 0; i < 3; ++i) {
        const int top = blockTop + pitch * i;
        const Rect line{cellRect.left, top, cellRect.right, top + pitch};
        if (line.isEmpty()) continue;
        painter.drawText(line, std::to_wstring(rows[i]), ink, font, TextAlign::kCenter);
      }
    }
  }
}

// ───────────────────────── 绘制 ─────────────────────────

void PhotoView::onPaint(Painter& painter, const Rect& rect) {
  // 网格的目标状态每次绘制前对齐一次：缩放、旋转、换图都从这里过，
  // 比在每个改状态的地方各补一句可靠（漏一处就会出现网格不跟着变）。
  updateGridFade();
  updateZoomBadge();

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
  //
  // 取整基准必须与网格线/格子文字的 toView() 同源（都是 lround(center+pan+off)）。
  // 这里是「先算相对坐标、后端再 translate」，所以先把 tx 减掉，使
  // dstSub + tx 恰好等于 lround(cx + off)，两边边界才落在同一个整数上。
  //
  // 注意这只能保证**外框**对齐：后端把 srcW 个源像素铺满 dstW 个设备像素，
  // 内部步长是 dstW/srcW，而 dstW 各自取整后与 srcW*zoom 最多差 1，于是
  // 非整数倍时内部边界最多漂 1 像素（整数倍如 1000% 是精确的）。
  // 这是整数 dst 矩形的固有误差，前端无法再用取整消除；要彻底消掉得改成
  // 逐像素 fillRect，一屏上万次绘制不划算。
  const int tx = static_cast<int>(std::lround(static_cast<float>(center.x) + pan_.x));
  const int ty = static_cast<int>(std::lround(static_cast<float>(center.y) + pan_.y));
  auto dstOf = [&](double imageX) {
    return static_cast<int>(
               std::llround(static_cast<double>(center.x) + static_cast<double>(pan_.x) +
                            (static_cast<double>(imageX) / ratio * zoom_ - fullW / 2.0))) -
           tx;
  };
  auto dstOfY = [&](double imageY) {
    return static_cast<int>(
               std::llround(static_cast<double>(center.y) + static_cast<double>(pan_.y) +
                            (static_cast<double>(imageY) / ratio * zoom_ - fullH / 2.0))) -
           ty;
  };
  const Rect dstSub{dstOf(source.left), dstOfY(source.top), dstOf(source.right),
                    dstOfY(source.bottom)};

  painter.save();
  // 平移量是浮点（动画逐帧反解出来的），但后端 translate 只收整数。
  // 这里四舍五入而不是截断：截断会让画面整体偏半个像素，放大时看得出来。
  // 与上面 dstOf 用的 tx/ty 是同一对值——两处一旦分别计算就可能差 1 像素。
  painter.translate(tx, ty);
  if (std::fabs(angle) > 0.001f) painter.rotate(angle);

  // 放大到能看清像素时用最近邻：插值会把像素格抹成一团，那不是用户想看的。
  // 但两种采样方式之间不能硬切——同一帧里锐度会跳一下。用交叉淡化：
  // 平滑版按 (1−fade) 淡出、最近邻版按 fade 淡入，过渡带里两层叠着画。
  // 代价只发生在过渡的那一格倍数上，进入网格后只剩一层。
  const float fade = sharpenFade();
  if (fade <= 0.0f) {
    painter.drawImageScaled(bitmap, dstSub, source, ImageSmoothing::kSmooth);
  } else if (fade >= 1.0f) {
    painter.drawImageScaled(bitmap, dstSub, source, ImageSmoothing::kNearest);
  } else {
    painter.drawImageScaled(bitmap, dstSub, source, ImageSmoothing::kSmooth, 1.0f - fade);
    painter.drawImageScaled(bitmap, dstSub, source, ImageSmoothing::kNearest, fade);
  }
  painter.restore();

  // 像素格与 RGB 值叠在图之上：只在静止且放大够多时画，跨阈值时淡入
  const float grid = gridFade();
  if (grid > 0.0f) paintPixelGrid(painter, rect, grid);

  // 缩放比浮层压在最上层：它是临时的读数，不该被任何内容挡住
  paintZoomBadge(painter, rect);
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
  if (std::fabs(target - zoom_) < 0.0005f && std::fabs(pan_.x) < 0.01f &&
      std::fabs(pan_.y) < 0.01f)
    return;
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
  cursorBeforeDrag_ = cursorShape();
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
  // 只有按住拖拽时才用握起的手，其余时间保持宿主设的光标（默认箭头）
  setCursor(cursorBeforeDrag_);
}

void PhotoView::onMouseMove(const MouseEvent& e) {
  // 随时记鼠标位置：工具栏的放大按钮也以它为中心
  // 状态栏显示的是「指针下的像素」，所以位置真的变了就要通知宿主刷新
  const bool moved = !hasLastMouse_ || e.position.x != lastMouse_.x ||
                     e.position.y != lastMouse_.y;
  lastMouse_ = e.position;
  hasLastMouse_ = true;

  if (!dragging_) {
    if (moved) notifyChanged();
    return;
  }
  pan_ = PanF{dragStartPan_.x + static_cast<float>(e.position.x - dragOrigin_.x),
              dragStartPan_.y + static_cast<float>(e.position.y - dragOrigin_.y)};
  // 拖动过程中不做 clamp：一旦夹住，用户往回拖时要等鼠标走完这一段才动，
  // 手感像卡住。松手时再夹。
  dropPixelCache();
  invalidate();
  notifyChanged();
}

void PhotoView::onMouseLeave() {
  const bool wasInside = hasLastMouse_;
  hasLastMouse_ = false;
  if (dragging_) {
    dragging_ = false;
    releaseMouse();
    clampPan();
    setCursor(cursorBeforeDrag_);
    invalidate();
    notifyChanged();
  } else if (wasInside) {
    // 指针离开后状态栏不该继续显示最后那个像素：那是已经不再指着的位置
    notifyChanged();
  }
}

bool PhotoView::onMouseWheel(const MouseEvent& e) {
  if (!image_.isValid() || e.wheelDelta == 0) return false;
  // 光标在画布空白处时不动：锚点在图上才有「放大哪一点」可言，拿空白处当锚点
  // 只会把图整块推出视野，看起来就像位置突变。
  if (!isOnImage(e.position)) return false;
  // 锚在光标位置：滚轮放大时那个像素必须留在原地
  lastMouse_ = e.position;
  hasLastMouse_ = true;
  const float steps = static_cast<float>(e.wheelDelta) / 120.0f;
  const float factor = std::pow(kWheelStep, steps);
  // 宿主没接管时走滚轮专用路径：更短、起步就快。宿主接管了（例如自己
  // 记倍数）仍按它给的 factor 走 zoomBy，保持既有约定不变。
  if (onWheelZoom_) onWheelZoom_(factor);
  else wheelZoomBy(factor);
  return true;
}

void PhotoView::onDoubleClick(const MouseEvent& e) {
  if (!image_.isValid()) return;
  // 同上：双击空白处不该缩放
  if (!isOnImage(e.position)) return;
  lastMouse_ = e.position;
  hasLastMouse_ = true;
  // 双击在「整图」与「原始大小」之间切换：这两个是所有看图软件的共同预期
  if (zoom_ < 0.999f) {
    zoomToActual();
    return;
  }
  zoomToFit();
}

void PhotoView::onContextMenu(const MouseEvent& e) {
  // 只有进了网格模式才复制颜色：那是「用户在看某一个像素」的唯一明确信号。
  // 没开网格时右键多半是想弹菜单，抢掉会让人莫名其妙。
  if (!onColorCopy_ || !isPixelGridVisible()) return;
  Point pixel;
  if (!sourcePixelAt(e.position, pixel)) return;

  // 宿主返回的就是要提示的文本，控件不必知道剪贴板里放了什么格式
  const std::wstring message = onColorCopy_(pixel);
  if (message.empty()) return;
  // 提示交给窗口级的 Toast：它画在整棵树之上，也由 ViewRoot 自己驱动淡出，
  // 不必让图片控件替它排帧
  if (ViewRoot* r = root()) r->showToast(message);
}

// ───────────────────────── 缩放比浮层 ─────────────────────────

void PhotoView::setZoomBadgeVisible(bool visible) {
  if (zoomBadgeVisible_ == visible) return;
  zoomBadgeVisible_ = visible;
  if (!visible) {
    badgeFade_ = 0.0f;
    badgeAlphaFrom_ = 0.0f;
    badgeAlphaTo_ = 0.0f;
    badgeFading_ = false;
  }
  invalidate();
}

// 浮层的显示程度。与网格淡入同一套路子：目标按「是否在缩放」翻，
// 当前值由「翻的那一刻过了多久」解析算出，不逐帧累加。
float PhotoView::zoomBadgeFade() const {
  if (!badgeFading_) return badgeAlphaTo_;
  const int64_t elapsed = platform()->monotonicMillis() - badgeFadeStartMs_;
  const float raw = kBadgeFadeMs > 0
                        ? static_cast<float>(elapsed) / static_cast<float>(kBadgeFadeMs)
                        : 1.0f;
  if (raw >= 1.0f) return badgeAlphaTo_;
  return lerp(badgeAlphaFrom_, badgeAlphaTo_, easeInOut(raw));
}

void PhotoView::updateZoomBadge() {
  // 缩放进行中才显示：静止时的倍数状态栏已经有了，屏幕上再挂一块只是噪音
  const bool want = zoomBadgeVisible_ && sourceSize_.width > 0 &&
                    anim_.kind == AnimKind::kZoom;
  const float target = want ? 1.0f : 0.0f;
  if (std::fabs(target - badgeAlphaTo_) < 0.0001f) return;

  // 起点取此刻的值：连续缩放时目标反复翻转，从当前值接着走才连续
  badgeAlphaFrom_ = zoomBadgeFade();
  badgeAlphaTo_ = target;
  badgeFadeStartMs_ = platform()->monotonicMillis();
  if (badgeFading_) return;
  badgeFading_ = true;
  postDelayed(kFrameMs);
}

bool PhotoView::tickZoomBadge() {
  if (!badgeFading_) return false;
  if (platform()->monotonicMillis() - badgeFadeStartMs_ >= kBadgeFadeMs) {
    badgeFading_ = false;
    return false;
  }
  return true;
}

void PhotoView::paintZoomBadge(Painter& painter, const Rect& view) {
  const float alpha = zoomBadgeFade();
  if (alpha <= 0.0f) return;

  // 倍数文本：与状态栏一样按百分比。缩放中途值每帧都变，所以每帧重算，
  // 不做缓存——这里只有一次整数格式化和一次阴影测量，代价可以忽略。
  const int percent = static_cast<int>(zoom_ * 100.0f + 0.5f);
  const std::wstring text = std::to_wstring(percent) + L"%";

  const int dpi = (std::max)(96, this->dpi());
  auto px = [dpi](int designPx) { return designPx * dpi / 96; };

  const int textPx = px(kBadgeTextPx);
  const Font& font = fonts.forPixelSize(textPx);
  if (!font.isValid()) return;

  // 跑道形：左右圆角半径取高度的一半，两端就是半圆。
  // 用字体行高估宽度，再留出左右内边距——不调 measureText 是因为这个
  // 后端没有暴露文本测量，估宽在这里足够（文本只有 3~5 个字符）。
  const int padX = px(kBadgePadX);
  const int padY = px(kBadgePadY);
  const int textWidth = static_cast<int>(text.size()) * textPx * 65 / 100;
  const int boxW = textWidth + padX * 2;
  const int boxH = textPx + padY * 2;
  if (boxW <= 0 || boxH <= 0) return;

  const Rect box{view.center().x - boxW / 2, view.center().y - boxH / 2,
                 view.center().x - boxW / 2 + boxW, view.center().y - boxH / 2 + boxH};
  const int radius = boxH / 2;

  const std::uint8_t bgAlpha =
      static_cast<std::uint8_t>(static_cast<float>(kBadgeBgAlpha) * alpha + 0.5f);
  const std::uint8_t fgAlpha =
      static_cast<std::uint8_t>(static_cast<float>(kBadgeFgAlpha) * alpha + 0.5f);
  painter.fillRoundRect(box, makeColor(0, 0, 0, bgAlpha), radius);
  painter.drawText(box, text, makeColor(0xff, 0xff, 0xff, fgAlpha), font, TextAlign::kCenter);
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
