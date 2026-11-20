// Slider：拖动定位、越界夹紧、键盘微调
//
// 参数面板全靠它调值，所以这里守的是「值一定在 0..1 且回调次数合理」，
// 而不是像素级外观——外观改配色时不该让这些用例失败。
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/widgets/slider.h"

namespace {

using namespace aiui_test;
using namespace AiUI;

struct SliderEnv {
  ScopedFakePlatform fake;
  SliderEnv() { fonts.create(96); }
  ~SliderEnv() { fonts.destroy(); }
};

// onPaint / 鼠标处理都是 protected：测试壳转发一下
struct TestSlider : Slider {
  using Slider::onKeyDown;
  using Slider::onMouseDown;
  using Slider::onMouseLeave;
  using Slider::onMouseMove;
  using Slider::onMouseUp;

  void paintForTest(Painter& painter, const Rect& rect) { onPaint(painter, rect); }
};

MouseEvent leftAt(int x, int y = 10) {
  MouseEvent e;
  e.position = Point{x, y};
  e.button = MouseButton::kLeft;
  return e;
}

}  // namespace

AIUI_TEST(slider_starts_at_zero) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});
  AIUI_CHECK(slider.value() == 0.0f);
}

// 点了轨道中段就该落到那个比例，而不是从当前位置开始相对拖动
AIUI_TEST(slider_click_jumps_to_position) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  float last = -1.0f;
  slider.setOnChange([&](float v) { last = v; });

  slider.onMouseDown(leftAt(100));
  // 滑块半径在两端各占一块，中点仍应接近 0.5
  AIUI_CHECK(last > 0.45f && last < 0.55f);
}

// 越界的值直接夹住：配置文件里读回来的数可能早就超出范围
AIUI_TEST(slider_clamps_out_of_range) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  slider.setValue(2.5f);
  AIUI_CHECK(slider.value() == 1.0f);
  slider.setValue(-3.0f);
  AIUI_CHECK(slider.value() == 0.0f);
}

// 不通知时不回调，通知时才回调
AIUI_TEST(slider_notifies_only_when_asked) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  int calls = 0;
  slider.setOnChange([&](float) { ++calls; });

  slider.setValue(0.3f);
  AIUI_CHECK(calls == 0);
  slider.setValue(0.6f, true);
  AIUI_CHECK(calls == 1);
  // 同一个值再设一次不该重复通知
  slider.setValue(0.6f, true);
  AIUI_CHECK(calls == 1);
}

// 拖到右端得 1，拖到左端得 0，中间不会因为半径让位而够不到两端
AIUI_TEST(slider_drag_reaches_both_ends) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  slider.onMouseDown(leftAt(0));
  slider.onMouseMove(leftAt(199));
  slider.onMouseUp(leftAt(199));
  AIUI_CHECK(slider.value() == 1.0f);

  slider.onMouseDown(leftAt(199));
  slider.onMouseMove(leftAt(0));
  slider.onMouseUp(leftAt(0));
  AIUI_CHECK(slider.value() == 0.0f);
}

// 松开时发一次 release：调用方靠它决定「这一下拖完了，可以出全尺寸图了」
AIUI_TEST(slider_release_fires_once_on_mouse_up) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  int releases = 0;
  slider.setOnRelease([&](float) { ++releases; });

  slider.onMouseDown(leftAt(50));
  slider.onMouseMove(leftAt(120));
  AIUI_CHECK(releases == 0);
  slider.onMouseUp(leftAt(120));
  AIUI_CHECK(releases == 1);
}

// 方向键微调，并且会发 release：键盘改完也该触发一次提交
AIUI_TEST(slider_keyboard_nudges_value) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});
  slider.setValue(0.5f);

  int releases = 0;
  slider.setOnRelease([&](float) { ++releases; });

  Modifiers none;
  slider.onKeyDown(Key::kRight, none);
  AIUI_CHECK(slider.value() > 0.5f);
  AIUI_CHECK(releases == 1);

  slider.onKeyDown(Key::kLeft, none);
  AIUI_CHECK(slider.value() < 0.55f);

  // 已经到头了还按右键，值不该溢出到 1 以外
  slider.setValue(1.0f);
  slider.onKeyDown(Key::kRight, none);
  AIUI_CHECK(slider.value() == 1.0f);
}

// 绘制不能越界：滑块圆心在两端时整个圆仍落在控件矩形内
AIUI_TEST(slider_paints_thumb_inside_bounds) {
  SliderEnv env;
  TestSlider slider;
  slider.layout(Rect{0, 0, 200, 28});

  for (float v : {0.0f, 0.5f, 1.0f}) {
    slider.setValue(v);
    RecordingBackend backend;
    Painter painter(backend);
    slider.paintForTest(painter, slider.clientRect());

    for (const FillCall& fill : backend.fills()) {
      AIUI_CHECK(fill.rect.left >= 0);
      AIUI_CHECK(fill.rect.right <= 200);
    }
  }
}
