// Slider 的绘制测试（铁律三）
//
// 滑块的几何有三个容易画错的地方，各写一组断言：
//   1. 轨道两端各内缩一个滑块半径，否则滑块拖到端点会有一半露在视图外；
//   2. 值 → 像素必须用 round 而不是 trunc，否则拖到最右停在 99%；
//   3. 已走过的那段填充与轨道同圆角，且右端被滑块压住，不该露出方角。
#include "widget_test_support.h"

#include "pyui/widgets/slider.h"

namespace pyui {
namespace test {
namespace {

constexpr int kThumbD = 18;   // kSizeThumb
constexpr int kTrackH = 6;    // kSizeTrackH
constexpr int kRadius = kThumbD / 2;  // 9

// 200×20 的视图（测量高度 = 18+2）：
//   轨道 {9, 7, 191, 13}，滑块直径 18 垂直居中 → top = 1
constexpr Rect kTrack{9, 7, 191, 13};

// 值 0.5 时的滑块矩形：中心 x = 9 + (182/2) = 100
constexpr Rect kThumbHalf{91, 1, 109, 19};

}  // namespace

TEST(slider_draw_commands_zero) {
  Slider view;
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 20}),
      // 轨道：灰底、两端半圆
      cmd::fillRound(kTrack, t.trackBg, kTrackH / 2),
      // 值 0：没有填充
      // 滑块：白底 + 描边，贴左端（中心 x = 9）
      cmd::fillRound(Rect{0, 1, 18, 19}, t.thumb, kRadius),
      cmd::strokeRound(Rect{0, 1, 18, 19}, t.thumbBorder, kRadius, 1),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 20}), expected);
}

TEST(slider_draw_commands_mid) {
  Slider view;
  view.setValue(0.5);
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 20}),
      cmd::fillRound(kTrack, t.trackBg, kTrackH / 2),
      // 已走过的部分：从轨道左端到滑块中心
      cmd::fillRound(Rect{9, 7, 100, 13}, t.trackFill, kTrackH / 2),
      cmd::fillRound(kThumbHalf, t.thumb, kRadius),
      cmd::strokeRound(kThumbHalf, t.thumbBorder, kRadius, 1),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 20}), expected);
}

TEST(slider_track_is_inset_by_thumb_radius) {
  Slider view;
  const auto commands = recordDirect(view, Size{200, 20});
  const DrawCommand* track = firstOf(commands, DrawCommand::Kind::kFillRoundRect);
  CHECK(track != nullptr);
  if (track == nullptr) return;

  // 两端各让出半径：滑块拖到端点时正好贴住视图边缘而不被裁掉
  CHECK_EQ(track->rect.left, kRadius);
  CHECK_EQ(track->rect.right, 200 - kRadius);
  // 行程宽度 182 = 200 - 2*9
  CHECK_EQ(track->rect.width(), 200 - 2 * kRadius);
}

TEST(slider_value_maps_to_thumb_center) {
  Slider view;

  const auto thumbOf = [&] {
    const auto commands = recordDirect(view, Size{200, 20});
    for (const DrawCommand& command : commands) {
      if (command.kind == DrawCommand::Kind::kFillRoundRect && command.color == defaultTheme().thumb) {
        return command.rect;
      }
    }
    return Rect{};
  };

  view.setValue(0.0);
  CHECK_EQ(thumbOf().center().x, kRadius);

  view.setValue(0.25);
  // 9 + round(0.25 * 182) = 9 + 46 = 55
  CHECK_EQ(thumbOf().center().x, 55);

  view.setValue(0.5);
  CHECK_EQ(thumbOf().center().x, 100);

  // 1.0 必须落在右端内缩点上，而不是视图右缘
  view.setValue(1.0);
  CHECK_EQ(thumbOf().center().x, 191);
}

TEST(slider_value_is_clamped_and_quantized) {
  Slider view;
  view.setRange(0.0, 10.0);
  view.setStep(2.0);
  view.setValue(7.3);
  // 相对最小值的偏移量化：7.3 → 6 或 8 中更近的 8
  CHECK_NEAR(view.value(), 8.0, 1e-9);

  view.setValue(99.0);
  CHECK_NEAR(view.value(), 10.0, 1e-9);

  view.setValue(-5.0);
  CHECK_NEAR(view.value(), 0.0, 1e-9);
}

TEST(slider_step_quantizes_relative_to_minimum) {
  Slider view;
  // minimum 不是 step 的整数倍：直接 value/step 取整会把最小值本身挪走
  view.setRange(3.0, 13.0);
  view.setStep(5.0);
  view.setValue(3.0);
  CHECK_NEAR(view.value(), 3.0, 1e-9);

  view.setValue(7.0);
  CHECK_NEAR(view.value(), 8.0, 1e-9);  // 3 + 1*5
  view.setValue(9.0);
  CHECK_NEAR(view.value(), 8.0, 1e-9);  // 更靠近 8 而不是 13
}

