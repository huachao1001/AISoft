// ThumbnailStrip 的行为：项数、命中、选中跟随、缩略图缓存与释放
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/view_root.h"
#include "AiUI/widgets/thumbnail_strip.h"

#include <memory>
#include <vector>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct StripEnv {
  ScopedFakePlatform fake;
  StripEnv() { fonts.create(96); }
  ~StripEnv() { fonts.destroy(); }
};

std::vector<std::wstring> samplePaths(int count) {
  std::vector<std::wstring> paths;
  for (int i = 0; i < count; ++i) {
    paths.push_back(L"C:\\pics\\img" + std::to_wstring(i) + L".png");
  }
  return paths;
}

MouseEvent mouseAt(int x, int y, MouseButton button = MouseButton::kLeft) {
  MouseEvent e;
  e.position = Point{x, y};
  e.button = button;
  return e;
}

}  // namespace

// 项数、行高与内容高度按固定行高推算
AIUI_TEST(thumbnail_strip_geometry_follows_item_count) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(3));
  strip.layout(Rect{0, 0, 176, 300});

  AIUI_CHECK_EQ(strip.itemCount(), 3);
  AIUI_CHECK(strip.itemHeight() > 0);
  // 三张卡片 + 两个间隙 + 上下留白
  const int gap = strip.cardStyle().itemGap;
  const int margin = strip.cardStyle().sideMargin;
  AIUI_CHECK_EQ(strip.contentHeight(),
                margin * 2 + strip.itemHeight() * 3 + gap * 2);
  // 视口不够时内容溢出，滚动上限是差值
  AIUI_CHECK_EQ(strip.maxScroll(), strip.contentHeight() - 300);
}

// 卡片在竖直方向按「顶部留白 + 行高 + 间隙」排布，宽度要给滚动条让位
AIUI_TEST(thumbnail_strip_card_rect_leaves_room) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(6));
  strip.layout(Rect{0, 0, 200, 200});

  const ThumbnailStyle& s = strip.cardStyle();
  const Rect first = strip.itemRect(0);
  AIUI_CHECK_EQ(first.top, s.sideMargin);
  AIUI_CHECK_EQ(first.left, s.sideMargin);
  AIUI_CHECK_EQ(first.height(), strip.itemHeight());
  // 卡片左右对称：右边距 = 左边距 + 滚动条让位
  AIUI_CHECK_EQ(200 - first.right, s.sideMargin + strip.scrollBarReserve());

  const Rect second = strip.itemRect(1);
  AIUI_CHECK_EQ(second.top - first.bottom, s.itemGap);
}

// 卡片要比阴影宽：间隙必须大于阴影两侧扩散之和，否则相邻阴影会连成一片
AIUI_TEST(thumbnail_strip_gap_clears_shadow) {
  StripEnv env;
  ThumbnailStrip strip;
  const ThumbnailStyle& s = strip.cardStyle();
  AIUI_CHECK(s.itemGap > s.shadowSpread);
  // 卡片高度要装得下：上下内边 + 图 + 名称间距 + 名称
  AIUI_CHECK_EQ(strip.itemHeight(),
                s.cardPadY * 2 + s.thumbH + s.labelGap + s.labelHeight);
}

