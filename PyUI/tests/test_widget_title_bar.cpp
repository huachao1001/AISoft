// TitleBar 的绘制测试（铁律三）
//
// 标题栏是唯一「平台差异只是数据」的控件：高度、按钮在哪一侧、配色、字号
// 全部来自 TitleBarAppearance，绘制代码只有一份。所以这里的断言分两半：
//
//   - 布局：按 appearance 摆出的矩形（Windows 按钮在右、macOS 在左且关闭
//     在最左），这是最容易错的部分——「反转顺序」会得到关、最大化、最小化；
//   - 外观：底色、底部分隔线、非激活时文字变淡。
//
// 平台桩（test_platform_stubs.cpp）给的是 Windows 基准值，测试再显式
// 覆盖 appearance，避免期望值跟着平台桩漂移。
#include "widget_test_support.h"

#include "pyui/widgets/title_bar.h"

namespace pyui {
namespace test {
namespace {

// Windows / 多数 Linux 桌面：按钮在右，46×32 热区，无圆角
TitleBarAppearance winAppearance() {
  TitleBarAppearance appearance;
  appearance.bandHeight = 32;
  appearance.buttonsOnSide = TitleButtonSide::kRight;
  appearance.buttonWidth = 46;
  appearance.buttonHeight = 32;
  appearance.roundButtons = false;
  appearance.contentInset = 10;
  appearance.iconTextGap = 8;
  appearance.iconSize = 11;     // 按钮字形
  appearance.appIconSize = 16;  // 应用图标
  appearance.background = makeColor(0xff, 0xff, 0xff);
  appearance.backgroundInactive = makeColor(0xfa, 0xfa, 0xfa);
  appearance.titleInk = makeColor(0x1a, 0x1a, 0x1a);
  appearance.titleInkInactive = makeColor(0x99, 0x99, 0x99);
  appearance.bottomLine = makeColor(0xe5, 0xe5, 0xe5);
  appearance.drawBottomLine = true;
  return appearance;
}

// macOS：红绿灯在左，圆点按钮是正方形，彼此间隔 8
TitleBarAppearance macAppearance() {
  TitleBarAppearance appearance;
  appearance.bandHeight = 28;
  appearance.buttonsOnSide = TitleButtonSide::kLeft;
  // 圆点按钮用 buttonHeight 作边长，buttonWidth 不参与——这里写成同一个值，
  // 免得读代码的人误以为两个数字各管一维。
  appearance.buttonWidth = 28;
  appearance.buttonHeight = 28;
  appearance.roundButtons = true;
  appearance.contentInset = 12;
  appearance.iconTextGap = 8;
  appearance.iconSize = 8;      // 圆点按钮里的字形
  appearance.appIconSize = 0;   // macOS 窗口一般不在标题栏放应用图标
  appearance.drawBottomLine = false;
  return appearance;
}

constexpr int kFontHeight = 12;

}  // namespace

// ── 布局 ──

TEST(title_bar_windows_buttons_sit_at_right_edge) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  bar.layout(Rect::fromSize(400, 32));

  auto* minimize = bar.findView<Button>(TitleBar::kIdMinimize);
  auto* maximize = bar.findView<Button>(TitleBar::kIdMaximize);
  auto* close = bar.findView<Button>(TitleBar::kIdClose);
  CHECK(minimize != nullptr);
  CHECK(maximize != nullptr);
  CHECK(close != nullptr);
  if (minimize == nullptr || maximize == nullptr || close == nullptr) return;

