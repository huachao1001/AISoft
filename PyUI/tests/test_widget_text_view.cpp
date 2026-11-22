// TextView 的绘制测试（铁律三）
//
// 两条断言各管一段：
//   - 录制型画笔：控件「想画什么」跨平台逐字节相同；
//   - 软件光栅器 + Mock 字体：画出来的形状确实落在该在的位置。
//
// Mock 字体在 96 DPI、9pt 下是 12px 高、行高 16px、每字符 6px（含 1px 缝）。
// 这组数字固定，所以像素断言可以写死，而不必忍受真字体的抖动。
#include "widget_test_support.h"

#include "pyui/widgets/text_view.h"

namespace pyui {
namespace test {
namespace {

constexpr int kFontHeight = 12;  // 9pt @96dpi
constexpr int kFallbackLineHeight = 16;

// 录制型画笔的推进宽度是 7（不是 Mock 字体的 6）：省略号二分会走
// RecordingPainter::advanceText，所以命令层测试必须按 7 计算前缀长度。
constexpr int kRecordAdvance = 7;

}  // namespace

TEST(text_view_draw_commands) {
  TextView view("H");
  const Size size{200, 40};

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 无底色：TextView 默认 resolveBackground() == kNoColor
      cmd::text(Rect{0, 0, 200, 40}, "H", defaultTheme().text, kFontHeight, TextAlign::kLeft),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, size), expected);
}

TEST(text_view_has_no_default_background) {
  TextView view("H");
  const auto commands = recordDirect(view, Size{200, 40});

  // 只有 clipRect 一条填充以外的命令，且不铺底：文字控件叠在父容器上，
  // 自己铺白底会在彩色面板上出现一个突兀的方块。
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kFillRect), 0);
  CHECK(view.backgroundColor() == kNoColor);
}

TEST(text_view_text_block_sits_in_line_box) {
  TextView view("H");
  const Surface surface = renderDirect(view, Size{60, 40});

  // Mock 字形：1 字符占 6px 步进，实心块 5px（右边 1px 是缝），顶部空 1/5 行高
  const Rect ink = alphaBounds(surface);
  CHECK_EQ(ink, (Rect{0, 40 / 5, 5, 40}));
  // 整块像素都应是墨色，没有被省略号或裁剪切掉
  CHECK(sameRgb(surface.at(0, 8), defaultTheme().text));
}

TEST(text_view_ellipsis_fits_available_width) {
  TextView view("abcdef");
  const auto commands = recordDirect(view, Size{30, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;

  // 30px 预算、每字符 7px：省略号占 1 个字符位，剩下 23px 只放得下 3 个字符
  CHECK_EQ(text->text, std::string("abc\xE2\x80\xA6"));
  CHECK(text->rect.width() == 30);
}

TEST(text_view_keeps_text_when_it_fits) {
  TextView view("abcdef");
  const auto commands = recordDirect(view, Size{100, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) {
    // 放得下就原样画，不要出现「总是补省略号」的退化
    CHECK_EQ(text->text, std::string("abcdef"));
  }
}

TEST(text_view_alignment_shifts_draw_rect) {
  // 三种对齐下 rc 相同（都是行框），差异只在后端如何摆放字形。
  // 所以断言落在像素上：居中时墨迹应离开左边界。
  TextView left("HH");
  left.setTextStyle([] {
    TextStyle style;
    style.align = TextAlign::kLeft;
    return style;
  }());

  TextView center("HH");
  center.setTextStyle([] {
    TextStyle style;
    style.align = TextAlign::kCenter;
    return style;
  }());

  const Rect leftInk = alphaBounds(renderDirect(left, Size{60, 40}));
  const Rect centerInk = alphaBounds(renderDirect(center, Size{60, 40}));

  CHECK_EQ(leftInk.left, 0);
  // 2 字符 = 12px，居中后左缘约 (60-12)/2 = 24
  CHECK_NEAR(centerInk.left, 24, 1);
  CHECK_NEAR(centerInk.width(), leftInk.width(), 1);
}

TEST(text_view_color_override_wins) {
  TextView view("H");
  view.setColor(makeColor(0xff, 0x00, 0x00));
  const Surface surface = renderDirect(view, Size{40, 40});
  CHECK(sameRgb(surface.at(0, 8), makeColor(0xff, 0x00, 0x00)));
}

TEST(text_view_empty_text_draws_nothing_but_keeps_height) {
  TextView view;
  const Surface surface = renderDirect(view, Size{40, 40});
  CHECK(alphaBounds(surface).isEmpty());

  // 空文本仍占一行高：否则空白标签会把整行高度塌掉
  CHECK_EQ(view.measure(Size{0, 0}).height, kFallbackLineHeight);
}

TEST(text_view_measure_uses_text_and_padding) {
  TextView view("HH");
  const Size measured = view.measure(Size{0, 0});
  CHECK_EQ(measured.width, 12);
  CHECK_EQ(measured.height, kFallbackLineHeight);
}

TEST(text_view_ellipsis_disabled_clips_instead) {
  TextView view("abcdef");
  view.setEllipsis(false);
  const auto commands = recordDirect(view, Size{30, 40});

  // 关掉省略时必须自己开裁剪，否则长文本会溢出到兄弟视图上
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kClipRect), 2);
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->text, std::string("abcdef"));
}

}  // namespace test
}  // namespace pyui
