// PhotoView 的几何与交互测试
//
// 这个控件几乎全部逻辑都在「视图坐标 ↔ 图像坐标」的换算上，肉眼看截图
// 很难判断某个像素到底偏了几个点。这里把关键不变式写成断言：
//   - 锚点缩放：光标下的那个图像像素，缩放前后必须落回同一位置
//   - 旋转：90° 倍数旋转后，同一个图像像素要按顺时针换到新位置
//   - clamp：图比视图小时不能被强行居中（那会破坏锚点）
//   - 代理：巨图要建缩略代理，动画期间用代理
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/layouts/frame_layout.h"
#include "AiUI/widgets/photo_view.h"

#include <cmath>
#include <memory>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct PhotoEnv {
  ScopedFakePlatform fake;
  PhotoEnv() { fonts.create(96); }
  ~PhotoEnv() { fonts.destroy(); }
};

MouseEvent mouseAt(int x, int y, MouseButton button = MouseButton::kNone) {
  MouseEvent e;
  e.position = Point{x, y};
  e.button = button;
  return e;
}

// 造一个装好图的 PhotoView：图源 200×100，视图 400×400，适应窗口
//
// 事件处理函数都是 protected（正常由 ViewRoot 分发），测试要直接戳，
// 所以派生一层只做转发的壳，不改变任何行为。
class TestPhotoView : public PhotoView {
 public:
  using PhotoView::onDelayed;
  using PhotoView::onMouseDown;
  using PhotoView::onMouseLeave;
  using PhotoView::onMouseMove;
  using PhotoView::onMouseUp;
  using PhotoView::onMouseWheel;
};

// 视图坐标 → 图像坐标（只对 90° 倍数旋转成立，测试用）
Point imageAt(const PhotoView& view, Point p) {
  const Size src = view.imageSize();
  const Rect rc = view.clientRect();
  const Point viewCenter = rc.center();
  const int turns = ((view.rotation() % 4) + 4) % 4;

  // V = C + pan + Rot(P·z − WH/2) 的逆
  const float ox = static_cast<float>(p.x - viewCenter.x - view.pan().x);
  const float oy = static_cast<float>(p.y - viewCenter.y - view.pan().y);
  float lx = ox;
  float ly = oy;
  switch (turns) {
    case 1: lx = oy; ly = -ox; break;   // Rot 的逆
    case 2: lx = -ox; ly = -oy; break;
    case 3: lx = -oy; ly = ox; break;
    default: break;
  }
  return Point{static_cast<int>(std::lround(lx / view.zoom() + src.width / 2.0f)),
               static_cast<int>(std::lround(ly / view.zoom() + src.height / 2.0f))};
}

}  // namespace

// ───────────────────────── 加载 ─────────────────────────

AIUI_TEST(photo_view_reports_failure_for_missing_image) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  AIUI_CHECK(!view.loadFile(L"nope.png"));
  AIUI_CHECK(!view.hasImage());
}

AIUI_TEST(photo_view_fit_does_not_enlarge_small_image) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  AIUI_CHECK(view.loadFile(L"a.png"));
  AIUI_CHECK(view.isFitToWindow());
  // 适应窗口只缩小不放大：小图保持 1:1，否则会被拉糊
  AIUI_CHECK(std::fabs(view.zoom() - 1.0f) < 0.001f);
  AIUI_CHECK_EQ(view.imageSize().width, 200);
  AIUI_CHECK_EQ(view.imageSize().height, 100);
}

AIUI_TEST(photo_view_fit_shrinks_large_image_to_fit) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{2000, 1000});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  AIUI_CHECK(view.loadFile(L"a.png"));
  // 400/2000 = 0.2，高度 1000*0.2=200 也放得下，取较小者
  AIUI_CHECK(std::fabs(view.zoom() - 0.2f) < 0.001f);
}

// ───────────────────────── 锚点缩放 ─────────────────────────

AIUI_TEST(photo_view_zoom_at_keeps_pixel_under_cursor) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);  // 几何断言只看终态，不等动画

  // 选一个偏离中心的光标位置：正中心的锚点即使算错也看不出来
  const Point cursor{260, 180};
  const Point before = imageAt(view, cursor);

  view.setZoomAt(4.0f, cursor);
  const Point after = imageAt(view, cursor);

  // 允许 1 像素的取整误差：pan 是整数，反解会四舍五入
  AIUI_CHECK(std::abs(before.x - after.x) <= 1);
  AIUI_CHECK(std::abs(before.y - after.y) <= 1);
  AIUI_CHECK(std::fabs(view.zoom() - 4.0f) < 0.001f);
}

AIUI_TEST(photo_view_zoom_at_keeps_pixel_when_image_smaller_than_view) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  // 图比视图小的时候最容易出错：clampPan 若强行居中，锚点就会滑走
  const Point cursor{170, 210};
  const Point before = imageAt(view, cursor);
  view.setZoomAt(3.0f, cursor);
  const Point after = imageAt(view, cursor);

  AIUI_CHECK(std::abs(before.x - after.x) <= 1);
  AIUI_CHECK(std::abs(before.y - after.y) <= 1);
}

AIUI_TEST(photo_view_wheel_zoom_is_cumulative) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);  // 动画开着，才能复现「按中途值累计」

  // 动画没走完就连滚两格：目标是 1.15^2，不是 1.15×中途值
  for (int i = 0; i < 2; ++i) {
    MouseEvent e = mouseAt(220, 200);
    e.wheelDelta = 120;
    view.onMouseWheel(e);
    env.fake.platform().advanceMillis(30);  // 只走一小段动画
  }
  // 落位后应等于 1.15^2 = 1.3225
  MouseEvent e = mouseAt(220, 200);
  e.wheelDelta = 0;
  view.onMouseWheel(e);
  env.fake.platform().advanceMillis(500);
  view.onDelayed();  // 逼一次落位

  AIUI_CHECK(std::fabs(view.zoom() - 1.3225f) < 0.02f);
}

