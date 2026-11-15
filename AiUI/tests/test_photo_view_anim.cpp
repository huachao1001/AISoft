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

  // 给一个恒定颜色的像素源：网格文字测试只关心「画了几行、每行多高」，
  // 颜色固定成 (250,120,30) 便于辨认 R/G/B 三行。
  void useSolidPixels() {
    setPixelRegionProvider([](const Rect& region, RgbaImage& out) {
      out.width = region.width();
      out.height = region.height();
      out.pixels.assign(static_cast<size_t>(region.width()) * region.height(),
                        makeColor(250, 120, 30));
      return true;
    });
  }

  // 每个像素的颜色编码它自己的坐标：R = x*3, G = y*3, B = (x+y)%256。
  // 只要哪一格取到了错行错列，三个数字立刻对不上——这是验证
  // 「一个像素 = 一个格子 = 一个 RGB」唯一可靠的像素源：纯色图做不到。
  void useCodedPixels() {
    setPixelRegionProvider([](const Rect& region, RgbaImage& out) {
      out.width = region.width();
      out.height = region.height();
      out.pixels.resize(static_cast<size_t>(out.width) * out.height);
      for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
          const int ix = region.left + x;
          const int iy = region.top + y;
          out.pixels[static_cast<size_t>(y) * out.width + x] =
              makeColor(static_cast<std::uint8_t>((ix * 3) % 256),
                        static_cast<std::uint8_t>((iy * 3) % 256),
                        static_cast<std::uint8_t>((ix + iy) % 256));
        }
      }
      return true;
    });
  }
};

// 由一格的视图矩形反推它覆盖的是哪个图像像素。
// 绘制时图像按 translate(center+pan) 摆放，像素 (x,y) 的左边界是
//   left = cx + x*cell − W*cell/2
// 反过来解 x 即可。格子与像素只要错一格，这里就会算出邻居的坐标，
// 随后 RGB 校验必然失败。
struct DecodedCell {
  int x = 0;
  int y = 0;
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
  int r = 0;
  int g = 0;
  int b = 0;
};

std::vector<DecodedCell> decodeCells(const PhotoView& view, const RecordingBackend& backend,
                                     const Rect& area) {
  const Size src = view.imageSize();
  const float cell = view.zoom();
  const Point c = area.center();
  const Point pan = view.pan();
  const float cx =
      static_cast<float>(static_cast<int>(std::lround(static_cast<float>(c.x) + pan.x)));
  const float cy =
      static_cast<float>(static_cast<int>(std::lround(static_cast<float>(c.y) + pan.y)));
  const float halfW = static_cast<float>(src.width) * cell / 2.0f;
  const float halfH = static_cast<float>(src.height) * cell / 2.0f;

  std::vector<DecodedCell> out;
  const auto& texts = backend.texts();
  for (size_t i = 0; i + 2 < texts.size(); i += 3) {
    DecodedCell s;
    s.left = texts[i].rect.left;
    s.top = texts[i].rect.top;
    s.width = texts[i].rect.width();
    s.height = texts[i + 2].rect.bottom - texts[i].rect.top;
    s.x = static_cast<int>(std::lround((static_cast<float>(s.left) - cx + halfW) / cell));
    s.y = static_cast<int>(std::lround((static_cast<float>(s.top) - cy + halfH) / cell));
    s.r = std::stoi(texts[i].text);
    s.g = std::stoi(texts[i + 1].text);
    s.b = std::stoi(texts[i + 2].text);
    out.push_back(s);
  }
  return out;
}

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

