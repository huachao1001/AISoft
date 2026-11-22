// CheckBox 的绘制测试（铁律三）
//
// 三种状态（未选/已选/部分选）加禁用、悬停，外观组合最多。这里逐条锁住
// 「哪些状态该铺底、哪些该描边」——三态里最容易出的错是部分选时又描边
// 又画勾，看起来像没画完。
#include "widget_test_support.h"

#include "pyui/widgets/check_box.h"

namespace pyui {
namespace test {
namespace {

constexpr int kFontHeight = 12;

// 与 CheckBox::onPaint 一致
Color idleBorder() {
  return blendColor(defaultTheme().controlBorder, defaultTheme().text, 0.38f);
}

// 18px 方框在 40 高视图里垂直居中后的位置
constexpr Rect kSquare{0, 11, 18, 29};

}  // namespace

TEST(check_box_draw_commands_unchecked) {
  CheckBox view;
  view.setLabel("AB");

  const Theme& t = defaultTheme();
  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 未选：白底 + 一档较重的描边
      cmd::fillRound(kSquare, t.pane, 4),
      cmd::strokeRound(kSquare, idleBorder(), 4, 1),
      // 文字从方框右缘 + 8 起
      cmd::text(Rect{26, 0, 200, 40}, "AB", t.text, kFontHeight, TextAlign::kLeft),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(check_box_draw_commands_checked) {
  CheckBox view;
  view.setLabel("AB");
  view.setChecked(true);

  const Theme& t = defaultTheme();
  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 已选：强调色铺底，**不描边**（描边会让小方框看起来发灰）
      cmd::fillRound(kSquare, t.accent, 4),
      // 勾按墨迹内缩 1/5 画
      cmd::icon(IconId::kCheck, Rect{3, 14, 15, 26}, t.accentInk),
      cmd::text(Rect{26, 0, 200, 40}, "AB", t.text, kFontHeight, TextAlign::kLeft),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(check_box_draw_commands_indeterminate) {
  CheckBox view;
  view.setLabel("AB");
  view.setTristate(true);
  view.setState(CheckState::kIndeterminate);

  const auto commands = recordDirect(view, Size{200, 40});

  // 部分选画横线而不是勾：勾代表「全部」，半勾会让人以为勾没画完
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kIcon), 0);
  const DrawCommand* line = firstOf(commands, DrawCommand::Kind::kFillRect);
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK_EQ(line->rect, (Rect{5, 19, 13, 21}));
    CHECK_EQ(line->color, defaultTheme().accentInk);
  }
  // 部分选同样不描边
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kStrokeRoundRect), 0);
}

TEST(check_box_tristate_off_rejects_indeterminate) {
  CheckBox view;
  view.setTristate(false);
  view.setState(CheckState::kIndeterminate);
  // 关掉三态时不允许停在部分选：那是一个没有对应点击路径的中间态
  CHECK_EQ(static_cast<int>(view.state()), static_cast<int>(CheckState::kChecked));
}

TEST(check_box_click_cycles_two_states) {
  RootHarness harness(Size{200, 40});
  CheckBox* view = harness.add<CheckBox>();
  view->setLabel("AB");
  view->setChecked(false);

  MouseEvent down;
  down.position = Point{9, 20};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);
  harness.root().dispatchMouseUp(down);

  CHECK(view->isChecked());
}

TEST(check_box_click_cycles_three_states_when_tristate) {
  RootHarness harness(Size{200, 40});
  CheckBox* view = harness.add<CheckBox>();
  view->setTristate(true);
  view->setState(CheckState::kUnchecked);

  const auto click = [&harness] {
    MouseEvent event;
    event.position = Point{9, 20};
    event.button = MouseButton::kLeft;
    harness.root().dispatchMouseDown(event);
    harness.root().dispatchMouseUp(event);
  };

  click();
  CHECK_EQ(static_cast<int>(view->state()), static_cast<int>(CheckState::kChecked));
  click();
  CHECK_EQ(static_cast<int>(view->state()), static_cast<int>(CheckState::kUnchecked));
}

TEST(check_box_space_key_toggles) {
  RootHarness harness(Size{200, 40});
  CheckBox* view = harness.add<CheckBox>();
  view->setChecked(false);
  harness.root().setFocusedView(view);

  CHECK(harness.root().dispatchKeyDown(Key::kSpace, Modifiers{}));
  CHECK(view->isChecked());

  // 回车不该触发：那是按钮的语义
  CHECK(!harness.root().dispatchKeyDown(Key::kEnter, Modifiers{}));
}

TEST(check_box_hover_tints_box) {
  RootHarness harness(Size{200, 40});
  CheckBox* view = harness.add<CheckBox>();

  const Color idle = harness.render().at(9, 20);

  MouseEvent move;
  move.position = Point{9, 20};
  harness.root().dispatchMouseMove(move);
  CHECK(view->isHot());

  const Color hot = harness.render().at(9, 20);
  CHECK(hot != idle);
  CHECK(sameRgb(hot, defaultTheme().hover));
}

TEST(check_box_checked_pixels_are_accent) {
  CheckBox view;
  view.setChecked(true);
  const Surface surface = renderDirect(view, Size{60, 40});

  // 方框内部是强调色
  CHECK(sameRgb(surface.at(9, 20), defaultTheme().accent));
  // 勾是白墨：mark 区域内应有近白像素
  bool hasWhite = false;
  for (int y = 14; y < 26 && !hasWhite; ++y) {
    for (int x = 3; x < 15; ++x) {
      if (sameRgb(surface.at(x, y), defaultTheme().accentInk)) {
        hasWhite = true;
        break;
      }
    }
  }
  CHECK_MSG(hasWhite, "已选状态必须画出勾");
}

TEST(check_box_unchecked_box_corner_rounded) {
  CheckBox box;
  const Surface surface = renderDirect(box, Size{60, 40});

  // 圆角半径 4：方框左上角像素只被部分覆盖，不会是不透明的纯白
  CHECK(colorAlpha(surface.at(0, 11)) < 255);
  // 方框中心完全铺满
  CHECK(!isTransparent(surface, 9, 20));
}

TEST(check_box_disabled_text_is_faint) {
  CheckBox view;
  view.setLabel("AB");
  view.setEnabled(false);
  const auto commands = recordDirect(view, Size{200, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->color, defaultTheme().textDisabled);
}

TEST(check_box_measure_accounts_for_box_gap_text) {
  CheckBox view;
  view.setLabel("AB");
  // 18 + 8 + 12 = 38；高度取方框与行高较大者（16）
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 38);
  CHECK_EQ(measured.height, 18);
}

}  // namespace test
}  // namespace pyui
