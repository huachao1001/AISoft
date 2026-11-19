// SidePanel / PanelToggle / Tooltip：折叠落到终态、开关按钮方向、提示框不越界
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/tooltip.h"
#include "AiUI/view_root.h"
#include "AiUI/widgets/side_panel.h"

#include <string>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct PanelEnv {
  ScopedFakePlatform fake;
  PanelEnv() { fonts.create(96); }
  ~PanelEnv() { fonts.destroy(); }
};

const Rect kArea{0, 0, 800, 600};

}  // namespace

// 没有窗口时没有逐帧定时器，折叠必须直接落到终态而不是永远停在动画中
AIUI_TEST(side_panel_collapses_immediately_without_window) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);

  AIUI_CHECK(!panel.isCollapsed());
  AIUI_CHECK(panel.isAnimating() == false);

  panel.setCollapsed(true);
  AIUI_CHECK(panel.isCollapsed());
  AIUI_CHECK(panel.isAnimating() == false);
  // 收成 0 宽：侧栏不再保留任何「窄边」，开关按钮在状态栏里
  AIUI_CHECK(panel.layoutParams().width == 0);

  panel.setCollapsed(false);
  AIUI_CHECK(!panel.isCollapsed());
  AIUI_CHECK(panel.layoutParams().width == 208);
}

// 收起后宽度归零：整块让给看图区，而不是留一条要人继续认的窄边
AIUI_TEST(side_panel_collapsed_takes_no_width) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);
  auto* body = new View();
  panel.setContent(body);
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  panel.setCollapsed(true, false);
  panel.layout(Rect{0, 0, panel.layoutParams().width, 500});
  AIUI_CHECK(panel.layoutParams().width == 0);
  // 收完之后主体藏起来，不再参与绘制
  AIUI_CHECK(!body->isVisible());

  panel.setCollapsed(false, false);
  panel.layout(Rect{0, 0, panel.layoutParams().width, 500});
  AIUI_CHECK(body->isVisible());
}

// 内容填满整个侧栏：没有自带的顶栏或收缩条占高度
AIUI_TEST(side_panel_content_fills_the_column) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);
  auto* body = new View();
  panel.setContent(body);
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  AIUI_CHECK(body->bounds().top == 0);
  AIUI_CHECK(body->bounds().bottom == 500);
  AIUI_CHECK(body->bounds().width() == 208);
}

// 动画中途内容保持展开宽度：窄下来的是窗口，不是卡片被压扁
AIUI_TEST(side_panel_keeps_body_width_while_animating) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);
  auto* body = new View();
  panel.setContent(body);
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  panel.setCollapsed(true, true);
  // 动画已起步但没走完：仍应处于动画中
  AIUI_CHECK(panel.isAnimating());
  panel.layout(Rect{0, 0, panel.layoutParams().width, 500});
  AIUI_CHECK(body->bounds().width() == 208);
}

// 开关按钮只换箭头方向：展开时指左（收起），收起时指右（展开）
AIUI_TEST(panel_toggle_flips_its_arrow) {
  PanelEnv env;
  PanelToggle toggle;
  AIUI_CHECK(toggle.create(nullptr, Rect{0, 0, 30, 30}));

  AIUI_CHECK(!toggle.isCollapsed());
  toggle.setCollapsed(true);
  AIUI_CHECK(toggle.isCollapsed());
  toggle.setCollapsed(false);
  AIUI_CHECK(!toggle.isCollapsed());
}

// 开关按钮点击要回调一次，且只在按到按钮上才算
AIUI_TEST(panel_toggle_reports_click) {
  PanelEnv env;
  PanelToggle toggle;
  AIUI_CHECK(toggle.create(nullptr, Rect{0, 0, 30, 30}));

  int clicks = 0;
  toggle.setOnToggle([&clicks] { ++clicks; });

  // 走合成的窗口事件：按钮在整棵树里要能被正确命中
  ViewRoot* root = toggle.root();
  AIUI_CHECK(root != nullptr);
  root->sendMouseDown(Point{15, 15});
  root->sendMouseUp(Point{15, 15});
  AIUI_CHECK(clicks == 1);

  // 没按下就抬起，不应该算一次点击
  root->sendMouseUp(Point{15, 15});
  AIUI_CHECK(clicks == 1);
}

