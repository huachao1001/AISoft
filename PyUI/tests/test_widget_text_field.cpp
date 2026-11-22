// TextField 的绘制测试（铁律三）
//
// 输入框是「文字四处同源」（铁律七）唯一真正落地的地方：光标 x、选区边界、
// 绘制起点必须来自同一套排版。所以这里的断言分三类：
//
//   1. 命令序列：底板、描边、裁剪、文字、光标各自的矩形；
//   2. 像素：光标确实落在文字之间、占位提示是淡色、圆角外透明；
//   3. 排版一致性：点击位置换算出的光标必须与 byteToX 算出的 x 互相吻合。
//
// 第 3 类最关键——它不写死任何一个数字，只要求同一份排版在两个方向上
// 自洽，真字体换进来也依然成立。
#include "widget_test_support.h"

#include "pyui/widgets/text_field.h"

namespace pyui {
namespace test {
namespace {

constexpr int kFontHeight = 12;   // 9pt @96dpi
constexpr int kLineHeight = 16;   // 12 * 4/3
constexpr int kPad = 10;          // TextField::padding_
constexpr int kCaretBar = 2;      // 1.5 设计像素在 96 DPI 下取整

// 掩码字符 U+25CF，三字节
const std::string kMask = u8"\u25CF";

// 200×36 下文字区：上下按行高居中 → {10, 10, 190, 26}
constexpr Rect kArea{10, 10, 190, 26};

}  // namespace

TEST(text_field_draw_commands_empty) {
  TextField view;
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 36}),
      // 底板与描边同半径：描边向内，恰好压在白底边缘一个像素
      cmd::fillRound(Rect{0, 0, 200, 36}, t.inputBg, 4),
      cmd::strokeRound(Rect{0, 0, 200, 36}, t.controlBorder, 4, 1),
      // 内容层单独 save/clip：滚动时文字不会溢出圆角
      cmd::save(),
      cmd::clipRect(Rect{kPad, 0, 190, 36}),
      cmd::restore(),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 36}), expected);
}

TEST(text_field_placeholder_is_drawn_in_faint_color) {
  TextField view;
  view.setPlaceholder("Name");
  const auto commands = recordDirect(view, Size{200, 36});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;

  CHECK_EQ(text->text, std::string("Name"));
  CHECK_EQ(text->color, defaultTheme().textFaint);
  // 占位提示与真实内容共用同一行框，切到真实内容时高度不跳
  CHECK_EQ(text->rect, kArea);
}

TEST(text_field_placeholder_disappears_once_text_entered) {
  TextField view;
  view.setPlaceholder("Name");
  view.setText("Bob");
  const auto commands = recordDirect(view, Size{200, 36});

  // 有内容时占位提示必须完全消失，且内容不是淡色
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kText), 0);
  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run != nullptr) {
    CHECK_EQ(run->text, std::string("Bob"));
    CHECK(run->color != defaultTheme().textFaint);
  }
}

TEST(text_field_text_uses_draw_text_run) {
  TextField view;
  view.setText("Ab");
  const auto commands = recordDirect(view, Size{200, 36});

  // 必须走 drawTextRun：它与 advanceText 同源，光标才落在字形之间。
  // 换成 drawText 时会多算一次居中留白，光标整体偏半格（铁律七）。
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kText), 0);
  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run == nullptr) return;

  CHECK_EQ(run->text, std::string("Ab"));
  CHECK_EQ(run->rect.left, kArea.left);
  // 2 字符 × 6px 步进 + 一个字宽的光标余量
  CHECK_EQ(run->rect.right, kArea.left + 12 + kCaretBar);
  CHECK_EQ(run->fontHeight, kFontHeight);
}

TEST(text_field_caret_sits_after_last_character) {
  TextField view;
  view.setText("Ab");
  // setText 把光标放在末尾：这是「输入后继续打字」的默认行为
  CHECK_EQ(view.caretByte(), size_t{2});
}

TEST(text_field_selection_paints_band_before_text) {
  TextField view;
  view.setText("Abcd");
  view.setSelection(0, 2);
  const auto commands = recordDirect(view, Size{200, 36});

  const DrawCommand* band = firstOf(commands, DrawCommand::Kind::kFillRect);
  CHECK(band != nullptr);
  if (band == nullptr) return;

  // 2 字符 × 6px = 12px 宽，起于文字区左缘
  CHECK_EQ(band->rect, (Rect{kArea.left, kArea.top, kArea.left + 12, kArea.bottom}));
  CHECK_EQ(band->color, defaultTheme().selBg);

  // 选区层必须在文字之前：先铺底再压字，否则文字会被盖住
  size_t bandIndex = 0;
  size_t runIndex = 0;
  for (size_t i = 0; i < commands.size(); ++i) {
    if (commands[i].kind == DrawCommand::Kind::kFillRect) bandIndex = i;
    if (commands[i].kind == DrawCommand::Kind::kTextRun) runIndex = i;
  }
  CHECK(bandIndex < runIndex);
}