TEST(slider_reversed_range_is_swapped) {
  Slider view;
  view.setRange(10.0, 0.0);
  // 写反是常见笔误；两个端点都是合法取值，直接交换而不是报错
  CHECK_NEAR(view.minimum(), 0.0, 1e-9);
  CHECK_NEAR(view.maximum(), 10.0, 1e-9);
}

TEST(slider_range_shrink_clamps_value) {
  Slider view;
  view.setRange(0.0, 100.0);
  view.setValue(80.0);
  view.setRange(0.0, 10.0);
  // 范围收窄后旧值必须夹进来，否则滑块会停在一个键盘也到不了的位置
  CHECK_NEAR(view.value(), 10.0, 1e-9);
}

TEST(slider_click_jumps_to_pointer) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.relayout();

  MouseEvent down;
  down.position = Point{100, 10};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);

  // 点到轨道任意位置都直接跳过去，不是只允许抓滑块
  CHECK_NEAR(view->value(), 0.5, 0.01);
  CHECK(view->isPressed());  // ViewRoot 维护的按下态
}

TEST(slider_drag_updates_continuously_and_commits_on_release) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.relayout();

  std::vector<double> changes;
  std::vector<double> commits;
  view->setOnChange([&](double v) { changes.push_back(v); });
  view->setOnCommit([&](double v) { commits.push_back(v); });

  MouseEvent down;
  down.position = Point{50, 10};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);

  MouseEvent move = down;
  move.position = Point{100, 10};
  harness.root().dispatchMouseMove(move);
  move.position = Point{150, 10};
  harness.root().dispatchMouseMove(move);

  // 拖动中持续回调、松手前不提交：否则「松手才保存」的调用方会保存几十遍
  CHECK_MSG(changes.size() >= 2, "拖动过程必须持续通知");
  CHECK_EQ(commits.size(), size_t{0});

  harness.root().dispatchMouseUp(move);
  CHECK_EQ(commits.size(), size_t{1});
  CHECK_NEAR(commits.back(), view->value(), 1e-9);
}

TEST(slider_drag_outside_clamps_to_ends) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.relayout();

  MouseEvent down;
  down.position = Point{10, 10};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);

  MouseEvent move = down;
  move.position = Point{500, 10};  // 拖出视图右界
  harness.root().dispatchMouseMove(move);

  // 拖到外面仍能拉回来，值夹到端点而不是丢失
  CHECK_NEAR(view->value(), 1.0, 1e-9);
}

TEST(slider_keyboard_steps_by_twentieth_when_continuous) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  view->setRange(0.0, 1.0);
  harness.root().setFocusedView(view);

  // 没有显式 step 时按范围 1/20 走：任何范围都能在 20 次内走完
  CHECK(harness.root().dispatchKeyDown(Key::kRight, Modifiers{}));
  CHECK_NEAR(view->value(), 0.05, 1e-9);

  CHECK(harness.root().dispatchKeyDown(Key::kLeft, Modifiers{}));
  CHECK_NEAR(view->value(), 0.0, 1e-9);

  // Home / End 直接跳到端点
  CHECK(harness.root().dispatchKeyDown(Key::kEnd, Modifiers{}));
  CHECK_NEAR(view->value(), 1.0, 1e-9);
  CHECK(harness.root().dispatchKeyDown(Key::kHome, Modifiers{}));
  CHECK_NEAR(view->value(), 0.0, 1e-9);
}

TEST(slider_keyboard_uses_explicit_step) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  view->setRange(0.0, 100.0);
  view->setStep(5.0);
  harness.root().setFocusedView(view);

  harness.root().dispatchKeyDown(Key::kRight, Modifiers{});
  CHECK_NEAR(view->value(), 5.0, 1e-9);

  Modifiers shift;
  shift.shift = true;
  harness.root().dispatchKeyDown(Key::kRight, shift);
  // Shift 一次走 10 格
  CHECK_NEAR(view->value(), 55.0, 1e-9);
}

TEST(slider_page_keys_jump_a_tenth) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  view->setRange(0.0, 100.0);
  harness.root().setFocusedView(view);

  harness.root().dispatchKeyDown(Key::kPageUp, Modifiers{});
  CHECK_NEAR(view->value(), 10.0, 1e-9);
  harness.root().dispatchKeyDown(Key::kPageDown, Modifiers{});
  CHECK_NEAR(view->value(), 0.0, 1e-9);
}

TEST(slider_keyboard_commits_immediately) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.root().setFocusedView(view);

  int commits = 0;
  view->setOnCommit([&](double) { ++commits; });

  // 键盘没有「松手」这个动作：不提交的话用键盘改的值永远落不了盘
  harness.root().dispatchKeyDown(Key::kRight, Modifiers{});
  CHECK_EQ(commits, 1);
}