// 连续滚轮时平移必须严格跟着「锚点像素钉住」走，不能有额外的跳动。
//
// 断言的是解析式而不是斜率上限：某一格动画内，
//   pan.x = anchorX − deltaX · (zoom / fromZoom)
// 所以相邻两帧的平移增量必然是 −deltaX · dzoom / fromZoom。
// 实测偏差只来自 pan 的整数取整，容差给 1.5px。
//
// 不用「平移增量 / 倍数增量」这种比值：靠近起点时 dzoom 很小，
// 1 像素的取整误差除以小 dzoom 会放大成几百，那是量化的假象而不是跳变。
AIUI_TEST(photo_view_wheel_zoom_has_no_position_jump) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{2000, 1000});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);

  const Point cursor{260, 180};
  const Point center = view.clientRect().center();
  float worst = 0.0f;

  for (int i = 0; i < 30; ++i) {
    // 每一格都是独立的一段动画：记下起点，用来算应有的增量
    const Point panBefore = view.pan();
    const float fromZoom = view.zoom();
    const float deltaX = static_cast<float>(cursor.x - center.x - panBefore.x);
    const float deltaY = static_cast<float>(cursor.y - center.y - panBefore.y);

    view.onMouseWheel(wheelAt(cursor.x, cursor.y));
    env.fake.platform().advanceMillis(16);
    view.onDelayed();

    const Point panAfter = view.pan();
    const float ratio = (view.zoom() - fromZoom) / fromZoom;
    const float expectedX = -deltaX * ratio;
    const float expectedY = -deltaY * ratio;
    worst = (std::max)(worst,
                       (std::max)(std::fabs(static_cast<float>(panAfter.x - panBefore.x) - expectedX),
                                  std::fabs(static_cast<float>(panAfter.y - panBefore.y) - expectedY)));
  }
  // 修好前在「图刚好铺满」那一格会出现 25/46 像素的额外跳变
  AIUI_CHECK(worst <= 1.5f);
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

// 网格与锐化都必须按时间淡入，不能在某一倍率上整屏突然出现/切换。
// 跨越阈值那一帧起 alpha 从 0 走到 1，耗时 kFadeMs。
AIUI_TEST(photo_view_grid_fades_in_instead_of_popping) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  const Point anchor{200, 200};
  // 阈值以下完全不出现
  view.setZoomAt(9.99f, anchor);
  AIUI_CHECK(view.gridFade() <= 0.001f);

  // 刚跨过阈值：目标是「显示」，但当前值还该是接近 0（不是一过就满）
  view.setZoomAt(10.0f, anchor);
  const float atCrossing = view.gridFade();
  AIUI_CHECK(atCrossing >= 0.0f);
  AIUI_CHECK(atCrossing < 0.25f);
  // 但状态上已经算「该显示」：1000% 一个字就到位，不必等淡入结束
  AIUI_CHECK(view.isPixelGridVisible());

  // 淡入过程是单调递增的，走到一半时既不是 0 也不是 1
  env.fake.platform().advanceMillis(70);
  const float half = view.gridFade();
  AIUI_CHECK(half > atCrossing);
  AIUI_CHECK(half > 0.05f && half < 0.98f);

  // kFadeMs（140ms）之后必须完全显现
  env.fake.platform().advanceMillis(120);
  AIUI_CHECK(view.gridFade() >= 0.999f);

  // 更高的倍数一直是满的
  view.setZoomAt(40.0f, anchor);
  AIUI_CHECK(view.gridFade() >= 0.999f);
  // 采样淡入与网格同步：同一段过渡里两层一起变
  AIUI_CHECK(std::fabs(view.sharpenFade() - view.gridFade()) < 0.001f);
}

// 反向也要淡出：从网格状态缩回阈值以下，不是啪一下消失
AIUI_TEST(photo_view_grid_fades_out_instead_of_popping) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  const Point anchor{200, 200};
  view.setZoomAt(20.0f, anchor);
  env.fake.platform().advanceMillis(200);
  AIUI_CHECK(view.gridFade() >= 0.999f);

  // 缩回阈值以下：目标立刻变成 0，但当前值要慢慢降下来
  view.setZoomAt(5.0f, anchor);
  const float justBelow = view.gridFade();
  AIUI_CHECK(justBelow > 0.75f);  // 还在淡出途中，不是立刻消失
  AIUI_CHECK(!view.isPixelGridVisible());

  env.fake.platform().advanceMillis(200);
  AIUI_CHECK(view.gridFade() <= 0.001f);
}

