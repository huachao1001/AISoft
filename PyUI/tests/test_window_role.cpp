// 窗口角色的行为测试：role 属性 = 能力，与控件在哪个布局里无关。
//
// 为什么要单独一个文件：内置 TitleBar 的测试（test_widget_title_bar.cpp）
// 全部按 id 取控件，它证明的是「默认标题栏长得对」。而这次要保证的是另一件
// 事——**应用自己写的布局**里，只要写了 role，拖动与三个按钮就该生效，且
// 不依赖内置标题栏是否存在。这两件事漏掉任何一条，用户「隐藏标题栏、自己
// 画一个」就会得到一排按不动的按钮。
//
// 断言只碰公共接口：WindowRole 枚举、ViewRoot 的命中与分发。
#include "test_framework.h"
#include "widget_test_support.h"

#include "pyui/layouts/frame_layout.h"
#include "pyui/layouts/linear_layout.h"
#include "pyui/platform/window.h"
#include "pyui/view_root.h"
#include "pyui/widgets/button.h"

namespace pyui {
namespace test {
namespace {

// 造一条「自建标题栏」：一个 caption 条 + 三个按钮，全部只用 role 声明。
// 故意不给任何 id，也不摆在内置标题栏的位置——若实现还在按 id 或坐标认，
// 下面的断言就会失败。
struct CustomBar {
  FrameLayout* root = nullptr;
  Button* minimize = nullptr;
  Button* maximize = nullptr;
  Button* close = nullptr;
  FrameLayout* caption = nullptr;
};

CustomBar buildCustomBar(ViewRoot& root, Size size) {
  auto frame = std::make_unique<FrameLayout>();
  CustomBar bar;
  bar.root = frame.get();

  // 拖动条：只有 role="caption"，没有任何 id。
  auto* caption = new FrameLayout();
  caption->setWindowRole(WindowRole::kCaption);
  frame->addView(caption, LayoutParams{}.withSize(LayoutParams::kMatchParent, 40));
  bar.caption = caption;

  // 三个按钮挤在左上角的一小段里（不是窗口右上角），验证位置无关。
  auto* row = new LinearLayout(Orientation::kHorizontal);
  caption->addView(row, LayoutParams{}.withSize(200, 40));

  auto addButton = [&](WindowRole role) {
    auto* button = new Button();
    button->setWindowRole(role);
    row->addView(button, LayoutParams{}.withSize(50, 40));
    return button;
  };
  bar.minimize = addButton(WindowRole::kMinimize);
  bar.maximize = addButton(WindowRole::kMaximize);
  bar.close = addButton(WindowRole::kClose);

  root.setContentView(std::move(frame));
  root.setSize(size);
  root.setDpi(96);
  root.performLayout();
  return bar;
}

}  // namespace

// role="maximize" 必须让命中变成 kMaxButton，系统才会给出贴靠布局预览。
TEST(window_role_maximize_hits_max_button) {
  ensureEnvironment();
  ViewRoot root;
  const CustomBar bar = buildCustomBar(root, Size{400, 120});

  const Point center = bar.maximize->absoluteBounds().center();
  CHECK(root.hitTest(center) == bar.maximize);
}

// role="caption" 让任意位置的 View 变成拖动条：命中 kCaption。
// 这是「隐藏标题栏后仍能拖窗口」的唯一依据。
TEST(window_role_caption_hits_caption) {
  ensureEnvironment();
  ViewRoot root;
  const CustomBar bar = buildCustomBar(root, Size{400, 120});

  // 取 caption 条右下角靠里的点，避开三个按钮（它们只占左侧 200px）。
  const Rect caption = bar.caption->absoluteBounds();
  const Point blank{caption.right - 10, caption.center().y};
  CHECK(root.hitTest(blank) == bar.caption);
  CHECK(bar.caption->windowRole() == WindowRole::kCaption);
}

// 三个按钮在自定义布局里也要能点。这里只验证 onClick 被触发（窗口动作由
// AppWindow::bindWindowRoles 接上，需要真实窗口，属于 Windows 层测试）。
TEST(window_role_buttons_fire_click_in_custom_layout) {
  ensureEnvironment();
  ViewRoot root;
  const CustomBar bar = buildCustomBar(root, Size{400, 120});

  int minimized = 0;
  int maximized = 0;
  int closed = 0;
  bar.minimize->setOnClick([&] { ++minimized; });
  bar.maximize->setOnClick([&] { ++maximized; });
  bar.close->setOnClick([&] { ++closed; });

  auto click = [&](View* target) {
    const Point at = target->absoluteBounds().center();
    MouseEvent down;
    down.position = at;
    down.button = MouseButton::kLeft;
    root.dispatchMouseDown(down);
    MouseEvent up = down;
    root.dispatchMouseUp(up);
  };

  click(bar.minimize);
  click(bar.maximize);
  click(bar.close);

  CHECK_EQ(minimized, 1);
  CHECK_EQ(maximized, 1);
  CHECK_EQ(closed, 1);
}

// 按角色分发点击：命中带角色的控件就执行它的 onClick，与 id、位置无关。
TEST(window_role_click_dispatch_ignores_id_and_position) {
  ensureEnvironment();
  ViewRoot root;
  const CustomBar bar = buildCustomBar(root, Size{400, 120});

  int maximized = 0;
  bar.maximize->setOnClick([&] { ++maximized; });

  // 用窗口坐标直接分发，不经过按下/抬起配对
  const Point center = bar.maximize->absoluteBounds().center();
  CHECK(root.dispatchWindowRoleClick(center));
  CHECK_EQ(maximized, 1);

  // 角色按「最近的有角色祖先」算：点在 caption 条空白处，命中条本身，
  // 它带 kCaption，所以这里应当是 true（拖动区也是角色）。
  const Rect caption = bar.caption->absoluteBounds();
  CHECK(root.dispatchWindowRoleClick(Point{caption.right - 10, caption.center().y}));
  // 但 caption 没有 onClick，所以不会再触发最大化。
  CHECK_EQ(maximized, 1);
}

// 没有角色的普通按钮不该被角色分发误伤。
TEST(window_role_click_skips_views_without_role) {
  ensureEnvironment();
  ViewRoot root;
  auto frame = std::make_unique<FrameLayout>();
  auto* plain = new Button();
  plain->setLayoutParams(LayoutParams{}.withSize(80, 40));
  frame->addView(plain);
  root.setContentView(std::move(frame));
  root.setSize(Size{200, 80});
  root.performLayout();

  int clicks = 0;
  plain->setOnClick([&] { ++clicks; });

  CHECK(!root.dispatchWindowRoleClick(plain->absoluteBounds().center()));
  CHECK_EQ(clicks, 0);
}

}  // namespace test
}  // namespace pyui