  // 按钮的 bounds() 相对按钮条，所以这里要换算到窗口坐标才有意义：
  // 断言的是「关闭在最右、三者连续」，与它们被装在哪一层容器无关。
  CHECK_EQ(minimize->absoluteBounds(), (Rect{262, 0, 308, 32}));
  CHECK_EQ(maximize->absoluteBounds(), (Rect{308, 0, 354, 32}));
  CHECK_EQ(close->absoluteBounds(), (Rect{354, 0, 400, 32}));
}

TEST(title_bar_macos_close_sits_leftmost) {
  TitleBar bar;
  bar.setAppearance(macAppearance());
  bar.layout(Rect::fromSize(400, 28));

  auto* minimize = bar.findView<Button>(TitleBar::kIdMinimize);
  auto* maximize = bar.findView<Button>(TitleBar::kIdMaximize);
  auto* close = bar.findView<Button>(TitleBar::kIdClose);
  CHECK(minimize != nullptr);
  CHECK(maximize != nullptr);
  CHECK(close != nullptr);
  if (minimize == nullptr || maximize == nullptr || close == nullptr) return;

  // 圆点按钮边长取 buttonHeight，间隔固定 8
  const int d = macAppearance().buttonHeight;
  const int step = d + 8;

  // 红绿灯：关闭在 0 号槽，最小化顺延到 1，最大化到 2。
  // 简单反转会得到「关闭、最大化、最小化」——这是这里最容易错的一处。
  CHECK_EQ(close->absoluteBounds().left, 0);
  CHECK_EQ(minimize->absoluteBounds().left, step);
  CHECK_EQ(maximize->absoluteBounds().left, 2 * step);
  CHECK_MSG(close->absoluteBounds().right <= minimize->absoluteBounds().left,
            "三个按钮不能重叠");
}

TEST(title_bar_round_button_uses_square_slot) {
  TitleBar bar;
  bar.setAppearance(macAppearance());
  bar.layout(Rect::fromSize(400, 28));

  auto* close = bar.findView<Button>(TitleBar::kIdClose);
  CHECK(close != nullptr);
  if (close == nullptr) return;
  const Rect rc = close->absoluteBounds();
  // 圆点按钮：宽度取高度（正方形），圆角取半径
  CHECK_EQ(rc.width(), rc.height());
  CHECK_EQ(close->spec().radius, rc.height() / 2);
}

TEST(title_bar_title_starts_after_icon) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  bar.layout(Rect::fromSize(400, 32));

  auto* icon = bar.findView(TitleBar::kIdIcon);
  auto* title = bar.findView(TitleBar::kIdTitle);
  CHECK(title != nullptr);
  if (icon == nullptr || title == nullptr) return;

  // 10 内边距 + 16 图标 + 8 间距 = 34；标题右界止于按钮条左侧再减 8
  CHECK_EQ(icon->absoluteBounds(), (Rect{10, 8, 26, 24}));
  CHECK_EQ(title->absoluteBounds().left, 34);
  CHECK_EQ(title->absoluteBounds().right, 262 - 8);
  CHECK_MSG(icon->absoluteBounds().right <= title->absoluteBounds().left,
            "图标与标题不能重叠");
}

TEST(title_bar_macos_title_starts_after_button_strip) {
  TitleBar bar;
  bar.setAppearance(macAppearance());
  bar.layout(Rect::fromSize(400, 28));

  auto* title = bar.findView(TitleBar::kIdTitle);
  auto* icon = bar.findView(TitleBar::kIdIcon);
  CHECK(title != nullptr);
  if (title == nullptr) return;

  // 按钮条 3*28 + 2*8 = 100，再留 8 间距 → 108
  CHECK_EQ(title->absoluteBounds().left, 108);
  // iconSize = 0：图标视图退化为空矩形，但仍占着 ID 便于宿主替换
  if (icon != nullptr) CHECK(icon->bounds().isEmpty());
}

TEST(title_bar_buttons_never_take_focus) {
  TitleBar bar;
  bar.layout(Rect::fromSize(400, 32));

  // Tab 在客户区控件之间走；落到关闭按钮上会误关窗口
  auto* close = bar.findView<Button>(TitleBar::kIdClose);
  CHECK(close != nullptr);
  if (close != nullptr) CHECK(!close->isFocusable());
}

TEST(title_bar_measure_uses_band_height) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  CHECK_EQ(bar.measure(Size{400, 400}).height, 32);
}

TEST(title_bar_visual_height_adds_host_area) {
  TitleBarAppearance appearance = winAppearance();
  appearance.visualHeight = 48;

  TitleBar bar;
  bar.setAppearance(appearance);
  CHECK_EQ(bar.measure(Size{400, 400}).height, 48);
  // 可拖动的 band 仍是 32：多出来的部分留给宿主叠工具栏底纹
  CHECK_EQ(appearance.effectiveVisualHeight(), 48);
}

