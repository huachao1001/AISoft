// SidePanel 与 Tooltip：折叠落到终态、顶栏文案、提示框不越界
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
  // 宽度收敛到顶栏那条窄边，而不是 0：收完还留一个可点的展开按钮
  AIUI_CHECK(panel.layoutParams().width == PanelHeaderStyle{}.collapsedWidth);
  AIUI_CHECK(panel.header() != nullptr);
  AIUI_CHECK(panel.header()->isCollapsed());

  panel.setCollapsed(false);
  AIUI_CHECK(!panel.isCollapsed());
  AIUI_CHECK(panel.layoutParams().width == 208);
}

// 收缩条不承担文字：侧栏就这么窄，目录名放不下几个字，认目录交给状态栏。
// 这里只留一个动作，别再把标题塞回来。
AIUI_TEST(side_panel_header_carries_no_title) {
  PanelEnv env;
  SidePanel panel;
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  RecordingBackend backend;
  Painter painter(backend);
  panel.header()->draw(painter, panel.header()->clientRect());

  for (const TextCall& call : backend.texts()) {
    AIUI_CHECK(call.text.empty());
  }
}

// 顶栏落在底部，内容占它上方
AIUI_TEST(side_panel_header_sits_at_the_bottom) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);
  auto* body = new View();
  panel.setContent(body);
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  const int headerH = PanelHeaderStyle{}.height;
  AIUI_CHECK(panel.header()->bounds().bottom == 500);
  AIUI_CHECK(panel.header()->bounds().top == 500 - headerH);
  AIUI_CHECK(body->bounds().top == 0);
  AIUI_CHECK(body->bounds().bottom == panel.header()->bounds().top);
}

// 收成窄边后顶栏只剩一块：高度不超过宽度，按钮才落得进去
AIUI_TEST(side_panel_collapsed_header_stays_square) {
  PanelEnv env;
  SidePanel panel;
  panel.setExpandedWidth(208);
  AIUI_CHECK(panel.create(nullptr, Rect{0, 0, 208, 500}));

  panel.setCollapsed(true, false);
  // 宽度改的是布局参数，父容器下一轮排版才生效；这里手动跑一轮
  panel.layout(Rect{0, 0, panel.layoutParams().width, 500});
  const Rect header = panel.header()->bounds();
  AIUI_CHECK(header.width() == PanelHeaderStyle{}.collapsedWidth);
  AIUI_CHECK(header.height() <= header.width());
  AIUI_CHECK(header.bottom == 500);
  // 按钮要整个落在这一块里，否则收起来就点不到展开
  const Rect button = panel.header()->toggleRect();
  AIUI_CHECK(button.left >= 0 && button.right <= header.width());
  AIUI_CHECK(button.top >= 0 && button.bottom <= header.height());
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
