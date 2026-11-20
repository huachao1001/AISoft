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
  using PhotoView::onDoubleClick;
  using View::cursorShape;
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

AIUI_TEST(photo_view_fit_can_enlarge_for_presentation) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  // 演示态要铺满屏幕：200x100 在 400x400 里取 400/200=2 倍
  view.setFitAllowsUpscale(true);
  AIUI_CHECK(view.loadFile(L"a.png"));
  AIUI_CHECK(std::fabs(view.zoom() - 2.0f) < 0.001f);

  // 关掉开关后重新适应，回到 1:1
  view.setAnimationDuration(0);
  view.setFitAllowsUpscale(false);
  view.zoomToFit();
  AIUI_CHECK(std::fabs(view.zoom() - 1.0f) < 0.001f);
}

// 演示态进出：缩放与位置要能原样退回，而不是留在铺满状态
AIUI_TEST(photo_view_view_state_roundtrip) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.setAnimationDuration(0);
  view.loadFile(L"a.png");

  // 用户自己放大并拖动后的状态
  view.setZoomAt(3.0f, Point{260, 180});
  const float zoomBefore = view.zoom();
  const Point panBefore = view.pan();
  const PhotoView::ViewState saved = view.saveViewState();

  // 演示：铺满屏幕
  view.setFitAllowsUpscale(true);
  view.zoomToFit();
  AIUI_CHECK(std::fabs(view.zoom() - 2.0f) < 0.001f);

  // 退出演示：倍数与位置回到自己调好的样子
  view.setFitAllowsUpscale(false);
  view.restoreViewState(saved);
  AIUI_CHECK(std::fabs(view.zoom() - zoomBefore) < 0.001f);
  AIUI_CHECK(std::abs(view.pan().x - panBefore.x) <= 1);
  AIUI_CHECK(std::abs(view.pan().y - panBefore.y) <= 1);
}

// 快照记录的是「跟随窗口」这个选择，恢复时按当时的尺寸重算
AIUI_TEST(photo_view_view_state_keeps_fit_mode) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.setAnimationDuration(0);
  view.loadFile(L"a.png");
  AIUI_CHECK(view.isFitToWindow());

  const PhotoView::ViewState saved = view.saveViewState();
  view.setZoomAt(4.0f, Point{200, 200});  // 用户自己缩放，退出跟随模式

  view.restoreViewState(saved);
  AIUI_CHECK(view.isFitToWindow());
  AIUI_CHECK(std::fabs(view.zoom() - 1.0f) < 0.001f);
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
  // 跟随模型靠连续排帧推进：按 16ms 一帧驱动，直到它自己落位
  for (int f = 0; f < 60 && view.isAnimating(); ++f) {
    env.fake.platform().advanceMillis(16);
    view.onDelayed();
  }

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

AIUI_TEST(photo_view_uses_open_hand_only_while_dragging) {
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

  // 悬停保持箭头：手掌只在真的按住时才出现
  view->onMouseMove(mouseAt(200, 200));
  AIUI_CHECK(window->cursor() == CursorShape::kArrow);
  view->onMouseDown(mouseAt(200, 200, MouseButton::kLeft));
  AIUI_CHECK(window->cursor() == CursorShape::kGrabbing);
  view->onMouseUp(mouseAt(200, 200));
  AIUI_CHECK(window->cursor() == CursorShape::kArrow);
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
  AIUI_CHECK(window->cursor() == CursorShape::kArrow);
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

  // 1000% 就该看到网格：默认阈值是 10 倍
  AIUI_CHECK(std::fabs(view.pixelGridZoom() - 10.0f) < 0.001f);
  view.setZoomAt(9.9f, Point{200, 200});
  AIUI_CHECK(!view.isPixelGridVisible());
  view.setZoomAt(10.0f, Point{200, 200});
  AIUI_CHECK(view.isPixelGridVisible());
}

// 放大经过「图刚好铺满视图」那一格时，平移上限不能突变。
// 旧版在 drawn == view 处从 view/2 直接掉到 0，整幅图横跳几十像素，
// 表现就是「放大过程中位置突变」。这里逐格检查偏心的连续性。
AIUI_TEST(photo_view_zoom_past_fit_does_not_jump) {
  PhotoEnv env;
  // 200x100 的图铺满 400x400：2 倍时宽刚好等于视图，1.33 倍时高才到
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.setZoomAt(1.0f, Point{200, 200});

  const Point cursor{250, 170};
  int prevX = 0;
  int prevY = 0;
  bool first = true;
  // 从 1 倍细步放大到 8 倍，逐格看平移有没有突变
  for (float z = 1.0f; z <= 8.0f; z += 0.02f) {
    view.setZoomAt(z, cursor);
    const Point pan = view.pan();
    if (!first) {
      // 每格 2% 的倍数变化，平移的增量应当是平滑的；这里只拒绝大跳
      AIUI_CHECK(std::abs(pan.x - prevX) <= 12);
      AIUI_CHECK(std::abs(pan.y - prevY) <= 12);
    }
    prevX = pan.x;
    prevY = pan.y;
    first = false;
  }
}