// ── 外观 ──

TEST(title_bar_draws_appearance_background) {
  TitleBar bar;
  TitleBarAppearance appearance = winAppearance();
  appearance.background = makeColor(0xd0, 0xe0, 0xf0);
  bar.setAppearance(appearance);

  const Surface surface = renderDirect(bar, Size{400, 32});
  // 未换主题时用 appearance 的配色，这样默认看起来就像本平台原生窗口
  CHECK(sameRgb(surface.at(2, 2), makeColor(0xd0, 0xe0, 0xf0)));
}

TEST(title_bar_draws_bottom_line_only_below_band) {
  TitleBar bar;
  bar.setAppearance(winAppearance());

  // 高度等于 band：没有多余空间，不该画线
  const auto exact = recordDirect(bar, Size{400, 32});
  for (const DrawCommand& command : exact) {
    if (command.kind == DrawCommand::Kind::kFillRect && command.rect.top == 32) {
      FAIL_AT("高度正好等于 band 时不该画底部分隔线");
    }
  }

  // 视觉高度更大时，线画在 band 底部（y=32），不画在视觉底部
  TitleBarAppearance tall = winAppearance();
  tall.visualHeight = 48;
  bar.setAppearance(tall);
  const auto withLine = recordDirect(bar, Size{400, 48});

  const DrawCommand* line = nullptr;
  for (const DrawCommand& command : withLine) {
    if (command.kind == DrawCommand::Kind::kFillRect && command.rect.height() == 1 &&
        command.rect.width() == 400) {
      line = &command;
    }
  }
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK_EQ(line->rect, (Rect{0, 32, 400, 33}));
    CHECK_EQ(line->color, winAppearance().bottomLine);
  }
}

TEST(title_bar_bottom_line_can_be_disabled) {
  TitleBarAppearance appearance = winAppearance();
  appearance.visualHeight = 48;
  appearance.drawBottomLine = false;  // macOS 不画

  TitleBar bar;
  bar.setAppearance(appearance);
  const auto commands = recordDirect(bar, Size{400, 48});

  for (const DrawCommand& command : commands) {
    if (command.kind == DrawCommand::Kind::kFillRect && command.rect.top == 32) {
      FAIL_AT("drawBottomLine=false 时不应画分隔线");
    }
  }
}

TEST(title_bar_active_and_inactive_differ) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  bar.setTitle("Doc");

  const auto inkOf = [&](bool active) {
    bar.setActive(active);
    const auto commands = recordDirect(bar, Size{400, 32});
    const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
    return text != nullptr ? text->color : kNoColor;
  };

  const Color active = inkOf(true);
  const Color inactive = inkOf(false);

  CHECK_EQ(active, winAppearance().titleInk);
  CHECK_EQ(inactive, winAppearance().titleInkInactive);
  // 非激活时标题变淡：这是用户判断「哪个窗口接收键盘」的主要线索
  CHECK(colorRed(inactive) > colorRed(active));
}

TEST(title_bar_inactive_background_uses_its_own_color) {
  TitleBarAppearance appearance = winAppearance();
  appearance.backgroundInactive = makeColor(0xe0, 0xe0, 0xe0);

  TitleBar bar;
  bar.setAppearance(appearance);
  bar.setActive(false);

  const Surface surface = renderDirect(bar, Size{400, 32});
  CHECK(sameRgb(surface.at(2, 2), makeColor(0xe0, 0xe0, 0xe0)));
}

TEST(title_bar_custom_theme_overrides_appearance_colors) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  // 换了主题就必须整体跟随主题，否则深色主题下会出现一条刺白的标题栏
  bar.setTheme(&kDarkTheme);
  bar.layout(Rect::fromSize(400, 32));

  const Surface surface = renderDirect(bar, Size{400, 32});
  CHECK(sameRgb(surface.at(2, 2), kDarkTheme.titleBg));
}

