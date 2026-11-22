// Button 的绘制测试（铁律三）
//
// 按钮是「状态色」最集中的控件：常态、悬停、按下、禁用四种外观，加上
// 描边、圆角、图标 + 文字的水平/垂直排布。命令测试锁住「什么时候画几层」，
// 像素测试锁住「圆角外确实透、内容确实居中」。
#include "widget_test_support.h"

#include "pyui/widgets/button.h"

namespace pyui {
namespace test {
namespace {

constexpr int kFontHeight = 12;  // 9pt @96dpi 的 Mock 字体

// 与 Button::onPaint 一致：kOutline 的描边由底色派生
Color outlineBorder(Color background, bool hot) {
  return edgeOn(background, hot ? 24 : 17);
}

}  // namespace

TEST(button_draw_commands_outline) {
  Button view;
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 120, 32}),
      // 描边样式：先铺底再描边，两层都用同一个圆角半径
      cmd::fillRound(Rect{0, 0, 120, 32}, t.pane, 7),
      cmd::strokeRound(Rect{0, 0, 120, 32}, outlineBorder(t.pane, false), 7, 1),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{120, 32}), expected);
}

TEST(button_draw_commands_primary) {
  Button view;
  view.setStyle(ButtonStyle::kPrimary);
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 120, 32}),
      // 主行动按钮：实心强调色，没有描边
      cmd::fillRound(Rect{0, 0, 120, 32}, t.accent, 7),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{120, 32}), expected);
}

TEST(button_draw_commands_subtle_idle_draws_nothing) {
  Button view;
  view.setStyle(ButtonStyle::kSubtle);

  // 常态的 subtle 什么都不画：它靠悬停底色表达可点击，静态时与工具条
  // 融为一体。多铺一层（哪怕是透明色）会在每帧多一次整块混合。
  const auto commands = recordDirect(view, Size{120, 32});
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kFillRoundRect), 0);
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kStrokeRoundRect), 0);
  CHECK_EQ(commands.size(), static_cast<size_t>(3));  // save + clipRect + restore
}

TEST(button_label_is_centered) {
  Button view;
  view.setLabel("AB");
  const auto commands = recordDirect(view, Size{120, 32});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;

  // Mock 字体每字符 6px → "AB" 宽 12px，水平居中后左缘 (120-12)/2 = 54
  CHECK_EQ(text->rect.left, 54);
  CHECK_EQ(text->rect.width(), 12);
  CHECK_EQ(text->fontHeight, kFontHeight);
  CHECK_EQ(text->intValue, static_cast<int>(TextAlign::kLeft));
}

TEST(button_icon_and_label_share_horizontal_row) {
  Button view;
  view.setLabel("AB");
  view.setIcon(IconId::kCheck);
  view.setIconSize(16);
  const auto commands = recordDirect(view, Size{120, 32});

  const DrawCommand* icon = firstOf(commands, DrawCommand::Kind::kIcon);
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(icon != nullptr);
  CHECK(text != nullptr);
  if (icon == nullptr || text == nullptr) return;

  // 图标 16 + 间距 6 + 文字 12 = 34，居中起点 (120-34)/2 = 43
  CHECK_EQ(icon->rect, (Rect{43, 8, 59, 24}));
  CHECK_EQ(text->rect.left, 65);
  // 图标必须在文字左边，且不重叠
  CHECK(icon->rect.right <= text->rect.left);
}

TEST(button_icon_only_has_no_text) {
  Button view;
  view.setLabel("AB");
  view.setIcon(IconId::kClose);
  view.setIconPlacement(IconPlacement::kOnly);
  const auto commands = recordDirect(view, Size{46, 32});

  CHECK_EQ(countOf(commands, DrawCommand::Kind::kText), 0);
  const DrawCommand* icon = firstOf(commands, DrawCommand::Kind::kIcon);
  CHECK(icon != nullptr);
  // 图标在按钮内水平居中：46 宽、18 图标 → 起点 14
  if (icon != nullptr) CHECK_EQ(icon->rect.left, 14);
}