// 鼠标不在图上时不许缩放：锚点必须落在图像像素上才有「放大哪一点」可言。
// 拿画布空白处当锚点会把整幅图推出去，看着像位置突变。
AIUI_TEST(photo_view_wheel_outside_image_does_nothing) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  // 适应窗口后图是 400x200 居中，上下各 100 像素空白
  AIUI_CHECK(!view.isOnImage(Point{200, 20}));
  AIUI_CHECK(view.isOnImage(Point{200, 200}));

  const float before = view.zoom();
  const Point panBefore = view.pan();
  MouseEvent e = mouseAt(200, 20);
  e.wheelDelta = 120;
  AIUI_CHECK(!view.onMouseWheel(e));
  AIUI_CHECK(std::fabs(view.zoom() - before) < 0.001f);
  AIUI_CHECK_EQ(view.pan().x, panBefore.x);
  AIUI_CHECK_EQ(view.pan().y, panBefore.y);

  // 图上滚轮照常生效
  MouseEvent in = mouseAt(200, 200);
  in.wheelDelta = 120;
  AIUI_CHECK(view.onMouseWheel(in));
  AIUI_CHECK(view.zoom() > before);
}

// 工具栏缩放也遵守同一条规则：鼠标停在空白处时退回视图中心当锚点，
// 否则点「放大」图会从光标底下飞走。
AIUI_TEST(photo_view_toolbar_zoom_ignores_off_image_mouse) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  // 2 倍时图占 400x200，纵向 100..300，上下各留 100 像素空白
  view.setZoomAt(2.0f, Point{200, 200});
  AIUI_CHECK(!view.isOnImage(Point{200, 20}));
  view.onMouseMove(mouseAt(200, 20));  // 光标移到空白处

  const Point center = view.clientRect().center();
  const Point pixelAtCenterBefore = imageAt(view, center);
  view.zoomBy(1.25f);
  // 中心锚定：视图中心底下的像素不变
  AIUI_CHECK_EQ(imageAt(view, center).x, pixelAtCenterBefore.x);
  AIUI_CHECK_EQ(imageAt(view, center).y, pixelAtCenterBefore.y);
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

// ─────────────────── 最小显示尺寸：短边不小于 30px ───────────────────

// 缩小到底也不能把图缩成一个点：短边必须留在 30px。
AIUI_TEST(photo_view_zoom_out_keeps_min_side) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{400, 300});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 一路缩到远超下限：应停在「短边 30px」对应的倍数
  view.setZoomAt(0.0001f, Point{200, 200});
  const float shortSide = static_cast<float>(view.imageSize().height) * view.zoom();
  AIUI_CHECK(shortSide >= 29.0f);
  AIUI_CHECK(shortSide <= 31.0f);
}

// 本身就比 30px 还小的图，打开时就要被放大到 30px，而不是原尺寸一小撮。
AIUI_TEST(photo_view_tiny_image_opens_at_min_side) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{8, 6});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");

  // 适应窗口后短边就是 30px：6 * 5 = 30
  AIUI_CHECK(view.isFitToWindow());
  const float shortSide = static_cast<float>(view.imageSize().height) * view.zoom();
  AIUI_CHECK(shortSide >= 29.5f);
  AIUI_CHECK(shortSide <= 30.5f);

  // 再缩小也不该回到 1:1 以下
  view.setZoomAt(0.5f, Point{200, 200});
  const float afterZoomOut = static_cast<float>(view.imageSize().height) * view.zoom();
  AIUI_CHECK(afterZoomOut >= 29.5f);
}

// 旋转不改变短边，所以下限与旋转无关；但旋转后仍必须守住 30px。
AIUI_TEST(photo_view_min_side_survives_rotation) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{40, 300});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 40x300：短边 40 → 下限 30/40 = 0.75
  view.setZoomAt(0.01f, Point{200, 200});
  AIUI_CHECK(std::fabs(view.zoom() - 0.75f) < 0.02f);

  view.rotate(1);
  view.setZoomAt(0.01f, Point{200, 200});
  const Size rotated = view.imageSize();
  const int minSide = rotated.width < rotated.height ? rotated.width : rotated.height;
  const float shown = static_cast<float>(minSide) * view.zoom();
  AIUI_CHECK(shown >= 29.5f);
  AIUI_CHECK(shown <= 30.5f);
}

// ─────────────────── 状态栏坐标：指针下的像素 ───────────────────