TEST(title_bar_title_text_is_left_aligned_and_ellipsized) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  bar.setTitle("A very long document name that cannot possibly fit");

  const auto commands = recordDirect(bar, Size{400, 32});
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text == nullptr) return;

  CHECK_EQ(text->intValue, static_cast<int>(TextAlign::kLeft));
  CHECK_EQ(text->fontHeight, kFontHeight);
  // 放不下时尾部省略：中间省略会让窗口标题变得难认
  CHECK(text->text.size() < bar.title().size());
  CHECK(text->rect.right <= 400);
}

TEST(title_bar_header_height_is_band_not_visual) {
  TitleBarAppearance appearance = winAppearance();
  appearance.visualHeight = 48;

  TitleBar bar;
  bar.setAppearance(appearance);
  bar.setTitle("Doc");
  const auto commands = recordDirect(bar, Size{400, 48});

  // 标题行框必须落在 band 内（0..32），不能被拉到 48 高——否则文字
  // 会垂直居中到「band + 宿主区域」的中线上，看起来明显偏下
  const DrawCommand* text = firstOf(commands, DrawCommand::Kind::kText);
  CHECK(text != nullptr);
  if (text != nullptr) {
    CHECK(text->rect.top >= 0);
    CHECK(text->rect.bottom <= 32);
  }
}

// ── 交互 ──

TEST(title_bar_close_button_reports_click) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  int requests = 0;
  bar->setCloseInterceptor([&] {
    ++requests;
    return true;  // 还有未保存内容，阻止关闭
  });

  // 关闭按钮 {354,0,400,32} 的中心
  MouseEvent event;
  event.position = Point{377, 16};
  event.button = MouseButton::kLeft;
  harness.root().dispatchMouseDown(event);
  harness.root().dispatchMouseUp(event);

  CHECK_EQ(requests, 1);
}

TEST(title_bar_double_click_blank_uses_callback) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  int toggles = 0;
  bar->setOnDoubleClickBlank([&] { ++toggles; });

  MouseEvent event;
  event.position = Point{200, 16};
  event.button = MouseButton::kLeft;
  harness.root().dispatchDoubleClick(event);
  CHECK_EQ(toggles, 1);
}

TEST(title_bar_has_no_platform_handle) {
  TitleBar bar;
  // 铁律二：整棵树只有一个原生窗口，控件自身不带任何句柄
  CHECK(bar.parent() == nullptr);
  CHECK_EQ(bar.className(), std::string("TitleBar"));
}

TEST(title_bar_set_title_keeps_one_text_view) {
  TitleBar bar;
  bar.setTitle("one");
  bar.setTitle("two");
  // 重复设标题不该每次都往树里塞一个 TextView。
  //
  // 统计要递归：标题现在在 band 里（结构来自 titlebar.xml），不再是
  // TitleBar 的直接子视图。断言的是「整棵树里只有一个」，与层级无关。
  int textViews = 0;
  const std::function<void(View*)> visit = [&](View* view) {
    if (dynamic_cast<TextView*>(view) != nullptr) ++textViews;
    auto* group = dynamic_cast<ViewGroup*>(view);
    if (group == nullptr) return;
    for (View* child : group->children()) visit(child);
  };
  visit(&bar);
  CHECK_EQ(textViews, 1);
  CHECK_EQ(bar.title(), std::string("two"));
}

TEST(title_bar_buttons_are_ghost_so_background_shows_through) {
  TitleBar bar;
  bar.setAppearance(winAppearance());
  bar.layout(Rect::fromSize(400, 32));

  auto* minimize = bar.findView<Button>(TitleBar::kIdMinimize);
  CHECK(minimize != nullptr);
  if (minimize == nullptr) return;
  // 常态不铺底：按钮与标题栏融为一体，只在悬停时出现热区
  CHECK_EQ(minimize->spec().background, kNoColor);
  CHECK_EQ(minimize->spec().hoverBackground, winAppearance().buttonHover);
}

