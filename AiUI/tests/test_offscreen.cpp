// 离屏渲染与合成事件：不建可见窗口也要能出图、能驱动控件
//
// 这组用例守的是「自动化截图不打扰用户」这条约束：截图走同一个绘制入口，
// 但绝不触碰系统窗口、焦点与输入设备。
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/layouts/frame_layout.h"
#include "AiUI/view_root.h"

namespace {

using namespace aiui_test;
using namespace AiUI;

struct OffscreenEnv {
  ScopedFakePlatform scoped;
  OffscreenEnv() { fonts.create(96); }
  ~OffscreenEnv() { fonts.destroy(); }
  FakePlatform& platform() { return scoped.platform(); }
};

// 记录点击次数的按钮，用来确认合成事件真的送到了控件
struct ClickSpy : View {
  int clicks = 0;
  Size onMeasure(const Size& available) override { return Size{available.width, 40}; }
};

// 建一棵真正挂上的树：View::create 会建窗口并 attachTree，
// 悬停提示要求 owner 的 root() 指向同一个 ViewRoot。
struct Tree {
  Tree() {
    content = new FrameLayout();
    created = content->create(nullptr, Rect{0, 0, 400, 300});
    root = content->root();
  }
  ViewRoot* root = nullptr;
  FrameLayout* content = nullptr;
  bool created = false;
};

}  // namespace

// 显式尺寸要压过窗口客户区：快照的可复现性全靠这一点
AIUI_TEST(render_to_image_uses_given_size) {
  OffscreenEnv env;
  Tree tree;
  AIUI_CHECK(tree.created);
  auto* window = static_cast<FakeWindow*>(tree.root->platformWindow());
  AIUI_CHECK(window != nullptr);
  window->setBounds(Rect{0, 0, 400, 300});

  RgbaImage shot;
  AIUI_CHECK(tree.root->renderToImage(shot, Size{800, 600}));
  AIUI_CHECK(shot.width == 800);
  AIUI_CHECK(shot.height == 600);
  AIUI_CHECK(env.platform().offscreenArea().width() == 800);
  AIUI_CHECK(env.platform().offscreenArea().height() == 600);
  // 排版也跟着出图尺寸走，不会留着一个 400×300 的旧布局
  AIUI_CHECK(tree.content->bounds().width() == 800);
  AIUI_CHECK(tree.content->bounds().height() == 600);
}

// 不给尺寸就退回客户区：这条保证渲染路径在真实窗口里也能复用
AIUI_TEST(render_to_image_falls_back_to_client_rect) {
  OffscreenEnv env;
  Tree tree;
  AIUI_CHECK(tree.created);
  auto* window = static_cast<FakeWindow*>(tree.root->platformWindow());
  AIUI_CHECK(window != nullptr);
  window->setBounds(Rect{0, 0, 640, 480});

  RgbaImage shot;
  AIUI_CHECK(tree.root->renderToImage(shot));
  AIUI_CHECK(shot.width == 640);
  AIUI_CHECK(shot.height == 480);
}

// 没有内容就没有可画的东西，不能凑出一张空图
AIUI_TEST(render_to_image_without_content_fails) {
  OffscreenEnv env;
  ViewRoot root;
  AIUI_CHECK(root.attach(nullptr, WindowParams{}));

  RgbaImage shot;
  AIUI_CHECK(!root.renderToImage(shot, Size{100, 100}));
  AIUI_CHECK(env.platform().offscreenCalls() == 0);
}

// 合成事件必须真的命中控件，否则「点一下按钮截图」只是画个样子
AIUI_TEST(synthetic_click_reaches_the_target_view) {
  OffscreenEnv env;
  Tree tree;
  AIUI_CHECK(tree.created);

  auto* spy = new ClickSpy();
  spy->setOnMouseDown([spy](const MouseEvent& e) {
    (void)e;
    ++spy->clicks;
    return true;
  });
  tree.content->addView(spy, LayoutParams{}.withSize(100, 40));

  RgbaImage shot;
  AIUI_CHECK(tree.root->renderToImage(shot, Size{400, 300}));

  tree.root->sendMouseMove(Point{20, 20});
  tree.root->sendMouseDown(Point{20, 20});
  tree.root->sendMouseUp(Point{20, 20});
  AIUI_CHECK(spy->clicks == 1);

  // 点到空白处不该误伤
  tree.root->sendMouseDown(Point{380, 280});
  tree.root->sendMouseUp(Point{380, 280});
  AIUI_CHECK(spy->clicks == 1);
}

// 立即显示悬停提示：快照要的是稳态，不该等消息循环里的 600ms
AIUI_TEST(show_tooltip_for_skips_the_hover_delay) {
  OffscreenEnv env;
  Tree tree;
  AIUI_CHECK(tree.created);

  auto* owner = new View();
  owner->setTooltip(L"D:\\work\\项目");
  tree.content->addView(owner, LayoutParams{}.withSize(100, 40));

  RgbaImage shot;
  AIUI_CHECK(tree.root->renderToImage(shot, Size{400, 300}));

  AIUI_CHECK(!tree.root->tooltip().isVisible());
  tree.root->showTooltipFor(owner);
  AIUI_CHECK(tree.root->tooltip().isVisible());
  AIUI_CHECK(tree.root->tooltip().text() == L"D:\\work\\项目");

  // 空提示没有可显示的内容，不该弹出一个空白框
  auto* bare = new View();
  tree.content->addView(bare, LayoutParams{}.withSize(100, 40));
  AIUI_CHECK(tree.root->renderToImage(shot, Size{400, 300}));
  tree.root->showTooltipFor(bare);
  AIUI_CHECK(tree.root->tooltip().text() == L"D:\\work\\项目");
}
