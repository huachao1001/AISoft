// 核心四层与控件的单元测试
//
// 全部跑在假平台后端上：不建真窗口、不碰 GDI+，只验证几何、命中、
// 焦点、脏区、滚动条与适配器逻辑。
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/layouts/frame_layout.h"
#include "AiUI/layouts/linear_layout.h"
#include "AiUI/scroll_view.h"
#include "AiUI/view_group.h"
#include "AiUI/view_root.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/list_view.h"
#include "AiUI/widgets/tree_view.h"

#include <memory>
#include <vector>

namespace {

using namespace aiui_test;
using namespace AiUI;

// 每个用例一套独立的假后端与字体，析构顺序由成员/作用域保证
struct TestEnv {
  ScopedFakePlatform fake;
  TestEnv() { fonts.create(96); }
  ~TestEnv() { fonts.destroy(); }
};

// 固定尺寸的叶子视图，用来验证容器的摆放规则
class FixedView : public View {
 public:
  FixedView(int w, int h) : size_{w, h} {}

 protected:
  Size onMeasure(const Size& available) override {
    (void)available;
    return size_;
  }

 private:
  Size size_;
};

// 记录延时回调次数的叶子视图
class CountdownView : public View {
 public:
  int fired = 0;

 protected:
  void onDelayed() override { ++fired; }
};

// 造一个 ListView：固定 400x400 视口，N 行数据
std::unique_ptr<ListView> makeList(int rows) {
  auto list = std::make_unique<ListView>();
  ListStyle style;
  style.headHeight = 40;
  style.rowHeight = 44;
  list->setStyle(style);
  list->setColumns({ListColumn{L"名称", 300, ListAlign::kLeft, true}});

  auto adapter = std::make_shared<ListRowSource>();
  auto count = std::make_shared<int>(rows);
  adapter->setCount([count] { return *count; });
  adapter->setBinder([count](int row, ListRow& out) {
    (void)count;
    out.cells.resize(1);
    out.cells[0].text = L"row";
    out.tag = row;
  });
  list->setAdapter(adapter);
  list->layout(Rect{0, 0, 400, 400});
  return list;
}

MouseEvent mouseAt(int x, int y, MouseButton button = MouseButton::kNone) {
  MouseEvent e;
  e.position = Point{x, y};
  e.button = button;
  return e;
}

}  // namespace

// ───────────────────────── 几何 ─────────────────────────

AIUI_TEST(frame_layout_places_by_offset) {
  TestEnv env;
  FrameLayout frame;
  auto* child = new FixedView(0, 0);
  frame.addView(child, LayoutParams::at(10, 20, 100, 50));

  frame.layout(Rect{0, 0, 400, 300});

  AIUI_CHECK_EQ(child->bounds().left, 10);
  AIUI_CHECK_EQ(child->bounds().top, 20);
  AIUI_CHECK_EQ(child->bounds().width(), 100);
  AIUI_CHECK_EQ(child->bounds().height(), 50);
  // clientRect 恒为局部原点
  AIUI_CHECK_EQ(child->clientRect().left, 0);
  AIUI_CHECK_EQ(child->clientRect().width(), 100);
}

AIUI_TEST(absolute_bounds_accumulates_parents) {
  TestEnv env;
  FrameLayout outer;
  auto* inner = new FrameLayout();
  auto* leaf = new FixedView(20, 10);
  outer.addView(inner, LayoutParams::at(5, 7, 200, 200));
  inner->addView(leaf, LayoutParams::at(3, 4, 20, 10));

  outer.layout(Rect{0, 0, 400, 300});

  const Rect abs = leaf->absoluteBounds();
  AIUI_CHECK_EQ(abs.left, 8);
  AIUI_CHECK_EQ(abs.top, 11);
  const Point p = leaf->localToWindow(Point{1, 1});
  AIUI_CHECK_EQ(p.x, 9);
  AIUI_CHECK_EQ(p.y, 12);
  // 反向换算要能回原点
  const Point back = leaf->windowToLocal(Point{9, 12});
  AIUI_CHECK_EQ(back.x, 1);
  AIUI_CHECK_EQ(back.y, 1);
}

// ───────────────────────── 容器 ─────────────────────────

AIUI_TEST(linear_layout_weight_fills_rest) {
  TestEnv env;
  LinearLayout column(Orientation::kVertical);
  auto* header = new FixedView(0, 0);
  auto* body = new FixedView(0, 0);
  column.addView(header, LayoutParams{}.withSize(LayoutParams::kMatchParent, 50));
  column.addView(body, LayoutParams{}.withWeight(1.0f));

  column.layout(Rect{0, 0, 400, 300});

  AIUI_CHECK_EQ(header->bounds().height(), 50);
  AIUI_CHECK_EQ(header->bounds().width(), 400);
  AIUI_CHECK_EQ(body->bounds().top, 50);
  AIUI_CHECK_EQ(body->bounds().height(), 250);
  AIUI_CHECK_EQ(body->bounds().width(), 400);
}