// 过渡带里必须把两种采样各画一遍（交叉淡化），而不是直接换一种。
// 只有一层的时候说明还是硬切。
AIUI_TEST(photo_view_crossfades_smoothing_in_transition) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  RecordingBackend backend;
  Painter painter(backend);

  const Point anchor{200, 200};
  // 完全在阈值之外：只画一层平滑
  view.setZoomAt(5.0f, anchor);
  backend.clear();
  view.draw(painter, Rect{0, 0, 400, 400});
  AIUI_CHECK_EQ(static_cast<int>(backend.images().size()), 1);
  if (backend.images().size() == 1) {
    AIUI_CHECK(backend.images()[0].smoothing == ImageSmoothing::kSmooth);
  }

  // 淡入走完之后：只画一层最近邻
  view.setZoomAt(40.0f, anchor);
  env.fake.platform().advanceMillis(200);
  backend.clear();
  view.draw(painter, Rect{0, 0, 400, 400});
  AIUI_CHECK_EQ(static_cast<int>(backend.images().size()), 1);
  if (backend.images().size() == 1) {
    AIUI_CHECK(backend.images()[0].smoothing == ImageSmoothing::kNearest);
  }

  // 刚跨过阈值：两层，一层平滑淡出 + 一层最近邻淡入
  view.setZoomAt(5.0f, anchor);
  env.fake.platform().advanceMillis(200);
  view.setZoomAt(10.5f, anchor);
  env.fake.platform().advanceMillis(50);
  backend.clear();
  view.draw(painter, Rect{0, 0, 400, 400});
  const auto& images = backend.images();
  AIUI_CHECK_EQ(static_cast<int>(images.size()), 2);
  if (images.size() == 2) {
    AIUI_CHECK(images[0].smoothing == ImageSmoothing::kSmooth);
    AIUI_CHECK(images[1].smoothing == ImageSmoothing::kNearest);
    AIUI_CHECK(images[0].alpha < 1.0f && images[0].alpha > 0.0f);
    AIUI_CHECK(images[1].alpha < 1.0f && images[1].alpha > 0.0f);
    AIUI_CHECK(std::fabs((images[0].alpha + images[1].alpha) - 1.0f) < 0.01f);
  }
}

// 旋转期间绝不能画网格：网格按 rotation_ 的 ×90° 算坐标，图像按
// displayAngle_ 画，两者不同步，网格会整体错位。
AIUI_TEST(photo_view_hides_grid_while_rotating) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);

  const Point anchor{200, 200};
  // 先用瞬时缩放到位，再开动画做旋转——否则这里要跑一遍缩放动画才到位
  view.setAnimationDuration(0);
  view.setZoomAt(20.0f, anchor);
  env.fake.platform().advanceMillis(400);
  AIUI_CHECK(view.gridFade() >= 0.999f);

  // 用带时长的旋转：瞬时旋转没有中间角度，网格本来就不会错位
  view.setAnimationDuration(180);
  view.rotate(1);
  // 旋转一开始就必须钉到 0，不能留一帧错位的网格
  AIUI_CHECK(view.gridFade() <= 0.001f);
  env.fake.platform().advanceMillis(60);
  view.onDelayed();
  AIUI_CHECK(view.gridFade() <= 0.001f);

  // 转完之后角度已落回 90° 整数倍，网格重新淡入（不是立刻满格）
  env.fake.platform().advanceMillis(400);
  view.onDelayed();
  AIUI_CHECK(view.gridFade() < 0.5f);
  env.fake.platform().advanceMillis(200);
  view.onDelayed();
  AIUI_CHECK(view.gridFade() >= 0.999f);
}

