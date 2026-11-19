// SidePanel 与 Tooltip：折叠落到终态、顶栏文案、提示框不越界
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/tooltip.h"
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
  // 宽度收敛到顶栏那条窄边，而不是 0：收完还留一个可点的展开按钮
  AIUI_CHECK(panel.layoutParams().width == PanelHeaderStyle{}.collapsedWidth);
  AIUI_CHECK(panel.header() != nullptr);
  AIUI_CHECK(panel.header()->isCollapsed());

  panel.setCollapsed(false);
  AIUI_CHECK(!panel.isCollapsed());
  AIUI_CHECK(panel.layoutParams().width == 208);
}

// 顶栏只放目录名，完整路径走悬停提示
AIUI_TEST(side_panel_header_carries_name_and_full_path) {
  PanelEnv env;
  SidePanel panel;
  panel.header()->setText(L"项目");
  panel.header()->setHint(L"D:\\work\\项目");

  AIUI_CHECK(panel.header()->text() == L"项目");
  AIUI_CHECK(panel.header()->tooltip() == L"D:\\work\\项目");
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
