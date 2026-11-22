// ProgressBar 的绘制测试（铁律三）
//
// 两种模式画法完全不同，所以要分开断言：
//   - 确定态：轨道 + 按比例截断的渐变填充，填充宽度必须「右端取整后仍不
//     越过轨道」——用 floor 会让 50% 看起来像 49%；
//   - 不确定态：一段固定长度短条在轨道内循环，任何相位都不得越出轨道
//     左右边界，否则圆角会在边界外露出一截方角。
#include "widget_test_support.h"

#include "pyui/widgets/progress_bar.h"

namespace pyui {
namespace test {
namespace {

constexpr int kTrackH = 6;     // kSizeTrackH
constexpr int kLineHeight = 16;

// 200×40 无文字时：轨道垂直居中 → {0,17,200,23}
constexpr Rect kTrack{0, 17, 200, 23};

Color fillTop() { return blendColor(defaultTheme().accent, makeColor(0xff, 0xff, 0xff), 0.18f); }

}  // namespace

TEST(progress_bar_draw_commands_empty) {
  ProgressBar view;
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      // 进度 0：只有轨道，没有填充
      cmd::fillRound(kTrack, t.trackBg, kTrackH / 2),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(progress_bar_draw_commands_half) {
  ProgressBar view;
  view.setProgress(0.5f);
  const Theme& t = defaultTheme();

  const std::vector<DrawCommand> expected{
      cmd::save(),
      cmd::clipRect(Rect{0, 0, 200, 40}),
      cmd::fillRound(kTrack, t.trackBg, kTrackH / 2),
      // 填充 100px：强调色到 accentDown 的垂直渐变，纯色长条显得很平
      cmd::gradientRound(Rect{0, 17, 100, 23}, fillTop(), t.accentDown, kTrackH / 2),
      cmd::restore(),
  };

  CHECK_DRAW(recordDirect(view, Size{200, 40}), expected);
}

TEST(progress_bar_full_never_overflows_track) {
  ProgressBar view;
  view.setProgress(1.0f);
  const auto commands = recordDirect(view, Size{200, 40});

  const DrawCommand* fill = firstOf(commands, DrawCommand::Kind::kGradientRoundRect);
  CHECK(fill != nullptr);
  // 100% 时填充右缘必须正好等于轨道右缘：多一像素会盖住轨道的圆角
  if (fill != nullptr) CHECK_EQ(fill->rect.right, kTrack.right);
}

TEST(progress_bar_zero_draws_no_fill) {
  ProgressBar view;
  view.setProgress(0.0f);
  const auto commands = recordDirect(view, Size{200, 40});
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kGradientRoundRect), 0);
}

TEST(progress_bar_progress_is_clamped) {
  ProgressBar view;
  view.setProgress(5.0f);
  CHECK_EQ(view.progress(), 1.0f);

  view.setProgress(-3.0f);
  CHECK_EQ(view.progress(), 0.0f);
}

TEST(progress_bar_setting_progress_leaves_indeterminate) {
  ProgressBar view;
  view.setIndeterminate(true);
  CHECK(view.isIndeterminate());

  // 给了确定进度就退出不确定态：两者同时为真时画哪一条都自相矛盾
  view.setProgress(0.3f);
  CHECK(!view.isIndeterminate());
}

TEST(progress_bar_indeterminate_draws_segment_inside_track) {
  ProgressBar view;
  view.setIndeterminate(true);
  const auto commands = recordDirect(view, Size{200, 40});

  // 不确定态没有渐变填充，只有一段圆角短条
  CHECK_EQ(countOf(commands, DrawCommand::Kind::kGradientRoundRect), 0);
  const DrawCommand* segment = nullptr;
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kFillRoundRect && command.color == defaultTheme().trackFill) {
      segment = &command;
    }
  }
  CHECK(segment != nullptr);
  if (segment == nullptr) return;

  // 200/4 = 50px 短条，起点在轨道左端（相位 0）
  CHECK_EQ(segment->rect, (Rect{0, 17, 50, 23}));
}

TEST(progress_bar_indeterminate_segment_stays_inside_track_at_all_phases) {
  ProgressBar view;
  view.setIndeterminate(true);

  for (int step = 0; step < 80; ++step) {
    const auto commands = recordDirect(view, Size{200, 40});
    const DrawCommand* segment = nullptr;
    for (const DrawCommand& command : commands) {
      if (command.kind == DrawCommand::Kind::kFillRoundRect &&
          command.color == defaultTheme().trackFill) {
        segment = &command;
      }
    }
    CHECK_MSG(segment != nullptr, "不确定态每一帧都要画短条");
    if (segment == nullptr) return;
    // 夹到轨道内：越界会让圆角在边界外露方角
    CHECK_MSG(segment->rect.left >= kTrack.left, "短条左缘越出轨道");
    CHECK_MSG(segment->rect.right <= kTrack.right, "短条右缘越出轨道");
    view.dispatchTimer();
  }
}

TEST(progress_bar_indeterminate_phase_advances) {
  ProgressBar view;
  view.setIndeterminate(true);

  const auto startLeft = [&] {
    const auto commands = recordDirect(view, Size{200, 40});
    for (const DrawCommand& command : commands) {
      if (command.kind == DrawCommand::Kind::kFillRoundRect &&
          command.color == defaultTheme().trackFill) {
        return command.rect.left;
      }
    }
    return -1;
  };

  const int before = startLeft();
  view.dispatchTimer();
  view.dispatchTimer();
  const int after = startLeft();

  CHECK_MSG(after > before, "不确定态短条必须前进，否则用户以为卡死了");
}

