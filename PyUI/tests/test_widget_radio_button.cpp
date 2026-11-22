// RadioButton 的绘制测试（铁律三）
//
// 单选与复选框的区别只有两处：外形是圆、以及「组内互斥」。前者落在像素上
// （圆心、描边、实心点与外环之间那一圈空隙），后者落在行为上（点一个，
// 同组其余必须自动取消）。两处都容易画对一半，所以各写一组断言。
#include "widget_test_support.h"

#include "pyui/widgets/radio_button.h"

namespace pyui {
namespace test {
namespace {

constexpr int kFontHeight = 12;

// 12px 外圆在 40 高视图里垂直居中：top = (40-18)/2 = 11
constexpr Rect kCircle{0, 11, 18, 29};

Color idleBorder() { return blendColor(defaultTheme().controlBorder, defaultTheme().text, 0.38f); }

}  // namespace

TEST(radio_draw_commands_unchecked) {
  RadioButton view;
  view.setLabel("AB");

  const Theme& t = defaultTheme();
  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 未选且未悬停：不铺底，只描一圈
      cmd::strokeRound(kCircle, idleBorder(), 9, 1),
      // 文字从圆心右缘 + 8 起
      cmd::text(Rect{26, 0, 200, 40}, "AB", t.text, kFontHeight, TextAlign::kLeft),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(radio_draw_commands_checked) {
  RadioButton view;
  view.setLabel("AB");
  view.setChecked(true);

  const Theme& t = defaultTheme();
  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 已选：先铺白底再描强调色圆环
      cmd::fillRound(kCircle, t.pane, 9),
      cmd::strokeRound(kCircle, t.accent, 9, 1),
      // 实心点内缩 5px：18 → 8，与外环之间留一圈空隙
      cmd::fillRound(Rect{5, 16, 13, 24}, t.accent, 4),
      cmd::text(Rect{26, 0, 200, 40}, "AB", t.text, kFontHeight, TextAlign::kLeft),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(radio_checked_dot_is_concentric_and_inset) {
  RadioButton view;
  view.setChecked(true);
  const auto commands = recordDirect(view, Size{60, 40});

  // 找圆心相同的两个圆：外环与实心点。不同心是最常见的画错法，
  // 看起来像点「掉」到了圆的一边。
  const DrawCommand* ring = nullptr;
  const DrawCommand* dot = nullptr;
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kStrokeRoundRect) ring = &command;
    if (command.kind == DrawCommand::Kind::kFillRoundRect && command.radius < 9) dot = &command;
  }
  CHECK(ring != nullptr);
  CHECK(dot != nullptr);
  if (ring == nullptr || dot == nullptr) return;

  CHECK_EQ(dot->rect.center().x, ring->rect.center().x);
  CHECK_EQ(dot->rect.center().y, ring->rect.center().y);
  CHECK_MSG(dot->rect.width() < ring->rect.width() / 2, "实心点应明显小于外环，否则会看成实心圆");
}

TEST(radio_ring_radius_is_half_box) {
  RadioButton view;
  const auto commands = recordDirect(view, Size{60, 40});
  const DrawCommand* ring = firstOf(commands, DrawCommand::Kind::kStrokeRoundRect);
  CHECK(ring != nullptr);
  if (ring == nullptr) return;
  // 半径必须正好是边长一半，否则「圆」会变成圆角方块
  CHECK_EQ(ring->radius, ring->rect.width() / 2);
}

TEST(radio_group_click_deselects_others) {
  RadioGroup group;
  RadioButton a;
  RadioButton b;

  a.setLabel("A");
  b.setLabel("B");
  a.setGroup(&group);
  b.setGroup(&group);

  b.setChecked(true);
  CHECK(b.isChecked());
  CHECK(!a.isChecked());

  a.dispatchClick();
  CHECK(a.isChecked());
  CHECK_MSG(!b.isChecked(), "点 A 必须自动取消 B，否则同组会出现两个选中项");
  CHECK(group.selected() == &a);
}

TEST(radio_click_does_not_toggle_itself_off) {
  RadioGroup group;
  RadioButton button;
  button.setGroup(&group);
  button.setChecked(true);

  button.dispatchClick();
  // 单选按钮不接受「点自己取消」：那是系统里不存在的状态
  CHECK(button.isChecked());
}

TEST(radio_group_reports_change_once) {
  RadioGroup group;
  RadioButton a;
  RadioButton b;
  a.setGroup(&group);
  b.setGroup(&group);

  int calls = 0;
  RadioButton* last = nullptr;
  group.setOnChange([&](RadioButton* button) {
    ++calls;
    last = button;
  });

  b.dispatchClick();
  CHECK_EQ(calls, 1);
  CHECK(last == &b);

  // 重复点已选中的项不该再发通知：宿主据此决定是否落盘
  b.dispatchClick();
  CHECK_EQ(calls, 1);
}

TEST(radio_group_join_adopts_checked_button) {
  RadioGroup group;
  RadioButton button;
  button.setChecked(true);
  // 入组前已经选中：组应把它当成当前选中项，而不是要求再点一次
  button.setGroup(&group);
  CHECK(group.selected() == &button);
}

TEST(radio_space_key_selects) {
  RootHarness harness(Size{200, 40});
  RadioButton* view = harness.add<RadioButton>();
  view->setLabel("A");
  harness.root().setFocusedView(view);

  CHECK(!view->isChecked());
  CHECK(harness.root().dispatchKeyDown(Key::kSpace, Modifiers{}));
  CHECK(view->isChecked());

  // 回车不是单选框的语义（那是默认按钮）
  CHECK(!harness.root().dispatchKeyDown(Key::kEnter, Modifiers{}));
}

TEST(radio_hover_fills_circle_with_hover_color) {
  RootHarness harness(Size{200, 40});
  RadioButton* view = harness.add<RadioButton>();

  const Color idle = harness.render().at(9, 20);
  MouseEvent move;
  move.position = Point{9, 20};
  harness.root().dispatchMouseMove(move);
  CHECK(view->isHot());

  const Color hot = harness.render().at(9, 20);
  CHECK_MSG(hot != idle, "悬停必须换底色");
  CHECK(sameRgb(hot, defaultTheme().hover));
}

TEST(radio_hover_on_checked_keeps_accent_fill) {
  RootHarness harness(Size{200, 40});
  RadioButton* view = harness.add<RadioButton>();
  view->setChecked(true);
  harness.relayout();

  MouseEvent move;
  move.position = Point{9, 20};
  harness.root().dispatchMouseMove(move);

  // 已选优先于悬停：圆心必须仍是白底 + 强调色点，不能整块变灰
  const Surface surface = harness.render();
  CHECK(sameRgb(surface.at(9, 20), defaultTheme().accent));
  CHECK(sameRgb(surface.at(9, 13), defaultTheme().pane));
}

TEST(radio_disabled_uses_faint_text_and_grey_ring) {
  RadioButton view;
  view.setLabel("AB");
  view.setEnabled(false);
  const auto commands = recordDirect(view, Size{200, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->color, defaultTheme().textDisabled);

  const DrawCommand* ring = firstOf(commands, DrawCommand::Kind::kStrokeRoundRect);
  CHECK(ring != nullptr);
  // 禁用描边不能沿用启用态那支混色，否则看起来仍然可点
  if (ring != nullptr) CHECK(ring->color != idleBorder());
}

TEST(radio_measure_accounts_for_circle_gap_text) {
  RadioButton view;
  view.setLabel("AB");
  // 18 + 8 + 12 = 38；高度取圆（18）与行高（16）的较大者
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 38);
  CHECK_EQ(measured.height, 18);
}

TEST(radio_label_absent_leaves_no_text_gap) {
  RadioButton view;
  const Size measured = view.measure(Size{0, 0});
  // 没有标签时不该留着 8px 的间距
  CHECK_EQ(measured.width, 18);

  const auto commands = recordDirect(view, Size{60, 40});
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kText), 0);
}

TEST(radio_outside_ring_is_transparent) {
  RadioButton view;
  view.setChecked(true);
  const Surface surface = renderDirect(view, Size{60, 40});

  // 圆心 (9,20)、半径 9：四角必然在圆外
  CHECK(isTransparent(surface, 0, 11));
  CHECK(isTransparent(surface, 17, 11));
  CHECK(isTransparent(surface, 0, 28));
  CHECK(isTransparent(surface, 17, 28));
  // 圆的上沿中点是实心的
  CHECK(!isTransparent(surface, 9, 11));
}

}  // namespace test
}  // namespace pyui