TEST(text_field_password_masks_every_codepoint) {
  TextField view;
  view.setText("abc");
  view.setPassword(true);
  CHECK(view.isPassword());

  const auto commands = recordDirect(view, Size{200, 36});
  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run == nullptr) return;

  // 每个码点换成一个掩码字符，而不是按字节替换
  CHECK_EQ(run->text, kMask + kMask + kMask);
}

TEST(text_field_password_masks_multibyte_text) {
  TextField view;
  view.setText(u8"中文");
  view.setPassword(true);

  const auto commands = recordDirect(view, Size{200, 36});
  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run != nullptr) {
    // 两个汉字 → 两个掩码字符。按字节替换会得到 6 个，长度直接翻三倍
    CHECK_EQ(run->text, kMask + kMask);
  }
}

TEST(text_field_long_text_scrolls_to_keep_caret_visible) {
  TextField view;
  // 40 字符 × 6px = 240px，远超 180px 的文字区
  view.setText(std::string(40, 'a'));
  const auto commands = recordDirect(view, Size{200, 36});

  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run == nullptr) return;

  CHECK_MSG(run->rect.left < kArea.left, "末尾光标不可见时必须向左滚动内容");
  // 光标位置要从绘制起点反推：起点 + 40 字符宽度 应落在文字区右界之内
  CHECK(run->rect.left + 240 + kCaretBar <= kArea.right + 1);
}

TEST(text_field_short_text_never_scrolls) {
  TextField view;
  view.setText("Ab");
  const auto commands = recordDirect(view, Size{200, 36});
  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  // 放得下就不该滚动：滚动量非零会让文字贴着方框左缘消失一半
  if (run != nullptr) CHECK_EQ(run->rect.left, kArea.left);
}

TEST(text_field_click_positions_caret_at_nearest_boundary) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("abcd");
  harness.relayout();

  // 40 高时文字区 {10, 12, 190, 28}；每字符 6px → 边界 10/16/22/28/34
  const auto click = [&](int x) {
    MouseEvent event;
    event.position = Point{x, 20};
    event.button = MouseButton::kLeft;
    harness.root().dispatchMouseDown(event);
    harness.root().dispatchMouseUp(event);
  };

  click(11);  // 第 1 个字符左半
  CHECK_EQ(view->caretByte(), size_t{0});
  click(21);  // 第 2 个字符右半 → 更靠近 22
  CHECK_EQ(view->caretByte(), size_t{2});
  click(180);  // 越过文字末尾（仍在视图内）→ 夹到 4
  CHECK_EQ(view->caretByte(), size_t{4});
}

TEST(text_field_shift_click_extends_selection) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("abcd");
  harness.relayout();

  MouseEvent down;
  down.position = Point{11, 20};
  down.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(down);
  harness.root().dispatchMouseUp(down);
  CHECK_EQ(view->selectionStart(), size_t{0});

  MouseEvent shiftDown = down;
  shiftDown.position = Point{23, 20};
  shiftDown.modifiers.shift = true;
  harness.root().dispatchMouseDown(shiftDown);

  CHECK(view->hasSelection());
  // 锚点保持 0，光标跟到 22 附近 → 选区 0..2
  CHECK_EQ(view->selectionStart(), size_t{0});
  CHECK_EQ(view->selectionEnd(), size_t{2});
}

TEST(text_field_typing_replaces_selection) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("abcd");
  view->setSelection(1, 3);
  harness.root().setFocusedView(view);

  CHECK(harness.root().dispatchChar('X'));
  // 选区被替换，而不是在选区旁边插入
  CHECK_EQ(view->text(), std::string("aXd"));
  CHECK_EQ(view->caretByte(), size_t{2});
}

TEST(text_field_backspace_deletes_one_character) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText(u8"中文");
  harness.root().setFocusedView(view);

  CHECK(harness.root().dispatchKeyDown(Key::kBackspace, Modifiers{}));
  // 删掉的是一个完整汉字，不是半个字节
  CHECK_EQ(view->text(), std::string(u8"中"));
}

TEST(text_field_ctrl_a_selects_all) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("abcd");
  harness.root().setFocusedView(view);

  Modifiers ctrl;
  ctrl.ctrl = true;
  CHECK(harness.root().dispatchKeyDown(Key::kA, ctrl));
  CHECK_EQ(view->selectionStart(), size_t{0});
  CHECK_EQ(view->selectionEnd(), size_t{4});
}

TEST(text_field_read_only_ignores_typing) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("fixed");
  view->setReadOnly(true);
  harness.root().setFocusedView(view);

  CHECK(!harness.root().dispatchChar('x'));
  CHECK_EQ(view->text(), std::string("fixed"));
}

TEST(text_field_enter_reports_submit) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("query");

  std::string submitted;
  view->setOnSubmit([&](const std::string& text) {
    submitted = text;
    return true;
  });
  harness.root().setFocusedView(view);

  CHECK(harness.root().dispatchKeyDown(Key::kEnter, Modifiers{}));
  CHECK_EQ(submitted, std::string("query"));
}