TEST(title_bar_close_hover_is_red) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  MouseEvent move;
  move.position = Point{377, 16};
  harness.root().dispatchMouseMove(move);

  auto* close = bar->findView<Button>(TitleBar::kIdClose);
  CHECK(close != nullptr);
  if (close == nullptr) return;
  CHECK(close->isHot());
  // 误点代价最高的按钮必须给出强提示：整块变红、字形转白
  CHECK_EQ(close->spec().hoverBackground, winAppearance().closeHover);
  CHECK_EQ(close->spec().hoverInk, winAppearance().closeInk);
}

TEST(title_bar_minimize_hover_is_neutral) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  MouseEvent move;
  move.position = Point{285, 16};
  harness.root().dispatchMouseMove(move);

  auto* minimize = bar->findView<Button>(TitleBar::kIdMinimize);
  CHECK(minimize != nullptr);
  if (minimize == nullptr) return;
  CHECK(minimize->isHot());
  // 最小化不该变红：只有关闭才有「危险」语义
  CHECK_EQ(minimize->spec().hoverBackground, winAppearance().buttonHover);
}

// 悬停必须真的改到像素，并且必须自己请求重绘。
//
// 这两条都漏过一次：hot_ 只是视图上的一个 bool，没有任何东西会因为它的
// 变化自动重画；而 ViewRoot 进入新目标时又没调 invalidate。结果是「移上去
// 什么也不发生，移开才恢复」——看起来像悬停坏了，实际是**只画了离开方向**。
TEST(title_bar_hover_repaints_the_button) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  const Rect button = bar->findView(TitleBar::kIdMinimize)->absoluteBounds();
  const Point inside{button.center().x, button.center().y};

  // 先画一遍无悬停的基线
  harness.root().clearDirty();
  const Surface before = harness.render();

  MouseEvent move;
  move.position = inside;
  harness.root().dispatchMouseMove(move);

  // 进入悬停必须产生脏区：没有脏区，屏幕后端就不会重画这一块，
  // 悬停底色永远出不来（实机上就是这么失效的）。
  Rect dirty;
  const bool hasDirty = harness.root().consumeDirtyRect(dirty);
  CHECK(hasDirty);
  if (hasDirty) {
    CHECK(dirty.contains(inside));
  }

  const Surface after = harness.render();
  // 取样点要避开字形：最小化图标正好画在按钮中心那条横线上，
  // 取中心会读到墨色而不是底色。
  const Point sample{button.left + 4, button.top + 4};
  const Color idle = before.at(sample.x, sample.y);
  const Color hot = after.at(sample.x, sample.y);

  // 按钮本身是透明的（kSubtle 常态不铺底），所以悬停底色必须真的不同
  CHECK_MSG(!sameRgb(idle, hot), "悬停后按钮底色应当改变");
  CHECK(sameRgb(hot, winAppearance().buttonHover));
}

// 内置标题栏：XML 里 band 声明了 role="caption"，而 TitleBar::hitTest 会把
// band 上的命中改判给 TitleBar 自己。若 TitleBar 自身没有角色，这份声明就
// 在改判那一步被丢掉——「拖动条由 XML 声明」也就名存实亡。
TEST(title_bar_declares_caption_role_so_xml_role_survives_hittest) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  CHECK(bar->windowRole() == WindowRole::kCaption);

  // 标题空白处：命中会被改判成 TitleBar 自身，角色必须仍是 caption。
  MouseEvent event;
  event.position = Point{200, 16};
  View* hit = harness.root().hitTest(event.position);
  CHECK(hit == bar);
  CHECK(hit->windowRole() == WindowRole::kCaption);
}

// 三个按钮仍然优先于容器角色：往上找角色不能越过按钮自己的 role。
TEST(title_bar_buttons_keep_their_own_role_over_caption) {
  RootHarness harness(Size{400, 32});
  TitleBar* bar = harness.add<TitleBar>();
  bar->setAppearance(winAppearance());
  harness.relayout();

  auto* maximize = bar->findView<Button>(TitleBar::kIdMaximize);
  CHECK(maximize != nullptr);
  if (maximize == nullptr) return;

  View* hit = harness.root().hitTest(maximize->absoluteBounds().center());
  CHECK(hit == maximize);
  CHECK(hit->windowRole() == WindowRole::kMaximize);
}

}  // namespace test
}  // namespace pyui
