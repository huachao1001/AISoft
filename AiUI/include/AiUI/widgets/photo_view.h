// PhotoView：可缩放、平移、旋转的图片查看控件
//
// 与 ImageView 的分工：ImageView 只做「等比塞进目标矩形」的静态展示；
// 看图中用户要能放大到像素级、拖动看局部、转个方向，那些交互都收在这里。
//
// 变换模型：图像坐标 → 视图坐标只有一条链——
//   scale（每图像像素多少视图像素）+ 90° 倍数旋转 + 平移。
// 旋转固定是 90° 的整数倍：任意角度要重采样，看图的清晰度会受损，而用户
// 要的「转一下」几乎都是竖直/横置。
//
// 超大图：两万像素见方的图整张解码就是上 GB。控件因此同时持有全尺寸位图与
// 缩略代理，动画期间只画代理；放得更大需要细节时才换全尺寸，而那时可见
// 区域已经很小，后端只需处理屏幕内那一块。
#pragma once

#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/view.h"

#include <cstdint>
#include <functional>
#include <string>

namespace AiUI {

class PhotoView : public View {
 public:
  PhotoView();
  ~PhotoView() override;

  // 从文件加载；失败返回 false 并清空当前图
  bool loadFile(const std::wstring& path);
  // 直接给一张已解码的位图（旋转后重建时用）
  void setImage(const Image& image);
  void clearImage();

  bool hasImage() const { return image_.isValid(); }
  // 图像原始像素尺寸
  Size imageSize() const { return sourceSize_; }
  const std::wstring& path() const { return path_; }

  // 缩放倍数：1.0 = 一个图像像素对应一个视图像素（物理像素）
  float zoom() const { return zoom_; }
  // 缩放，锚点为上一次鼠标位置（没有则视图中心）
  void setZoom(float zoom);
  // 以指定锚点为中心缩放。「鼠标在哪就放大哪」是看图的基本预期：
  // 光标下的像素必须钉在原地，否则每滚一格图都从光标底下溜走。
  void setZoomAt(float zoom, Point anchor);
  // 适应窗口：整张图放得下，不放大。带动画，终态是「跟随窗口」
  void zoomToFit();
  // 缩放到 100%（物理像素 1:1）
  void zoomToActual();
  // 在当前倍数上乘以 factor，围绕锚点
  void zoomBy(float factor);
  // 是否处于「适应窗口」状态：此状态下视图尺寸变化时自动重算倍数。
  // 用户一旦自己缩放就退出该状态——否则拖大窗口会把他刚定的倍数作废。
  bool isFitToWindow() const { return fitToWindow_; }

  // 旋转：每调用一次转 90°，顺时针为正。带动画
  void rotate(int quarterTurns);
  int rotation() const { return rotation_; }

  // 平移量（视图像素）；0 表示图像中心对齐视图中心。
  // 内部用浮点存：动画逐帧反解出来的平移是小数，先取整再画会让整幅图在
  // 每帧之间抖 ±1 像素（放大后尤其明显），也破坏「锚点像素不动」。
  Point pan() const {
    return Point{static_cast<int>(std::lround(pan_.x)), static_cast<int>(std::lround(pan_.y))};
  }
  void setPan(Point pan);

  // ── 动画 ──
  // 动画进行中：绘制改用缩略代理，并跳过逐像素叠加层
  bool isAnimating() const { return anim_.kind != AnimKind::kNone; }
  // 动画时长（毫秒）；0 表示关闭动画、立即到位
  void setAnimationDuration(int ms) { animMs_ = ms; }
  int animationDuration() const { return animMs_; }

  // ── 像素检查 ──
  // 放大到能看清单个像素时，在像素格上叠网格与 RGB 值。界面上的 Image 只是
  // 后端句柄取不到内容，所以由宿主按区域提供像素：只索取可见的那一小块，
  // 巨图不必整张解码。返回 false 表示这块取不到，网格就不画。
  using PixelRegionProvider = std::function<bool(const Rect& region, RgbaImage& out)>;
  void setPixelRegionProvider(PixelRegionProvider fn) { pixelProvider_ = std::move(fn); }
  // 当前倍数是否已经进入像素网格模式
  bool isPixelGridVisible() const;
  // 进入像素网格的倍数阈值。默认 10 倍（界面上显示 1000%）：再小格子不到
  // 10 像素，网格线和 RGB 数字都塞不进去，画出来只是脏。
  void setPixelGridZoom(float zoom) { gridZoom_ = zoom; }
  float pixelGridZoom() const { return gridZoom_; }

  // 视图点底下是否有图像像素。缩放只看图上的点：光标停在画布空白处时
  // 以它当锚点会把整幅图推出视野，看起来就是「一滚就飞了」。
  bool isOnImage(Point viewPoint) const;

  // 双击与滚轮的行为在宿主里决定，这里只报事件
  using ZoomHandler = std::function<void(float factor)>;
  void setWheelZoomHandler(ZoomHandler fn) { onWheelZoom_ = std::move(fn); }
  // 平移、缩放、旋转变化后通知宿主刷新状态栏
  using ChangeHandler = std::function<void()>;
  void setChangeHandler(ChangeHandler fn) { onChange_ = std::move(fn); }