TEST(text_field_focus_draws_accent_caret_and_border) {
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  harness.root().setFocusedView(view);
  CHECK(view->isFocused());

  const Surface surface = harness.render();
  // 焦点描边换成强调色：这是用户判断「键盘输入会进哪里」的唯一线索
  CHECK(sameRgb(surface.at(100, 0), defaultTheme().inputFocus));
  // 光标在内边距起点，颜色是强调色
  CHECK(sameRgb(surface.at(kPad, 20), defaultTheme().accent));
}

TEST(text_field_unfocused_has_no_caret) {
  TextField view;
  const Surface surface = renderDirect(view, Size{200, 40});
  // 输入框始终有白底，所以「没有光标」不能靠透明判断，要看有没有强调色像素
  const Color accent = defaultTheme().accent;
  for (int y = 0; y < 40; ++y) {
    for (int x = kPad; x < 180; ++x) {
      if (sameRgb(surface.at(x, y), accent)) {
        FAIL_AT("未聚焦的空输入框画出了光标");
        return;
      }
    }
  }
}

TEST(text_field_caret_sits_between_glyphs) {
  // 排版自洽性：把光标放在第 n 个字符后，画出来的光标必须紧贴该字符的
  // 右边界。这条不写死步进宽度，真字体换进来也成立——它比较的是
  // 「光标 x」与「第 n 个字符的排版本宽度」两个同源量。
  RootHarness harness(Size{200, 40});
  TextField* view = harness.add<TextField>();
  view->setText("abcd");
  view->setCaretByte(2);
  harness.root().setFocusedView(view);

  const Surface surface = harness.render();
  const Color accent = defaultTheme().accent;
  const Color ink = defaultTheme().text;

  // 光标是强调色、占满行高；字形是墨色、只占行高的 4/5。用颜色区分两者。
  int caretLeft = -1;
  int inkBeforeCaret = 0;
  for (int x = kPad; x < 180; ++x) {
    bool isAccentColumn = false;
    for (int y = 12; y < 28; ++y) {
      if (sameRgb(surface.at(x, y), accent)) isAccentColumn = true;
    }
    if (isAccentColumn) {
      caretLeft = x;
      break;
    }
    if (sameRgb(surface.at(x, 20), ink)) ++inkBeforeCaret;
  }

  CHECK_MSG(caretLeft >= 0, "聚焦的输入框必须画出光标");
  if (caretLeft < 0) return;

  // 光标起点必须等于「前两个字符的排版本宽度」——同源量比较，不写死 12。
  const Font font = uiFont(9.0f, false, false, 96);
  CHECK_EQ(caretLeft, kPad + measureTextWidth("ab", font));
  // 光标左侧确实有文字，否则「光标停在第 0 个字符后」也会通过
  CHECK_MSG(inkBeforeCaret > 0, "光标前应已画出前两个字符");
}

TEST(text_field_corners_are_transparent) {
  TextField view;
  const Surface surface = renderDirect(view, Size{60, 36});

  // 半径 4：四角必然落在圆外
  CHECK(isTransparent(surface, 0, 0));
  CHECK(isTransparent(surface, 59, 0));
  CHECK(isTransparent(surface, 0, 35));
  CHECK(isTransparent(surface, 59, 35));
  // 边中点完全铺满
  CHECK(!isTransparent(surface, 30, 0));
}

TEST(text_field_never_draws_outside_bounds) {
  TextField view;
  view.setText(std::string(200, 'a'));
  const Surface surface = renderDirect(view, Size{60, 36});
  const Rect ink = alphaBounds(surface);
  CHECK(ink.right <= 60);
  CHECK(ink.bottom <= 36);
}

TEST(text_field_measure_follows_text_and_padding) {
  TextField view;
  view.setText("Ab");
  // 12px 文本 + 左右各 10px 内边距；高度取标准控件高 36
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 32);
  CHECK_EQ(measured.height, 36);
}

TEST(text_field_measure_uses_placeholder_when_empty) {
  TextField view;
  view.setPlaceholder("Name");
  // 空输入框也要按占位提示撑开，否则初始宽度是 20px 显得像个空槽
  CHECK_EQ(view.measure(Size{0, 0}).width, 24 + 20);
}

TEST(text_field_line_box_is_vertically_centered) {
  TextField view;
  view.setText("Ab");
  const auto commands = recordDirect(view, Size{200, 36});

  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run == nullptr) return;
  // {0,0,200,36} 里行高 16 → 上下各留 10
  CHECK_EQ(run->rect.top, (36 - kLineHeight) / 2);
  CHECK_EQ(run->rect.height(), kLineHeight);
}

TEST(text_field_alignment_centers_short_text) {
  TextField view;
  view.setText("Ab");
  view.setAlign(TextAlign::kCenter);
  const auto commands = recordDirect(view, Size{200, 36});

  const DrawCommand* run = firstOf(commands, DrawCommand::Kind::kTextRun);
  CHECK(run != nullptr);
  if (run == nullptr) return;
  // 180px 文字区里放 12px → 起点 10 + 84
  CHECK_EQ(run->rect.left, kArea.left + (kArea.width() - 12) / 2);
}

}  // namespace test
}  // namespace pyui