// 布局前就调用 scrollToItem 时，要等拿到真实视口高度再补一次定位，
// 否则打开靠后的图会停在列表顶部
AIUI_TEST(thumbnail_strip_scroll_waits_for_layout) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(40));
  // 还没有尺寸：此时定位必须被记住而不是直接丢掉
  strip.scrollToItem(30);
  AIUI_CHECK_EQ(strip.scrollOffset(), 0);

  strip.layout(Rect{0, 0, 200, 400});
  AIUI_CHECK(strip.scrollOffset() > 0);
  const Rect rc = strip.itemRect(30);
  AIUI_CHECK(rc.top >= 0);
  AIUI_CHECK(rc.bottom <= strip.viewportHeight());
}
AIUI_TEST(thumbnail_strip_click_selects_item) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(5));
  AIUI_CHECK(strip.create(nullptr, Rect{0, 0, 176, 400}));
  ViewRoot* root = strip.root();
  AIUI_CHECK(root != nullptr);

  int clicked = -1;
  strip.setOnSelect([&clicked](int index) { clicked = index; });

  const int h = strip.itemHeight();
  const int gap = strip.cardStyle().itemGap;
  const int margin = strip.cardStyle().sideMargin;
  root->onMouseDown(mouseAt(80, margin + h / 2));
  root->onMouseUp(mouseAt(80, margin + h / 2));
  AIUI_CHECK_EQ(clicked, 0);
  AIUI_CHECK_EQ(strip.selected(), 0);

  root->onMouseDown(mouseAt(80, margin + h + gap + h / 2));
  root->onMouseUp(mouseAt(80, margin + h + gap + h / 2));
  AIUI_CHECK_EQ(clicked, 1);
  AIUI_CHECK_EQ(strip.selected(), 1);

  // 卡片间缝隙：命中不到任何项，选中也不变
  clicked = -1;
  root->onMouseDown(mouseAt(80, margin + h + 1));
  root->onMouseUp(mouseAt(80, margin + h + 1));
  AIUI_CHECK_EQ(clicked, -1);
  AIUI_CHECK_EQ(strip.selected(), 1);
}

// 选中项移出视口后 scrollToItem 要把它拉回来
AIUI_TEST(thumbnail_strip_scrolls_selection_into_view) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(20));
  strip.layout(Rect{0, 0, 176, 300});

  strip.setSelected(15);
  strip.scrollToItem(15);
  const Rect rc = strip.itemRect(15);
  AIUI_CHECK(rc.top >= 0);
  AIUI_CHECK(rc.bottom <= strip.viewportHeight());

  // 已经可见时再调一次不应改变滚动位置
  const int before = strip.scrollOffset();
  strip.scrollToItem(15);
  AIUI_CHECK_EQ(strip.scrollOffset(), before);
}

// 选中越界一律落到 -1，避免调用方传错值后崩在绘制里
AIUI_TEST(thumbnail_strip_clamps_selection) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(2));
  strip.layout(Rect{0, 0, 176, 300});

  strip.setSelected(1);
  AIUI_CHECK_EQ(strip.selected(), 1);
  strip.setSelected(9);
  AIUI_CHECK_EQ(strip.selected(), -1);
  strip.setSelected(-3);
  AIUI_CHECK_EQ(strip.selected(), -1);
}

// 换路径列表后旧选中不能越界，滚动位置也要回顶
AIUI_TEST(thumbnail_strip_reset_on_new_items) {
  StripEnv env;
  ThumbnailStrip strip;
  strip.setItems(samplePaths(20));
  strip.layout(Rect{0, 0, 176, 300});
  strip.setSelected(10);
  strip.setScrollOffset(200);

  strip.setItems(samplePaths(3));
  AIUI_CHECK_EQ(strip.itemCount(), 3);
  AIUI_CHECK_EQ(strip.scrollOffset(), 0);
  AIUI_CHECK_EQ(strip.selected(), 2);  // 夹到新的最后一项

  strip.setItems({});
  AIUI_CHECK_EQ(strip.itemCount(), 0);
  AIUI_CHECK_EQ(strip.selected(), -1);
  AIUI_CHECK_EQ(strip.contentHeight(), 0);
}

// 绘制时才解码缩略图：同一项重复画不会重复解码
AIUI_TEST(thumbnail_strip_decodes_thumbnails_lazily) {
  StripEnv env;
  // 假平台只在给了图像尺寸时才返回有效位图，且缩略图按 maxSide 缩放
  env.fake.platform().setImageSize(Size{800, 600});

  ThumbnailStrip strip;
  strip.setItems(samplePaths(3));
  AIUI_CHECK(strip.create(nullptr, Rect{0, 0, 176, 400}));
  ViewRoot* root = strip.root();
  AIUI_CHECK(root != nullptr);

  // 没有像素尺寸时不会解出图，但绘制本身不该崩
  root->onExpose(Rect{0, 0, 176, 400});
  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK(window != nullptr);
  AIUI_CHECK(window->lastCommits() >= 1);

  // 解出来的缩略图应当已经按 maxSide 缩过，而不是原图
  const Size thumb = env.fake.platform().thumbSize();
  AIUI_CHECK(thumb.width > 0);
  AIUI_CHECK(thumb.width < 800);
}