  // 视图中心处的图像像素坐标：状态栏与取色都要用
  Point centerPixel() const;
  // 图像尺寸按当前旋转后的宽高
  Size rotatedSize() const;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  // 尺寸变化时若处于「适应窗口」，重算倍数
  void onLayout(const Rect& frame) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onMouseWheel(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  void onAttach() override;
  void onDelayed() override;
  Color backgroundColor() const override;

 private:
  // 一次只跑一种动画：缩放与旋转互斥，新动画把旧的直接落位。
  //
  // 缩放动画不插值 pan：pan 每帧都从「锚点下的像素钉住」这条不变式反解，
  // 这样动画全程光标底下那个像素都不动——缩放与锚定同时成立，而不是先缩放
  // 再微调（那样中途会明显滑一下）。
  //
  // 「适应窗口」不是独立动画：它就是一次以视图中心为锚、落位后再按当时尺寸
  // 重算倍数的缩放，靠 refit 标记区分。多一种动画就多一套互斥规则。
  enum class AnimKind { kNone, kZoom, kRotate };

  // 平移量的内部表示。必须是浮点：动画逐帧反解出来的是小数，先取整再画会让
  // 整幅图在相邻帧之间抖 ±1 像素，放大后肉眼可见；对外仍用整数 Point。
  struct PanF {
    float x = 0.0f;
    float y = 0.0f;
  };

  struct Animation {
    AnimKind kind = AnimKind::kNone;
    int64_t startMs = 0;
    int durationMs = 0;
    float fromZoom = 1.0f;
    float toZoom = 1.0f;
    float fromAngle = 0.0f;
    float toAngle = 0.0f;
    PanF fromPan{};
    PanF toPan{};
    // 缩放动画用：锚点相对视图中心的偏移，以及「锚点 − 视图中心 − 起始平移」。
    // 每帧由这两个量反解 pan，「光标下的像素钉住」因此在动画全程都成立。
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    // 落位后是否重新按「适应窗口」算一次（旋转在适应状态下要重算）
    bool refit = false;
  };

  // 缩放在动画中的目标倍数：连续滚轮要按目标累计，按中途值累计会丢格
  float targetZoom() const;
  // 把平移量夹到「图至少露出一点」的范围。夹的范围是连续的（见 panLimit），
  // 所以放大经过「刚好铺满」那一格时不会横跳。
  void clampPan();
  void notifyChanged();
  // 缩放锚点：鼠标在控件内就用鼠标位置，否则视图中心
  Point zoomAnchor() const;
  // 「适应窗口」在当前视图尺寸下的倍数（不放大）
  float fitZoomFor(int rotation) const;
  // 立即按当前尺寸重算「适应窗口」，不做动画
  void applyFitNow();
  // 立即以锚点为中心缩放（不动画）
  void applyZoomAt(float zoom, Point anchor);
  // 像素区缓存作废：倍数或平移一变，上一块就过时了
  void dropPixelCache();

  // 动画入口：缩放带锚点，适应窗口与旋转落位后还要再算一次
  void beginZoomTo(float zoom, Point anchor, int durationMs, bool refit);
  void beginRotate(int quarterTurns);
  void beginAnimation(const Animation& anim);
  void tickAnimation();
  void finishAnimation();

  // 视图矩形覆盖到的原图像素范围；放大时只画这一块
  Rect visibleSourceRect(const Rect& view) const;
  // 同上，但按任意角度（动画中途角度不是 90° 整倍数）；像素格用固定角度
  Rect visibleSourceRectAt(const Rect& view, float angleDeg) const;
  // 旋转后坐标 → 原图坐标
  Point toSource(int u, int v) const;
  // 原图坐标 → 旋转后坐标
  Point toRotated(int x, int y) const;
  // 旋转后图像坐标里的矩形 → 原图像素坐标
  Rect rotatedToSource(const Rect& inRotated) const;
  // 这一帧该用哪张位图：选出够用的最小那张
  void pickBitmap(Image& out, float& ratio) const;
  // 像素格 + RGB 值
  void paintPixelGrid(Painter& painter, const Rect& view);

  Image image_{};   // 全尺寸位图，需要细节时用
  Image proxy_{};   // 缩略代理，动画与低倍数时用；小图没有代理
  std::wstring path_;
  Size sourceSize_{};
  float zoom_ = 1.0f;
  // 代理像素 / 原图像素；0 表示没有代理
  float proxyRatio_ = 0.0f;
  // 处于「适应窗口」：视图尺寸变化时重算 zoom_，用户自己缩放则清掉
  bool fitToWindow_ = false;
  // 90° 的整数倍，取值 0..3；绘制时只做坐标变换，不重采样
  int rotation_ = 0;
  // 绘制用的角度：旋转动画期间不等于 rotation_ * 90
  float displayAngle_ = 0.0f;
  PanF pan_{};
  bool dragging_ = false;
  Point dragOrigin_{};
  PanF dragStartPan_{};
  // 按下前的光标：拖完要还原成宿主设的那个，而不是硬写箭头
  CursorShape cursorBeforeDrag_ = CursorShape::kArrow;

  // 上一次鼠标位置（控件局部坐标），滚轮与工具栏缩放都以它为锚
  Point lastMouse_{};
  bool hasLastMouse_ = false;

  Animation anim_{};
  int animMs_ = 180;

  PixelRegionProvider pixelProvider_;
  RgbaImage pixelCache_;
  Rect pixelCacheRect_{};
  float gridZoom_ = 10.0f;

  ZoomHandler onWheelZoom_;
  ChangeHandler onChange_;
};

}  // namespace AiUI