TEST(progress_bar_indeterminate_reset_starts_at_left) {
  ProgressBar view;
  view.setIndeterminate(true);
  view.dispatchTimer();
  view.dispatchTimer();
  // 重新进入不确定态应从头开始：否则会接着上一次的位置，看起来像跳帧
  view.setIndeterminate(false);
  view.setIndeterminate(true);

  const auto commands = recordDirect(view, Size{200, 40});
  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kFillRoundRect && command.color == defaultTheme().trackFill) {
      CHECK_EQ(command.rect.left, kTrack.left);
      return;
    }
  }
  FAIL_AT("不确定态没有画短条");
}

TEST(progress_bar_determinate_ignores_timer) {
  ProgressBar view;
  view.setProgress(0.5f);
  const auto before = recordDirect(view, Size{200, 40});
  view.dispatchTimer();
  view.dispatchTimer();
  const auto after = recordDirect(view, Size{200, 40});
  // 确定态的帧不该因定时器而变：那条短条只属于不确定态
  CHECK_DRAW(after, before);
}

TEST(progress_bar_percent_text_is_right_aligned_in_its_own_column) {
  ProgressBar view;
  view.setProgress(0.42f);
  view.setShowPercent(true);
  const auto commands = recordDirect(view, Size{200, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;

  // 文字独占右侧 56px 一列，与轨道左右分列：把百分比压在轨道中央会让
  // 填充色与文字抢位置
  CHECK_EQ(text->text, std::string("42%"));
  CHECK_EQ(text->rect, (Rect{144, 12, 200, 28}));
  CHECK_EQ(text->intValue, static_cast<int>(TextAlign::kRight));
  CHECK_EQ(text->color, defaultTheme().textMuted);

  // 轨道相应缩短，且与文字之间留 6px 空隙
  const DrawCommand* track = firstOf(commands, DrawCommand::Kind::kFillRoundRect);
  CHECK(track != nullptr);
  if (track != nullptr) CHECK_EQ(track->rect.right, 144 - 6);
}

TEST(progress_bar_explicit_label_overrides_percent) {
  ProgressBar view;
  view.setProgress(0.42f);
  view.setShowPercent(true);
  view.setLabel("Copying");
  const auto commands = recordDirect(view, Size{200, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  // 显式标签优先：调用方写了自己想要的文字，就别再补百分比
  if (text != nullptr) CHECK_EQ(text->text, std::string("Copying"));
}

TEST(progress_bar_percent_rounds_to_nearest) {
  ProgressBar view;
  view.setShowPercent(true);

  // 0.425 * 100 = 42.5 → 43（四舍五入，不是截断）
  view.setProgress(0.425f);
  auto commands = recordDirect(view, Size{200, 40});
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->text, std::string("43%"));

  view.setProgress(0.994f);
  commands = recordDirect(view, Size{200, 40});
  text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->text, std::string("99%"));

  view.setProgress(1.0f);
  commands = recordDirect(view, Size{200, 40});
  text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) CHECK_EQ(text->text, std::string("100%"));
}

TEST(progress_bar_text_column_is_capped_on_narrow_views) {
  ProgressBar view;
  view.setShowPercent(true);
  const auto commands = recordDirect(view, Size{60, 40});

  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;
  // 窄视图上文字列取一半宽（30px），否则轨道会被挤没
  CHECK_EQ(text->rect.left, 60 - 30);
}

TEST(progress_bar_track_has_rounded_ends) {
  ProgressBar view;
  const Surface surface = renderDirect(view, Size{200, 20});

  // 半径 = 厚度一半：轨道两端应是半圆，角落透明
  CHECK(isTransparent(surface, 0, 7));
  CHECK(isTransparent(surface, 199, 7));
  // 上沿中点完全铺满
  CHECK(!isTransparent(surface, 100, 7));
}

TEST(progress_bar_fill_is_accent_and_track_is_grey) {
  ProgressBar view;
  view.setProgress(0.5f);
  const Surface surface = renderDirect(view, Size{200, 40});

  // 渐变顶行对应 rc.top（铁律 #5 的「含首末行」），所以取第一行而不是中线。
  // x 取填充区中部：右缘 100 是开区间，不在填充内。
  CHECK(sameRgb(surface.at(50, 17), fillTop()));
  // 轨道右端是灰色底
  CHECK(sameRgb(surface.at(195, 20), defaultTheme().trackBg));
}

TEST(progress_bar_measure_grows_for_text) {
  ProgressBar noText;
  CHECK_EQ(noText.measure(Size{0, 0}).height, kTrackH);

  ProgressBar withText;
  withText.setShowPercent(true);
  // 有文字时要让出整行高度，否则百分比会压在轨道上
  CHECK_EQ(withText.measure(Size{0, 0}).height, kLineHeight);
}

TEST(progress_bar_measure_defaults_to_160_wide) {
  ProgressBar view;
  CHECK_EQ(view.measure(Size{0, 0}).width, 160);
}

TEST(progress_bar_thickness_is_configurable) {
  ProgressBar view;
  view.setTrackThickness(12);
  const auto commands = recordDirect(view, Size{200, 40});
  const DrawCommand* track = firstOf(commands, DrawCommand::Kind::kFillRoundRect);
  CHECK(track != nullptr);
  if (track == nullptr) return;
  CHECK_EQ(track->rect.height(), 12);
  // 圆角跟着厚度走，否则粗轨道两端会被削成方角
  CHECK_EQ(track->radius, 6);
  // 加粗后仍垂直居中
  CHECK_EQ(track->rect.center().y, 20);
}

}  // namespace test
}  // namespace pyui
