// PhotoView 的动画平滑性测试
//
// 两个已经修过的问题都发生在这里，用逐帧采样把它们钉死：
//   - 平移上限在「图刚好铺满视图」处不连续 → 放大跨过那一格整幅图横跳
//   - 平移量取整 → 动画每帧抖 1 像素，放大后肉眼可见
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/widgets/photo_view.h"

#include <cmath>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct AnimEnv {
  ScopedFakePlatform fake;
  AnimEnv() { fonts.create(96); }
  ~AnimEnv() { fonts.destroy(); }
};

MouseEvent wheelAt(int x, int y) {
  MouseEvent e;
  e.position = Point{x, y};
  e.wheelDelta = 120;
  return e;
}

class AnimProbe : public PhotoView {
 public:
  using PhotoView::onDelayed;
  using PhotoView::onMouseWheel;
};

// 浮点平移下的精确图像坐标：视图点 p 底下是哪个图像像素（含小数）
void imagePointAt(const PhotoView& v, Point p, float& outX, float& outY) {
  const Size src = v.imageSize();
  const Point c = v.clientRect().center();
  const Point pan = v.pan();
  outX = (static_cast<float>(p.x - c.x - pan.x)) / v.zoom() + src.width / 2.0f;
  outY = (static_cast<float>(p.y - c.y - pan.y)) / v.zoom() + src.height / 2.0f;
}

}  // namespace

// 滚轮放大到 1000% 的过程中，光标底下的像素必须始终留在光标底下。
// 允许 1 像素：图像边缘处平移会被夹住，这是几何上不可避免的。
AIUI_TEST(photo_view_wheel_zoom_keeps_anchor_during_animation) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);

  const Point cursor{250, 170};
  float ax = 0.0f;
  float ay = 0.0f;
  imagePointAt(view, cursor, ax, ay);

  float worst = 0.0f;
  for (int notch = 0; notch < 20; ++notch) {
    view.onMouseWheel(wheelAt(cursor.x, cursor.y));
    // 逐帧走完整段动画，逐帧检查锚点
    for (int f = 0; f < 13; ++f) {
      env.fake.platform().advanceMillis(16);
      view.onDelayed();
      float bx = 0.0f;
      float by = 0.0f;
      imagePointAt(view, cursor, bx, by);
      worst = (std::max)(worst, (std::max)(std::fabs(bx - ax), std::fabs(by - ay)));
    }
  }
  // 修好前最坏 46 像素（整幅图横跳），修好后只剩边缘夹紧的 1 像素
  AIUI_CHECK(worst <= 1.5f);
}

// 连续滚轮时不能因为「先落位再算目标」而弹一下：每一格的位置都要连续。
AIUI_TEST(photo_view_wheel_zoom_has_no_position_jump) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{2000, 1000});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);

  const Point cursor{260, 180};
  int prevX = 0;
  int prevY = 0;
  float prevZoom = view.zoom();
  bool first = true;
  float worstRatio = 0.0f;

  for (int i = 0; i < 30; ++i) {
    view.onMouseWheel(wheelAt(cursor.x, cursor.y));
    env.fake.platform().advanceMillis(16);
    view.onDelayed();

    const Point pan = view.pan();
    if (!first) {
      // 平移随倍数的变化率应当稳定：用「平移增量 / 倍数增量」衡量，
      // 突变会让这个比值暴涨（旧版在铺满点处是几十倍）
      const float dz = view.zoom() - prevZoom;
      if (dz > 1e-5f) {
        const float dx = static_cast<float>(pan.x - prevX) / dz;
        const float dy = static_cast<float>(pan.y - prevY) / dz;
        worstRatio = (std::max)(worstRatio, (std::max)(std::fabs(dx), std::fabs(dy)));
      }
    }
    prevX = pan.x;
    prevY = pan.y;
    prevZoom = view.zoom();
    first = false;
  }
  // 锚点偏移最多 200 像素，放大 30 格也不该出现更大的斜率
  AIUI_CHECK(worstRatio < 400.0f);
}

// 网格出现的那一刻画面不能动：网格坐标与实际绘制用的是同一个取整基准
AIUI_TEST(photo_view_grid_appearance_does_not_shift_image) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  const Point cursor{250, 170};
  view.setZoomAt(9.9f, cursor);
  const Point before = view.pan();
  view.setZoomAt(10.0f, cursor);
  const Point after = view.pan();
  // 0.1 倍的倍数变化只该让平移挪一点点，绝不该整块跳
  AIUI_CHECK(std::abs(after.x - before.x) <= 6);
  AIUI_CHECK(std::abs(after.y - before.y) <= 6);
}