// 状态栏要报「我指着的是哪个像素」。以前报的是视图中心，平移时那个数
// 自己会跑，看着像图片坐标而不是指针坐标。
AIUI_TEST(photo_view_mouse_pixel_follows_cursor) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{400, 300});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 还没进过控件：没有「指着哪个像素」可言
  Point pixel;
  AIUI_CHECK(!view.mousePixel(pixel));

  // 1:1 居中：视图中心对应图像中心 (200,150)
  view.zoomToActual();
  const Point center = view.clientRect().center();
  view.onMouseMove(mouseAt(center.x, center.y));
  AIUI_CHECK(view.mousePixel(pixel));
  AIUI_CHECK_EQ(pixel.x, 200);
  AIUI_CHECK_EQ(pixel.y, 150);

  // 光标右移 10 像素、1:1 时图像坐标也右移 10
  view.onMouseMove(mouseAt(center.x + 10, center.y + 5));
  AIUI_CHECK(view.mousePixel(pixel));
  AIUI_CHECK_EQ(pixel.x, 210);
  AIUI_CHECK_EQ(pixel.y, 155);

  // 平移不改「光标下是哪个像素」之外的东西：把图推走，同一处光标指向别处
  view.setPan(Point{-40, 0});
  view.onMouseMove(mouseAt(center.x + 10, center.y + 5));
  AIUI_CHECK(view.mousePixel(pixel));
  AIUI_CHECK_EQ(pixel.x, 250);
  AIUI_CHECK_EQ(pixel.y, 155);
}

// 光标移出图外或离开控件时不该报坐标（上层退回视图中心）
AIUI_TEST(photo_view_mouse_pixel_rejects_outside_image) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{100, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.zoomToActual();

  const Point center = view.clientRect().center();
  // 图只占中间 100x100，四角是画布空白
  view.onMouseMove(mouseAt(10, 10));
  Point pixel;
  AIUI_CHECK(!view.mousePixel(pixel));

  // 回到图上就有
  view.onMouseMove(mouseAt(center.x, center.y));
  AIUI_CHECK(view.mousePixel(pixel));

  // 离开控件：立刻失效，否则状态栏停在最后那个像素上
  view.onMouseLeave();
  AIUI_CHECK(!view.mousePixel(pixel));
}

// 旋转后仍要报对：指针下的像素按旋转后的图像取
AIUI_TEST(photo_view_mouse_pixel_after_rotation) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{400, 300});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.zoomToActual();

  const Point center = view.clientRect().center();
  view.onMouseMove(mouseAt(center.x, center.y));
  Point before;
  AIUI_CHECK(view.mousePixel(before));
  AIUI_CHECK_EQ(before.x, 200);
  AIUI_CHECK_EQ(before.y, 150);

  // 转 90° 后中心像素会变（宽高交换），但仍必须落在图内且可解析
  view.rotate(1);
  view.onMouseMove(mouseAt(center.x, center.y));
  Point after;
  AIUI_CHECK(view.mousePixel(after));
  AIUI_CHECK(after.x >= 0 && after.x < 400);
  AIUI_CHECK(after.y >= 0 && after.y < 300);
}

// ───────────────────────── 演示锁定 ─────────────────────────

// 投影演示时画面必须钉住：滚轮、双击、拖动都不该改动倍数或位置。
// 程序化调用不受影响——进出演示本身就要靠它改倍数。
AIUI_TEST(photo_view_ignores_input_when_not_interactive) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  view.setZoomAt(2.0f, Point{200, 200});
  const float zoomBefore = view.zoom();
  const Point panBefore = view.pan();

  view.setInteractive(false);
  AIUI_CHECK(!view.isInteractive());

  MouseEvent wheel = mouseAt(200, 200);
  wheel.wheelDelta = 120;
  AIUI_CHECK(!view.onMouseWheel(wheel));

  view.onDoubleClick(mouseAt(200, 200));
  // 拖动同样不接：按下不成立，后续移动也就不会改平移
  AIUI_CHECK(!view.onMouseDown(mouseAt(200, 200, MouseButton::kLeft)));
  view.onMouseMove(mouseAt(260, 240));
  view.onMouseUp(mouseAt(260, 240));

  AIUI_CHECK(std::fabs(view.zoom() - zoomBefore) < 0.001f);
  AIUI_CHECK_EQ(view.pan().x, panBefore.x);
  AIUI_CHECK_EQ(view.pan().y, panBefore.y);

  // 恢复交互后一切照旧：这个开关不能把控件弄坏
  view.setInteractive(true);
  AIUI_CHECK(view.onMouseWheel(wheel));
  AIUI_CHECK(view.zoom() != zoomBefore);
}

// 拖动中途被锁定：必须立刻收尾，否则光标停在「握起的手」、鼠标捕获也不释放
AIUI_TEST(photo_view_locking_mid_drag_releases_pointer) {
  PhotoEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  TestPhotoView view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.setZoomAt(2.0f, Point{200, 200});

  AIUI_CHECK(view.onMouseDown(mouseAt(200, 200, MouseButton::kLeft)));
  view.setInteractive(false);

  AIUI_CHECK(view.cursorShape() != CursorShape::kGrabbing);
}