AIUI_TEST(linear_layout_splits_weight_by_ratio) {
  TestEnv env;
  LinearLayout row(Orientation::kHorizontal);
  auto* first = new FixedView(0, 0);
  auto* second = new FixedView(0, 0);
  row.addView(first, LayoutParams{}.withSize(100, LayoutParams::kMatchParent));
  row.addView(second, LayoutParams{}.withWeight(1.0f));

  row.layout(Rect{0, 0, 400, 100});

  AIUI_CHECK_EQ(first->bounds().width(), 100);
  AIUI_CHECK_EQ(second->bounds().left, 100);
  AIUI_CHECK_EQ(second->bounds().width(), 300);
}

AIUI_TEST(viewgroup_rejects_second_parent) {
  TestEnv env;
  FrameLayout a;
  FrameLayout b;
  auto* child = new FixedView(10, 10);

  AIUI_CHECK(a.addView(child, LayoutParams::at(0, 0, 10, 10)) == child);
  // 视图只属于一个父容器，换父容器必须先 removeView
  AIUI_CHECK(b.addView(child, LayoutParams{}) == nullptr);
  AIUI_CHECK_EQ(a.childCount(), 1);
  AIUI_CHECK_EQ(b.childCount(), 0);
}

AIUI_TEST(viewgroup_remove_and_find) {
  TestEnv env;
  LinearLayout root(Orientation::kVertical);
  auto* button = new Button();
  auto* nested = new LinearLayout(Orientation::kHorizontal);
  auto* input = new InputBox();
  root.addView(button);
  root.addView(nested);
  nested->addView(input);

  // 深度优先按类型找后代
  AIUI_CHECK(root.find<InputBox>() == input);
  AIUI_CHECK(root.find<Button>() == button);
  AIUI_CHECK_EQ(root.indexOfChild(nested), 1);

  root.removeView(nested);
  AIUI_CHECK_EQ(root.childCount(), 1);
  AIUI_CHECK(root.find<InputBox>() == nullptr);
}

AIUI_TEST(hit_test_returns_topmost_child) {
  TestEnv env;
  FrameLayout frame;
  auto* below = new FixedView(0, 0);
  auto* above = new FixedView(0, 0);
  frame.addView(below, LayoutParams::at(0, 0, 100, 100));
  frame.addView(above, LayoutParams::at(0, 0, 100, 100));
  frame.layout(Rect{0, 0, 200, 200});

  // 后加的在上层
  AIUI_CHECK(frame.hitTest(Point{50, 50}) == above);
  // 落在空白处返回容器自己
  AIUI_CHECK(frame.hitTest(Point{150, 150}) == &frame);
}

// ───────────────────────── ViewRoot ─────────────────────────

AIUI_TEST(viewroot_single_window_and_invalidation) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* button = new Button();
  ButtonSpec spec;
  spec.label = L"刷新";
  spec.height = 40;
  button->setSpec(spec);
  root->addView(button, LayoutParams{}.withSize(LayoutParams::kMatchParent, 40));

  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  // 整棵树只有一个窗口，子视图不持有句柄
  AIUI_CHECK(root->handle() != nullptr);
  AIUI_CHECK(button->handle() == root->handle());

  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK(window != nullptr);

  window->clearInvalidRects();
  button->invalidate();
  const auto& rects = window->invalidRects();
  AIUI_CHECK_EQ(rects.size(), static_cast<size_t>(1));
  // 脏区是窗口坐标，等于按钮的绝对矩形
  AIUI_CHECK_EQ(rects[0].top, 0);
  AIUI_CHECK_EQ(rects[0].bottom, 40);
  AIUI_CHECK_EQ(rects[0].right, 400);
}