// 按钮是个小方块：图标居中，四边留白对称，放进状态栏不会比文字高太多
AIUI_TEST(panel_toggle_measures_as_a_square) {
  PanelEnv env;
  PanelToggle toggle;
  // 默认布局参数是撑满父容器，要量自然尺寸得先放开
  toggle.setLayoutParams(LayoutParams::wrap());
  const Size size = toggle.measure(Size{100, 36});
  AIUI_CHECK(size.width == size.height);
  AIUI_CHECK(size.height <= 36);

  AIUI_CHECK(toggle.create(nullptr, Rect{0, 0, size.width, size.height}));
  const Rect icon = toggle.iconRect();
  AIUI_CHECK(icon.left >= 0 && icon.right <= size.width);
  AIUI_CHECK(icon.top >= 0 && icon.bottom <= size.height);
  // 水平方向要居中
  AIUI_CHECK(icon.left == size.width - icon.right);
}

// 设了状态源就以它为准：别的路径改了侧栏，箭头也跟着掉头，
// 不必要求每条改动路径都记得手动同步
AIUI_TEST(panel_toggle_follows_collapsed_source) {
  PanelEnv env;
  SidePanel panel;
  PanelToggle toggle;
  toggle.setCollapsedSource([&panel] { return panel.isCollapsed(); });

  AIUI_CHECK(!toggle.isCollapsed());
  panel.setCollapsed(true, false);
  AIUI_CHECK(toggle.isCollapsed());
  // 手动设过也压不过真实状态
  toggle.setCollapsed(false);
  AIUI_CHECK(toggle.isCollapsed());
}

// 提示框优先贴在锚点上方，且两端都要留在客户区内
AIUI_TEST(tooltip_sits_above_anchor_inside_client) {
  PanelEnv env;
  Tooltip tip;
  tip.show(L"D:\\work\\项目");

  const Rect anchor{300, 200, 380, 220};
  const Rect box = tip.bounds(kArea, anchor, 96);
  AIUI_CHECK(!box.isEmpty());
  AIUI_CHECK(box.bottom <= anchor.top);
  AIUI_CHECK(box.left >= kArea.left);
  AIUI_CHECK(box.right <= kArea.right);
  AIUI_CHECK(box.top >= kArea.top);
}

// 上方放不下就翻到下方；靠右的锚点不能把提示推出窗口
AIUI_TEST(tooltip_flips_below_and_stays_inside) {
  PanelEnv env;
  Tooltip tip;
  tip.show(L"很长很长的完整路径用来把提示框撑宽，好验证右边界");

  const Rect topAnchor{700, 5, 780, 25};
  const Rect above = tip.bounds(kArea, topAnchor, 96);
  AIUI_CHECK(above.top >= topAnchor.bottom);
  AIUI_CHECK(above.right <= kArea.right);

  const Rect bottomAnchor{700, 560, 780, 580};
  const Rect below = tip.bounds(kArea, bottomAnchor, 96);
  AIUI_CHECK(below.top >= kArea.top);
  AIUI_CHECK(below.bottom <= kArea.bottom);
  AIUI_CHECK(below.right <= kArea.right);
}

// 空文本等于没有提示：不占位置也不画
AIUI_TEST(tooltip_hides_when_empty) {
  PanelEnv env;
  Tooltip tip;
  tip.show(std::wstring());
  AIUI_CHECK(!tip.isVisible());
  AIUI_CHECK(tip.bounds(kArea, Rect{100, 100, 200, 120}, 96).isEmpty());

  tip.show(L"有内容");
  AIUI_CHECK(tip.isVisible());
  tip.hide();
  AIUI_CHECK(!tip.isVisible());
}