// ───────────────────────── 旋转 ─────────────────────────

AIUI_TEST(photo_view_rotate_preserves_center_pixel) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setZoomAt(2.0f, Point{200, 200});  // 以中心放大，转动时不跑偏

  view.rotate(1);
  AIUI_CHECK_EQ(view.rotation(), 1);

  // 中心对齐时，视图中心在旋转前后都对应图像中心
  const Point center = view.clientRect().center();
  const Point pixel = imageAt(view, center);
  AIUI_CHECK(std::abs(pixel.x - 100) <= 2);
  AIUI_CHECK(std::abs(pixel.y - 50) <= 2);
}

AIUI_TEST(photo_view_rotate_is_modulo_four) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");

  for (int i = 0; i < 5; ++i) view.rotate(1);
  AIUI_CHECK_EQ(view.rotation(), 1);

  view.rotate(-1);
  AIUI_CHECK_EQ(view.rotation(), 0);
}

// ───────────────────────── 拖动 ─────────────────────────

AIUI_TEST(photo_view_drag_works_without_zooming_first) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{2000, 1000});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  // 能完全缩进视图的图不该拖动，先确保图比视图大
  view.setZoomAt(1.0f, Point{200, 200});
  const Point start = view.pan();

  AIUI_CHECK(view.onMouseDown(mouseAt(200, 200, MouseButton::kLeft)));
  view.onMouseMove(mouseAt(150, 170));
  view.onMouseUp(mouseAt(150, 170));

  AIUI_CHECK(view.pan().x < start.x || view.pan().y < start.y);
}

AIUI_TEST(photo_view_drag_uses_grab_cursor_and_switches_on_press) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  // 光标要经过 ViewRoot 才能落到窗口上，所以这里真的建一层根视图
  FrameLayout root;
  auto* view = new TestPhotoView();
  view->setAnimationDuration(0);
  root.addView(view, LayoutParams{}.withSize(400, 400));
  AIUI_CHECK(root.create(nullptr, Rect{0, 0, 400, 400}));
  view->loadFile(L"a.png");
  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK(window != nullptr);

  // 光标本就是在悬停时才落到窗口上：先模拟鼠标进入控件
  view->onMouseMove(mouseAt(200, 200));
  AIUI_CHECK(window->cursor() == CursorShape::kGrab);
  view->onMouseDown(mouseAt(200, 200, MouseButton::kLeft));
  AIUI_CHECK(window->cursor() == CursorShape::kGrabbing);
  view->onMouseUp(mouseAt(200, 200));
  AIUI_CHECK(window->cursor() == CursorShape::kGrab);
}

AIUI_TEST(photo_view_mouse_leave_releases_drag) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{2000, 1000});
  FrameLayout root;
  auto* view = new TestPhotoView();
  view->setAnimationDuration(0);
  root.addView(view, LayoutParams{}.withSize(400, 400));
  AIUI_CHECK(root.create(nullptr, Rect{0, 0, 400, 400}));
  view->loadFile(L"a.png");
  view->setZoomAt(1.0f, Point{200, 200});
  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK(window != nullptr);

  view->onMouseDown(mouseAt(200, 200, MouseButton::kLeft));
  AIUI_CHECK(window->captured());
  view->onMouseLeave();
  // 鼠标离开必须松掉捕获与握紧光标，否则用户回来时控件还以为在拖
  AIUI_CHECK(!window->captured());
  AIUI_CHECK(window->cursor() == CursorShape::kGrab);
}

// ───────────────────────── 代理位图 ─────────────────────────

AIUI_TEST(photo_view_builds_proxy_only_for_huge_images) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView small;
  small.layout(Rect{0, 0, 400, 400});
  small.loadFile(L"small.png");
  // 小图不建代理：多解一次纯属浪费
  AIUI_CHECK_EQ(env.fake.platform().thumbSize().width, 0);

  env.fake.platform().setImageSize(Size{9000, 7000});
  TestPhotoView big;
  big.layout(Rect{0, 0, 400, 300});
  big.loadFile(L"big.png");
  const Size thumb = env.fake.platform().thumbSize();
  AIUI_CHECK(thumb.width > 0);
  // 最长边被压到 2048
  AIUI_CHECK_EQ(thumb.width, 2048);
  AIUI_CHECK(thumb.height > 1500 && thumb.height < 1700);
}

// ───────────────────────── 像素格 ─────────────────────────

AIUI_TEST(photo_view_pixel_grid_only_appears_when_zoomed_in) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.setZoomAt(2.0f, Point{200, 200});
  AIUI_CHECK(!view.isPixelGridVisible());

  view.setZoomAt(16.0f, Point{200, 200});
  AIUI_CHECK(view.isPixelGridVisible());
}

AIUI_TEST(photo_view_center_pixel_tracks_pan) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 100% 居中时中心像素就是图像中心
  view.zoomToActual();
  AIUI_CHECK_EQ(view.centerPixel().x, 100);
  AIUI_CHECK_EQ(view.centerPixel().y, 50);

  view.setPan(Point{20, 0});
  // 平移 20 个视图像素、倍数 1 时应左移 20 个图像像素
  AIUI_CHECK_EQ(view.centerPixel().x, 80);
}