AIUI_TEST(viewroot_expose_draws_in_window_coordinates) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* button = new Button();
  ButtonSpec spec;
  spec.label = L"刷新";
  spec.height = 40;
  spec.style = ButtonStyle::kOutline;
  button->setSpec(spec);
  root->addView(button, LayoutParams{}.withSize(LayoutParams::kMatchParent, 40));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  // 第二个子视图下移 40，用来验证绘制平移
  auto* second = new Button();
  second->setSpec(spec);
  root->addView(second, LayoutParams{}.withSize(LayoutParams::kMatchParent, 40));

  ViewRoot* viewRoot = root->root();
  AIUI_CHECK(viewRoot != nullptr);
  viewRoot->onExpose(Rect{0, 0, 400, 300});

  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK_EQ(window->lastCommits(), 1);

  // 第二个按钮的背景填充必须落在 40..80 这段
  bool found = false;
  for (const FillCall& fill : window->lastFills()) {
    if (fill.rect.top == 40 && fill.rect.bottom == 80) found = true;
  }
  AIUI_CHECK(found);
  // 画完必须回到原点，不能把平移漏给下一次绘制
  AIUI_CHECK_EQ(window->translateXAfterDraw(), 0);
  AIUI_CHECK_EQ(window->translateYAfterDraw(), 0);
}

AIUI_TEST(viewroot_focus_moves_to_clicked_view) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* input = new InputBox();
  auto* button = new Button();
  ButtonSpec spec;
  spec.label = L"确定";
  spec.height = 32;
  button->setSpec(spec);
  root->addView(input, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  root->addView(button, LayoutParams{}.withSize(LayoutParams::kMatchParent, 32));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(50, 10, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(50, 10, MouseButton::kLeft));
  AIUI_CHECK(viewRoot->focused() == input);
  AIUI_CHECK(input->isFocused());

  // 点不可聚焦的按钮要让输入框失焦
  viewRoot->onMouseDown(mouseAt(50, 45, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(50, 45, MouseButton::kLeft));
  AIUI_CHECK(viewRoot->focused() == nullptr);
  AIUI_CHECK(!input->isFocused());
}

AIUI_TEST(button_click_ends_even_when_released_outside) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* button = new Button();
  ButtonSpec spec;
  spec.label = L"确定";
  spec.height = 40;
  button->setSpec(spec);
  root->addView(button, LayoutParams{}.withSize(LayoutParams::kMatchParent, 40));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  int clicks = 0;
  button->setOnClick([&clicks] { ++clicks; });

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(50, 20, MouseButton::kLeft));
  // 拖到按钮外再松手：不算点击，但按下态必须清掉
  viewRoot->onMouseUp(mouseAt(50, 200, MouseButton::kLeft));
  AIUI_CHECK_EQ(clicks, 0);

  // 正常在按钮内松手才算一次
  viewRoot->onMouseDown(mouseAt(50, 20, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(50, 20, MouseButton::kLeft));
  AIUI_CHECK_EQ(clicks, 1);
}

// ───────────────────────── ScrollView ─────────────────────────

AIUI_TEST(scroll_view_clamps_to_content) {
  TestEnv env;
  auto list = makeList(100);
  // 视口 360，行高 44，内容 4400
  AIUI_CHECK_EQ(list->contentHeight(), 4400);
  AIUI_CHECK_EQ(list->viewportHeight(), 360);
  AIUI_CHECK_EQ(list->maxScroll(), 4040);

  list->setScrollOffset(99999);
  AIUI_CHECK_EQ(list->scrollOffset(), 4040);
  list->setScrollOffset(-5);
  AIUI_CHECK_EQ(list->scrollOffset(), 0);
  list->scrollBy(100);
  AIUI_CHECK_EQ(list->scrollOffset(), 100);
}

AIUI_TEST(scroll_view_hides_bar_when_content_fits) {
  TestEnv env;
  auto list = makeList(3);
  // 3 行只有 132 高，装得下就不该有滚动条
  AIUI_CHECK_EQ(list->maxScroll(), 0);
  AIUI_CHECK_EQ(list->scrollBarReserve(), 0);
}

AIUI_TEST(scroll_bar_starts_below_header) {
  TestEnv env;
  auto list = makeList(100);
  AIUI_CHECK(list->scrollBarReserve() > 0);
  // 表头 40，滚动条不能画到表头上
  AIUI_CHECK_EQ(list->scrollOffset(), 0);
}

AIUI_TEST(wheel_scrolls_three_rows) {
  TestEnv env;
  auto list = makeList(100);
  MouseEvent wheel;
  wheel.wheelDelta = -120;  // 向后滚一格
  AIUI_CHECK(list->dispatchMouseWheel(wheel));
  AIUI_CHECK_EQ(list->scrollOffset(), 44 * 3);
}