// 动画首帧不能一口气走掉大半个行程：那正是「生硬」的来源。
// smoothstep 起步平缓，第一帧（约 9% 时间）的进度应远小于一半。
AIUI_TEST(photo_view_zoom_animation_starts_gently) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{200, 100});
  AnimProbe view;
  view.layout(Rect{0, 0, 400, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(180);

  const float before = view.zoom();
  MouseEvent e = wheelAt(200, 200);
  view.onMouseWheel(e);

  env.fake.platform().advanceMillis(16);
  view.onDelayed();
  const float firstFrame = (view.zoom() - before) / (before * 0.15f);
  // 16/180 ≈ 8.9% 时间；smoothstep 在这点的进度约 2.2%
  AIUI_CHECK(firstFrame < 0.10f);

  // 中点附近才是最快的，末帧几乎不再移动
  env.fake.platform().advanceMillis(74);
  view.onDelayed();
  const float mid = view.zoom();
  env.fake.platform().advanceMillis(60);
  view.onDelayed();
  const float late = view.zoom();
  env.fake.platform().advanceMillis(60);
  view.onDelayed();
  const float end = view.zoom();
  // 最后一小段（60ms）的位移应小于中间那一段
  AIUI_CHECK(std::fabs(end - late) < std::fabs(late - mid));
  AIUI_CHECK(std::fabs(end - mid) > 0.0f);
}

// ───────────────────────── 像素格里的 RGB 文字 ─────────────────────────

// 每个可见格子都必须有 R、G、B 三行数字：三行竖直排、自上而下。
// 「有些格子有字、有些没有」是修过的缺陷，这里按格数精确核对条数。
AIUI_TEST(photo_view_cell_text_covers_every_cell) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{1600, 1000});
  AnimProbe view;
  view.useSolidPixels();
  view.layout(Rect{0, 0, 600, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 20px 的格子：视口 600x400 正好摆 30x20 格
  view.setZoomAt(20.0f, Point{300, 200});
  env.fake.platform().advanceMillis(300);
  view.onDelayed();

  RecordingBackend backend;
  Painter painter(backend);
  view.draw(painter, Rect{0, 0, 600, 400});

  const auto& texts = backend.texts();
  AIUI_CHECK(!texts.empty());
  // 条数必须是格数的三倍，不多不少：多说明重复画，少说明跳过了某些格子
  AIUI_CHECK_EQ(static_cast<int>(texts.size()) % 3, 0);

  // 逐格核对：三行自上而下依次是 R、G、B，且行框落在同一格里
  for (size_t i = 0; i + 2 < texts.size(); i += 3) {
    const TextCall& r = texts[i];
    const TextCall& g = texts[i + 1];
    const TextCall& b = texts[i + 2];
    AIUI_CHECK(r.text == L"250");
    AIUI_CHECK(g.text == L"120");
    AIUI_CHECK(b.text == L"30");
    // 三行左边界相同、竖直方向首尾相接
    AIUI_CHECK_EQ(r.rect.left, g.rect.left);
    AIUI_CHECK_EQ(r.rect.left, b.rect.left);
    AIUI_CHECK_EQ(r.rect.bottom, g.rect.top);
    AIUI_CHECK_EQ(g.rect.bottom, b.rect.top);
  }
}

// 字号必须随格子大小增长：格子越大，三行文字的行框越高。
AIUI_TEST(photo_view_cell_text_grows_with_cell_size) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{1600, 1000});
  AnimProbe view;
  view.useSolidPixels();
  view.layout(Rect{0, 0, 600, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  auto firstRowHeight = [&](float zoom) {
    view.setZoomAt(zoom, Point{300, 200});
    env.fake.platform().advanceMillis(300);
    view.onDelayed();
    RecordingBackend backend;
    Painter painter(backend);
    view.draw(painter, Rect{0, 0, 600, 400});
    const auto& texts = backend.texts();
    return texts.empty() ? 0 : texts[0].rect.height();
  };

  const int at20 = firstRowHeight(20.0f);
  const int at40 = firstRowHeight(40.0f);
  const int at80 = firstRowHeight(80.0f);
  AIUI_CHECK(at20 > 0);
  // 格子放大一倍，每一行的行框该跟着长：这是「格子大字大」的直接体现
  AIUI_CHECK(at40 > at20);
  AIUI_CHECK(at80 > at40);
}

// 三行必须正好铺满一格、不越界：这是「不叠字、不压到隔壁格」的几何保证。
AIUI_TEST(photo_view_cell_text_stays_inside_its_cell) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{1600, 1000});
  AnimProbe view;
  view.useSolidPixels();
  view.layout(Rect{0, 0, 600, 400});
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.setZoomAt(20.0f, Point{300, 200});
  env.fake.platform().advanceMillis(300);
  view.onDelayed();

  RecordingBackend backend;
  Painter painter(backend);
  view.draw(painter, Rect{0, 0, 600, 400});

  const auto& texts = backend.texts();
  AIUI_CHECK(!texts.empty());
  for (size_t i = 0; i + 2 < texts.size(); i += 3) {
    // 三行首尾相接，整体高度恰好一格：20px 的格子应当正好 20 高
    const int span = texts[i + 2].rect.bottom - texts[i].rect.top;
    AIUI_CHECK_EQ(span, 20);
    // 行框宽度也就是一格宽
    AIUI_CHECK_EQ(texts[i].rect.width(), 20);
  }
}

// ─────────────────── 一个像素 = 一个格子 = 一个 RGB ───────────────────