TEST(slider_drag_draws_halo) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.relayout();

  const size_t idleStrokes = recordDirect(*view, Size{200, 20}).size();

  MouseEvent down;
  down.position = Point{100, 10};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);

  const auto dragging = recordDirect(*view, Size{200, 20});
  // 拖动中多一圈光晕：让「正在拖」与「只是悬停」区分开
  CHECK_MSG(dragging.size() > idleStrokes, "拖动中的滑块应比静止时多一层绘制");

  const DrawCommand* halo = nullptr;
  for (const DrawCommand& command : dragging) {
    if (command.kind == DrawCommand::Kind::kStrokeRoundRect && command.rect.width() > kThumbD) {
      halo = &command;
    }
  }
  CHECK(halo != nullptr);
  if (halo == nullptr) return;
  // 光晕比滑块每边大 3px
  CHECK_EQ(halo->rect, kThumbHalf.inflated(3, 3));
  CHECK_EQ(halo->radius, halo->rect.height() / 2);
}

TEST(slider_no_halo_when_not_dragging) {
  Slider view;
  const auto commands = recordDirect(view, Size{200, 20});
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kStrokeRoundRect) {
      CHECK_MSG(command.rect.width() <= kThumbD, "静止时不应有光晕");
    }
  }
}

TEST(slider_focus_ring_uses_accent_color) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  harness.root().setFocusedView(view);
  harness.relayout();

  const Surface surface = harness.render();
  // 滑块外圈描边换成 inputFocus：滑块没有文字，不加这层看不出键盘焦点
  CHECK(sameRgb(surface.at(0, 10), defaultTheme().inputFocus));
}

TEST(slider_disabled_dims_track_and_thumb) {
  Slider view;
  view.setValue(0.5);
  view.setEnabled(false);
  const auto commands = recordDirect(view, Size{200, 20});

  const Theme& t = defaultTheme();
  // 填充也要褪掉：只褪轨道会让「已经走过的部分」看起来仍然可用
  const Color expectedFill = blendColor(t.trackFill, t.pane, 0.7f);
  bool sawFill = false;
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kFillRoundRect && command.color == expectedFill) {
      sawFill = true;
    }
    // 禁用时轨道不能用启用态那支灰
    if (command.kind == DrawCommand::Kind::kFillRoundRect && command.color == t.trackBg) {
      FAIL_AT("禁用态轨道仍用了启用色");
    }
  }
  CHECK(sawFill);
}

TEST(slider_disabled_ignores_input) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  view->setValue(0.5);
  view->setEnabled(false);
  harness.root().setFocusedView(view);

  MouseEvent down;
  down.position = Point{150, 10};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);
  CHECK_NEAR(view->value(), 0.5, 1e-9);

  CHECK(!harness.root().dispatchKeyDown(Key::kRight, Modifiers{}));
  CHECK_NEAR(view->value(), 0.5, 1e-9);
}

TEST(slider_right_click_does_not_drag) {
  RootHarness harness(Size{200, 20});
  Slider* view = harness.add<Slider>();
  view->setValue(0.0);
  harness.relayout();

  MouseEvent down;
  down.position = Point{150, 10};
  down.button = MouseButton::kRight;
  harness.root().dispatchMouseDown(down);
  // 右键留给上下文菜单，不能顺手把值改了
  CHECK_NEAR(view->value(), 0.0, 1e-9);
}

TEST(slider_thumb_stays_inside_view) {
  Slider view;
  for (double value : {0.0, 0.001, 0.5, 0.999, 1.0}) {
    view.setValue(value);
    const Surface surface = renderDirect(view, Size{200, 20});
    const Rect ink = alphaBounds(surface);
    CHECK_MSG(ink.left >= 0, "滑块左缘越出视图");
    CHECK_MSG(ink.right <= 200, "滑块右缘越出视图");
  }
}

TEST(slider_fill_and_track_share_rounding) {
  Slider view;
  view.setValue(0.5);
  const auto commands = recordDirect(view, Size{200, 20});

  const DrawCommand* track = firstOf(commands, DrawCommand::Kind::kFillRoundRect);
  const DrawCommand* fill = nullptr;
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kFillRoundRect &&
        command.color == defaultTheme().trackFill) {
      fill = &command;
    }
  }
  CHECK(track != nullptr);
  CHECK(fill != nullptr);
  if (track == nullptr || fill == nullptr) return;

  // 同圆角、同上下界、左端对齐：任何一条不一致都会在交界处露出缺口
  CHECK_EQ(fill->radius, track->radius);
  CHECK_EQ(fill->rect.top, track->rect.top);
  CHECK_EQ(fill->rect.bottom, track->rect.bottom);
  CHECK_EQ(fill->rect.left, track->rect.left);
}

TEST(slider_zero_width_view_does_not_crash) {
  Slider view;
  // 视图比滑块还窄：退化成零宽轨道，valueToX 仍要返回有效坐标
  const Surface surface = renderDirect(view, Size{10, 20});
  (void)surface;
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 120);
}

}  // namespace test
}  // namespace pyui