AIUI_TEST(scroll_bar_shows_on_hover_and_hides_after_delay) {
  TestEnv env;
  auto list = makeList(100);
  // 只有挂到窗口才有延时定时器：用 create 而不是 setContentView
  AIUI_CHECK(list->create(nullptr, Rect{0, 0, 400, 400}));
  ViewRoot* root = list->root();
  AIUI_CHECK(root != nullptr);

  // 初始不显示，滚动条不占位
  AIUI_CHECK(!list->scrollBarVisible());

  // 鼠标进入：立刻显示
  list->dispatchMouseMove(mouseAt(200, 200));
  AIUI_CHECK(list->scrollBarVisible());

  // 离开后立刻还在，等过 3 秒才消失
  list->dispatchMouseLeave();
  AIUI_CHECK(list->scrollBarVisible());
  env.fake.platform().advanceMillis(2900);
  root->onDelayTimer();
  AIUI_CHECK(list->scrollBarVisible());
  env.fake.platform().advanceMillis(200);
  root->onDelayTimer();
  AIUI_CHECK(!list->scrollBarVisible());
}

AIUI_TEST(delayed_timer_keeps_tasks_of_different_views) {
  TestEnv env;
  auto container = std::make_unique<FrameLayout>();
  auto* first = new CountdownView();
  auto* second = new CountdownView();
  container->addView(first);
  container->addView(second);
  AIUI_CHECK(container->create(nullptr, Rect{0, 0, 400, 400}));
  ViewRoot* root = container->root();
  AIUI_CHECK(root != nullptr);

  // 两个视图各自计时：先到期的不能被后到的覆盖
  root->postDelayed(first, 100);
  root->postDelayed(second, 500);

  env.fake.platform().advanceMillis(120);
  root->onDelayTimer();
  AIUI_CHECK_EQ(first->fired, 1);
  AIUI_CHECK_EQ(second->fired, 0);

  env.fake.platform().advanceMillis(400);
  root->onDelayTimer();
  AIUI_CHECK_EQ(first->fired, 1);
  AIUI_CHECK_EQ(second->fired, 1);

  // 取消后不再触发
  root->postDelayed(first, 100);
  root->postDelayed(first, 0);
  env.fake.platform().advanceMillis(300);
  root->onDelayTimer();
  AIUI_CHECK_EQ(first->fired, 1);
}

// ───────────────────────── ListView ─────────────────────────

AIUI_TEST(list_adapter_and_selection) {
  TestEnv env;
  auto list = makeList(10);
  AIUI_CHECK_EQ(list->rowCount(), 10);
  AIUI_CHECK_EQ(list->rowTag(3), 3);

  list->selectRow(3, false);
  const auto rows = list->selectedRows();
  AIUI_CHECK_EQ(rows.size(), static_cast<size_t>(1));
  AIUI_CHECK_EQ(rows[0], 3);
  AIUI_CHECK_EQ(list->selectedCount(), 1);

  list->selectAll();
  AIUI_CHECK_EQ(list->selectedCount(), 10);

  list->clearSelection();
  AIUI_CHECK_EQ(list->selectedCount(), 0);
}

AIUI_TEST(list_click_selects_row) {
  TestEnv env;
  auto list = makeList(10);
  // 表头 40，第 2 行在 40 + 2*44 处
  list->dispatchMouseDown(mouseAt(50, 40 + 2 * 44 + 10, MouseButton::kLeft));
  list->dispatchMouseUp(mouseAt(50, 40 + 2 * 44 + 10, MouseButton::kLeft));

  const auto rows = list->selectedRows();
  AIUI_CHECK_EQ(rows.size(), static_cast<size_t>(1));
  AIUI_CHECK_EQ(rows[0], 2);

  // 按住 ctrl 再点多选一行
  MouseEvent ctrl = mouseAt(50, 40 + 5 * 44 + 10, MouseButton::kLeft);
  ctrl.modifiers.ctrl = true;
  list->dispatchMouseDown(ctrl);
  list->dispatchMouseUp(ctrl);
  AIUI_CHECK_EQ(list->selectedCount(), 2);
}

AIUI_TEST(list_keyboard_navigates_rows) {
  TestEnv env;
  auto list = makeList(10);
  list->selectRow(0, false);

  AIUI_CHECK(list->dispatchKeyDown(Key::kDown, Modifiers{}));
  AIUI_CHECK_EQ(list->focusedRow(), 1);
  AIUI_CHECK(list->dispatchKeyDown(Key::kEnd, Modifiers{}));
  AIUI_CHECK_EQ(list->focusedRow(), 9);
  // 报头行不能越界
  AIUI_CHECK(list->dispatchKeyDown(Key::kDown, Modifiers{}));
  AIUI_CHECK_EQ(list->focusedRow(), 9);

  Modifiers ctrl;
  ctrl.ctrl = true;
  AIUI_CHECK(list->dispatchKeyDown(Key::kA, ctrl));
  AIUI_CHECK_EQ(list->selectedCount(), 10);
}