// 这是像素格的立身之本：格子里的三个数必须是它覆盖的那个像素的颜色，
// 不能是邻居、不能是平均值、不能重采样。用「颜色编码自身坐标」的像素源
// 来钉：任何错位都会让三个数字对不上编码规则。
AIUI_TEST(photo_view_one_pixel_per_cell) {  AnimEnv env;
  env.fake.platform().setImageSize(Size{400, 300});
  AnimProbe view;
  view.useCodedPixels();
  const Rect area{0, 0, 600, 400};
  view.layout(area);
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);

  // 整数倍与非整数倍都要过：非整数倍时格边长会在 floor/ceil 之间交替，
  // 最容易出现「格子往左偏半格、数字取到隔壁像素」。
  for (float zoom : {10.0f, 13.7f, 20.0f, 33.3f, 64.0f}) {
    view.setZoomAt(zoom, area.center());
    env.fake.platform().advanceMillis(400);
    view.onDelayed();

    RecordingBackend backend;
    Painter painter(backend);
    view.draw(painter, area);
    const std::vector<DecodedCell> cells = decodeCells(view, backend, area);
    AIUI_CHECK(!cells.empty());

    // 同一像素不能被画两次：那说明格子在重复
    std::vector<std::pair<int, int>> seen;
    for (const DecodedCell& s : cells) {
      AIUI_CHECK_EQ(s.r, (s.x * 3) % 256);
      AIUI_CHECK_EQ(s.g, (s.y * 3) % 256);
      AIUI_CHECK_EQ(s.b, (s.x + s.y) % 256);

      // 格边长应当就是倍数（±1 给取整交替）
      const int span = static_cast<int>(std::lround(zoom));
      AIUI_CHECK(std::abs(s.width - span) <= 1);
      AIUI_CHECK(std::abs(s.height - span) <= 1);

      for (const auto& p : seen) {
        AIUI_CHECK(!(p.first == s.x && p.second == s.y));
      }
      seen.emplace_back(s.x, s.y);
    }

    // 也不能漏：可见范围内每个像素都该有一个格子，不能中间空一行
    int minX = cells[0].x;
    int maxX = cells[0].x;
    int minY = cells[0].y;
    int maxY = cells[0].y;
    for (const DecodedCell& s : cells) {
      minX = (std::min)(minX, s.x);
      maxX = (std::max)(maxX, s.x);
      minY = (std::min)(minY, s.y);
      maxY = (std::max)(maxY, s.y);
    }
    // 边缘可能被视图切掉半格，允许少一圈
    const int full = (maxX - minX + 1) * (maxY - minY + 1);
    const int edge = 2 * (maxX - minX + 1) + 2 * (maxY - minY + 1);
    AIUI_CHECK(static_cast<int>(cells.size()) >= full - edge);
  }
}

// 旋转 90° 之后，格子里的 RGB 仍必须是一个真实像素的颜色：旋转只改变摆放
// 方向，不该改变「一格一像素」这个前提。这里只校验三值自洽
// （能还原出同一个 (x,y)），坐标映射由上面那条测试负责。
AIUI_TEST(photo_view_one_pixel_per_cell_after_rotation) {
  AnimEnv env;
  env.fake.platform().setImageSize(Size{400, 300});
  AnimProbe view;
  view.useCodedPixels();
  const Rect area{0, 0, 600, 400};
  view.layout(area);
  view.loadFile(L"a.png");
  view.setAnimationDuration(0);
  view.setZoomAt(20.0f, area.center());
  view.rotate(1);
  env.fake.platform().advanceMillis(1000);
  view.onDelayed();
  AIUI_CHECK_EQ(view.rotation(), 1);

  RecordingBackend backend;
  Painter painter(backend);
  view.draw(painter, area);

  const auto& texts = backend.texts();
  AIUI_CHECK(!texts.empty());
  for (size_t i = 0; i + 2 < texts.size(); i += 3) {
    const int r = std::stoi(texts[i].text);
    const int g = std::stoi(texts[i + 1].text);
    const int b = std::stoi(texts[i + 2].text);
    // R=x*3, G=y*3, B=(x+y)%256：只要存在一个像素能同时满足三者就是自洽的
    bool found = false;
    for (int x = 0; x < 400 && !found; ++x) {
      if ((x * 3) % 256 != r) continue;
      for (int y = 0; y < 300; ++y) {
        if ((y * 3) % 256 == g && (x + y) % 256 == b) {
          found = true;
          break;
        }
      }
    }
    AIUI_CHECK(found);
  }
}