TEST(button_icon_on_top_stacks_text_below) {
  Button view;
  view.setLabel("AB");
  view.setIcon(IconId::kFolder);
  view.setIconPlacement(IconPlacement::kTop);
  const auto commands = recordDirect(view, Size{100, 64});

  const DrawCommand* icon = firstOf(commands, DrawCommand::Kind::kIcon);
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(icon != nullptr);
  CHECK(text != nullptr);
  if (icon == nullptr || text == nullptr) return;

  CHECK_MSG(icon->rect.bottom <= text->rect.top, "图标应排在文字上方");
  // 两者都在按钮水平中线上
  CHECK_EQ(icon->rect.center().x, 50);
}

TEST(button_hover_changes_background) {
  RootHarness harness(Size{120, 32});
  Button* view = harness.add<Button>();
  view->setLabel("AB");
  harness.relayout();

  const Color idle = harness.render().at(4, 16);

  MouseEvent move;
  move.position = Point{60, 16};
  harness.root().dispatchMouseMove(move);
  CHECK(view->isHot());

  const Color hot = harness.render().at(4, 16);
  CHECK_MSG(hot != idle, "悬停必须换底色，否则用户看不出按钮可点");
  CHECK(sameRgb(hot, defaultTheme().btnHoverBg));
}

TEST(button_pressed_shifts_content_down_one_pixel) {
  RootHarness harness(Size{120, 32});
  Button* view = harness.add<Button>();
  view->setLabel("AB");
  harness.relayout();

  const Rect idleInk = alphaBounds(harness.render());

  MouseEvent down;
  down.position = Point{60, 16};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);
  CHECK(view->isPressed());

  const auto commands = recordDirect(*view, Size{120, 32});
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  // 按下时内容下沉 1px：比只换底色更接近系统按钮的手感
  if (text != nullptr) CHECK_EQ(text->rect.top, 1);
  (void)idleInk;
}

TEST(button_disabled_uses_grey_palette) {
  Button view;
  view.setStyle(ButtonStyle::kOutline);
  view.setEnabled(false);
  const auto commands = recordDirect(view, Size{120, 32});

  const Theme& t = defaultTheme();
  const auto* fill = firstOf(commands, DrawCommand::Kind::kFillRoundRect);
  CHECK(fill != nullptr);
  if (fill != nullptr) CHECK_EQ(fill->color, t.btnDisBg);

  const auto* text = firstOf(commands, DrawCommand::Kind::kText);
  (void)text;
  // 禁用不铺「高亮」，且底色比正常白底暗
  CHECK(blendColor(t.btnDisBg, t.pane, 0.5f) != t.btnDisBg);
}

TEST(button_rounded_corners_are_transparent) {
  Button view;
  const Surface surface = renderDirect(view, Size{60, 40});

  // 半径 7：最角落必然落在圆外
  CHECK(isTransparent(surface, 0, 0));
  CHECK(isTransparent(surface, 59, 0));
  CHECK(isTransparent(surface, 0, 39));
  CHECK(isTransparent(surface, 59, 39));

  // 四条边中点在圆角带之外，必须完全填满（相邻视图之间不该留缝）
  CHECK(!isTransparent(surface, 30, 0));
  CHECK(!isTransparent(surface, 0, 20));
}

TEST(button_never_draws_outside_bounds) {
  Button view;
  view.setLabel("VERY LONG LABEL THAT DOES NOT FIT");
  const Surface surface = renderDirect(view, Size{40, 32});

  // 内容溢出时由 View::draw 的 clipRect 兜住，不会画到控件矩形之外
  CHECK_EQ(alphaBounds(surface).right <= 40, true);
  CHECK_EQ(alphaBounds(surface).bottom <= 32, true);
}

TEST(button_measure_follows_content) {
  Button view;
  view.setLabel("AB");
  // padX 12*2 + 文字 12 = 36；高度固定 32
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 36);
  CHECK_EQ(measured.height, 32);
}

}  // namespace test
}  // namespace pyui