AIUI_TEST(list_notify_resets_selection) {
  TestEnv env;
  auto list = makeList(10);
  list->selectRow(4, false);
  AIUI_CHECK_EQ(list->selectedCount(), 1);

  list->notifyDataSetChanged();
  AIUI_CHECK_EQ(list->selectedCount(), 0);
  AIUI_CHECK_EQ(list->focusedRow(), -1);
}

AIUI_TEST(list_column_stretch_uses_spare_width) {
  TestEnv env;
  auto list = std::make_unique<ListView>();
  list->setColumns({ListColumn{L"名称", 200, ListAlign::kLeft, true},
                    ListColumn{L"大小", 100, ListAlign::kRight, false}});
  list->layout(Rect{0, 0, 400, 400});
  // columnWidth 报的是用户设定宽度，没拖过就是 0，供持久化使用
  AIUI_CHECK_EQ(list->columnWidth(0), 0);
  AIUI_CHECK_EQ(list->columnWidth(1), 0);

  list->setUserWidth(1, 150);
  AIUI_CHECK_EQ(list->columnWidth(1), 150);
  // 越界列号安全返回 0
  AIUI_CHECK_EQ(list->columnWidth(9), 0);
}

// ───────────────────────── TreeView ─────────────────────────

AIUI_TEST(tree_select_path_expands_and_activates) {
  TestEnv env;
  auto tree = std::make_unique<TreeView>();
  TreeStyle treeStyle;
  treeStyle.rowHeight = 30;
  tree->setStyle(treeStyle);

  auto root = std::make_unique<TreeNode>();
  root->depth = -1;
  root->loaded = true;
  root->expanded = true;

  auto first = std::make_unique<TreeNode>();
  first->label = L"a";
  first->path = L"a";
  first->icon = IconId::kFolder;
  root->children.push_back(std::move(first));

  tree->setRoot(std::move(root));
  // 懒加载：只有 a 有子节点，其余是叶子
  tree->setChildrenLoader([](TreeNode* node) {
    std::vector<std::unique_ptr<TreeNode>> out;
    if (node->path != L"a") return out;
    for (const wchar_t* name : {L"b", L"c"}) {
      auto child = std::make_unique<TreeNode>();
      child->label = name;
      child->path = node->path + L"\\" + name;
      out.push_back(std::move(child));
    }
    return out;
  });

  tree->layout(Rect{0, 0, 300, 400});
  tree->selectPath(L"a\\b");

  AIUI_CHECK(tree->active() != nullptr);
  AIUI_CHECK(tree->activePath() == L"a\\b");
  // 展开后三行都可见：a、a\b、a\c
  AIUI_CHECK_EQ(tree->contentHeight(), 3 * 30);
}

AIUI_TEST(tree_select_handler_not_fired_by_select_path) {
  TestEnv env;
  auto tree = std::make_unique<TreeView>();
  auto root = std::make_unique<TreeNode>();
  root->loaded = true;
  root->expanded = true;
  auto node = std::make_unique<TreeNode>();
  node->label = L"a";
  node->path = L"a";
  node->hasChildren = false;
  root->children.push_back(std::move(node));
  tree->setRoot(std::move(root));
  tree->layout(Rect{0, 0, 300, 400});

  int calls = 0;
  tree->setSelectHandler([&calls](TreeNode*) { ++calls; });
  // 程序化同步不应触发导航回调，避免回调成环
  tree->selectPath(L"a");
  AIUI_CHECK_EQ(calls, 0);
}

// ───────────────────────── InputBox ─────────────────────────

AIUI_TEST(input_box_typing_and_backspace) {
  TestEnv env;
  auto box = std::make_unique<InputBox>();
  box->layout(Rect{0, 0, 300, 34});

  box->dispatchChar(L'a');
  box->dispatchChar(L'b');
  box->dispatchChar(L'c');
  AIUI_CHECK(box->text() == L"abc");

  AIUI_CHECK(box->dispatchKeyDown(Key::kBackspace, Modifiers{}));
  AIUI_CHECK(box->text() == L"ab");

  // 换行、制表这类控制字符单行框直接吞掉
  AIUI_CHECK(box->dispatchChar(L'\n'));
  AIUI_CHECK(box->text() == L"ab");
}

AIUI_TEST(input_box_reports_ime_caret) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  root->root()->requestFocus(box);
  AIUI_CHECK(box->isFocused());
  FakeWindow* window = env.fake.platform().lastWindow();
  AIUI_CHECK(window->blink());

  box->setText(L"hello");
  // 光标位置随文本推进，IME 候选框要跟着走
  AIUI_CHECK(window->imeX() >= 0);
}
