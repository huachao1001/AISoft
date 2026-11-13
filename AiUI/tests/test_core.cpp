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
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/password_box.h"
#include "AiUI/widgets/list_view.h"
#include "AiUI/widgets/option_view.h"
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

  // 把 protected 的取色接口暴露给测试
  Color bg() const { return backgroundColor(); }
  Color resolved() const { return resolvedBackground(); }

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

AIUI_TEST(linear_layout_weighted_child_margin_stays_inside) {
  // 表单行：标签 + 带 marginX 的权重输入框 + 按钮，右侧不能溢出
  TestEnv env;
  LinearLayout row(Orientation::kHorizontal);
  auto* label = new FixedView(0, 0);
  auto* input = new FixedView(0, 0);
  auto* button = new FixedView(0, 0);
  row.addView(label, LayoutParams{}.withSize(84, 30));
  row.addView(input, LayoutParams{}.withSize(LayoutParams::kMatchParent, 30).margin(10, 0).withWeight(1.0f));
  row.addView(button, LayoutParams{}.withSize(76, 30));

  row.layout(Rect{0, 0, 400, 30});

  AIUI_CHECK_EQ(label->bounds().left, 0);
  AIUI_CHECK_EQ(input->bounds().left, 94);
  AIUI_CHECK_EQ(input->bounds().width(), 220);
  AIUI_CHECK_EQ(button->bounds().left, 324);
  AIUI_CHECK_EQ(button->bounds().right, 400);
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

// 光标要落在末字之后固定距离，不能随文本变长而漂移。
// 逐前缀测量会把每段各自的排版留白累加进来，正是本用例要防的回归。
AIUI_TEST(input_box_caret_gap_stays_constant) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  const int charWidth = env.fake.platform().charWidth();

  auto caretRightOfShort = [&](const std::wstring& text) {
    box->setText(text);
    root->root()->onExpose(Rect{0, 0, 400, 300});
    int right = -1;
    for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
      // 光标是 1px 左右窄条，高度撑满文本行
      if (fill.rect.width() <= 2 && fill.rect.height() > 10) right = fill.rect.right;
    }
    return right;
  };

  const int shortText = caretRightOfShort(L"ab");
  const int longText = caretRightOfShort(L"abcdefghij");
  AIUI_CHECK(shortText > 0);
  AIUI_CHECK(longText > shortText);
  // 文本多了 8 个字，光标应恰好多推进 8 个字宽
  AIUI_CHECK(longText - shortText == charWidth * 8);
}

AIUI_TEST(input_box_caret_stays_inside_long_text) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  // 长路径是常态（"F:\...\sample.tar.gz" 早已超过 32 字符）。
  // 只要字符边界有一处算错，光标就会把文本整体推出可视区，看起来像输入框空了。
  box->setText(L"F:\\AISoft\\build_tmp\\a-rather-long-file-name-sample.tar.gz");
  root->root()->onExpose(Rect{0, 0, 400, 300});

  int caretRight = -1;
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretRight = fill.rect.right;
  }
  AIUI_CHECK(caretRight > 0);
  // 光标必须仍在输入框内；滚出边界说明偏移表被算坏了
  AIUI_CHECK(caretRight <= box->bounds().right);
}

AIUI_TEST(input_box_falls_back_when_glyph_bounds_are_broken) {
  TestEnv env;
  env.fake.platform().setBrokenGlyphBounds(true);
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  // 后端边界不可用时必须退回逐前缀测量，而不是把文本推出可视区
  box->setText(L"F:\\AISoft\\build_tmp\\a-rather-long-file-name-sample.tar.gz");
  root->root()->onExpose(Rect{0, 0, 400, 300});

  int caretRight = -1;
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretRight = fill.rect.right;
  }
  AIUI_CHECK(caretRight > 0);
  AIUI_CHECK(caretRight <= box->bounds().right);
}

// DrawString 按传入矩形排版，矩形比整串窄就会把尾部字形（如 ".gz"）吞掉。
// 可视范围另有 clipRect 负责，所以绘制矩形必须按整串推进给足。
AIUI_TEST(input_box_draw_rect_fits_whole_text) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 120, 34}));
  root->root()->requestFocus(box);

  // 文本明显宽于控件：靠滚动显示尾部，绘制矩形仍要够宽
  const std::wstring text = L"F:\\AISoft\\build_tmp\\a-rather-long-file-name.tar.gz";
  box->setText(text);
  root->root()->onExpose(Rect{0, 0, 120, 34});

  // 用平台自己的度量口径作为下限：宽度给不够时 DrawString 会吞掉尾部字形
  const AiUI::Font font = env.fake.platform().createFont(L"Microsoft YaHei UI", 10.0f, false, 96);
  const int textWidth = env.fake.platform().advanceText(text, font);

  bool found = false;
  for (const TextCall& call : env.fake.platform().lastWindow()->lastTexts()) {
    if (call.text != text) continue;
    found = true;
    // 必须走 drawTextRun：普通 drawText 会补 em/6 留白，光标就与字形错位
    AIUI_CHECK(call.run);
    AIUI_CHECK(call.rect.width() >= textWidth);
  }
  AIUI_CHECK(found);
}

// 长文本删短后滚动量必须归零，否则文本整体停在左侧之外，输入框看起来像空的。
// 这是「切换格式后后缀消失」的另一种成因：scrollX_ 只增不减。
AIUI_TEST(input_box_scroll_resets_when_text_shrinks) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 200, 300}));
  root->root()->requestFocus(box);

  const std::wstring longText = L"C:\\Temp\\a_very_long_file_name_for_scroll_test.tar.bz2";
  box->setText(longText);
  root->root()->onExpose(Rect{0, 0, 200, 300});

  // 换成放得下的短文本：滚过的那段必须收回
  const std::wstring shortText = L"ab.zip";
  box->setText(shortText);
  root->root()->onExpose(Rect{0, 0, 200, 300});

  int textLeft = -1;
  for (const TextCall& call : env.fake.platform().lastWindow()->lastTexts()) {
    if (call.text == shortText) textLeft = call.rect.left;
  }
  AIUI_CHECK(textLeft >= 0);
  // 文本左边界必须落在输入框内侧（未发生滚动）
  AIUI_CHECK(textLeft > 0);
  AIUI_CHECK(textLeft < box->bounds().center().x);
}

// setText 常在布局之前调用（onCreate 里设置初始值），那时 clientRect 为空，
// ensureCaretVisible 会直接返回。宽度变化后若不重新校正，scrollX_ 停在旧值，
// 长文本从开头绘制、尾部被裁掉，看着就像末尾几个字消失了。
AIUI_TEST(input_box_scrolls_when_width_shrinks) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));

  // 宽窗口下放入长文本：整串放得下，滚动量为 0
  const std::wstring text = L"F:\\AISoft\\build_tmp\\a-rather-long-file-name.tar.gz";
  box->setText(text);
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 600, 200}));
  root->root()->requestFocus(box);

  // 收窄到放不下整串：必须由 onLayout 重新校正滚动，
  // 否则光标留在原处跑到输入框外
  box->setBounds(Rect{0, 0, 120, 34});
  root->root()->onExpose(Rect{0, 0, 600, 200});

  int caretRight = -1;
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretRight = fill.rect.right;
  }
  AIUI_CHECK(caretRight > 0);
  AIUI_CHECK(caretRight <= box->bounds().right);
}

AIUI_TEST(input_box_draws_caret_when_empty) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  // 空输入框也必须画出插入点，否则用户看不出焦点落在哪
  root->root()->onExpose(Rect{0, 0, 400, 300});
  FakeWindow* window = env.fake.platform().lastWindow();
  bool caret = false;
  for (const FillCall& fill : window->lastFills()) {
    // 光标是 1px 窄条，高度撑满文本行
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) caret = true;
  }
  AIUI_CHECK(caret);
}

// 插入点必须与绘制同源：文本绘制的左边界（drawTextRun 的 rect.left）
// 就是插入点 0 的原点，两者差一个像素都会让光标整体压字或离字太远。
// 这条同时钉住「绘制必须走 drawTextRun 而不是会补留白的 drawText」。
AIUI_TEST(input_box_caret_aligned_with_text_origin) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  const std::wstring text = L"C:\\T\\a_really_long_file_name_for_testing.gz";
  box->setText(text);
  root->root()->onExpose(Rect{0, 0, 400, 300});

  FakeWindow* window = env.fake.platform().lastWindow();

  // 文本绘制的排版原点
  int textLeft = -1;
  for (const TextCall& call : window->lastTexts()) {
    if (call.text != text) continue;
    AIUI_CHECK(call.run);
    textLeft = call.rect.left;
  }
  AIUI_CHECK(textLeft > 0);

  // 光标移到开头：插入点必须落在排版原点上（光标条左缘 == 文本左缘）
  AIUI_CHECK(box->dispatchKeyDown(Key::kHome, Modifiers{}));
  root->root()->onExpose(Rect{0, 0, 400, 300});
  int caretLeft = -1;
  for (const FillCall& fill : window->lastFills()) {
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretLeft = fill.rect.left;
  }
  AIUI_CHECK(caretLeft == textLeft);
}

// 文本长度任意变化后，插入点都必须落在「文本绘制范围」之内。
// 这是输入框最基本的能力：光标永远可见、永远不落后于字形。
AIUI_TEST(input_box_caret_never_leaves_text_run) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  const std::vector<std::wstring> samples = {
      L"a",
      L"ab",
      L"a_really_long_file_name_for_testing.gz",
      L"C:\\Users\\chaohua\\AppData\\Local\\Temp\\opencode\\lt\\a_really_long_name.tar.bz2",
  };
  for (const std::wstring& text : samples) {
    box->setText(text);
    root->root()->onExpose(Rect{0, 0, 400, 300});
    FakeWindow* window = env.fake.platform().lastWindow();

    int textRight = -1;
    for (const TextCall& call : window->lastTexts()) {
      if (call.text != text) continue;
      textRight = call.rect.right;
    }
    AIUI_CHECK(textRight > 0);

    // 末尾插入点：光标必须在文本绘制范围之内，且不越出控件
    AIUI_CHECK(box->dispatchKeyDown(Key::kEnd, Modifiers{}));
    root->root()->onExpose(Rect{0, 0, 400, 300});
    int caretRight = -1;
    for (const FillCall& fill : window->lastFills()) {
      if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretRight = fill.rect.right;
    }
    AIUI_CHECK(caretRight > 0);
    AIUI_CHECK(caretRight <= box->bounds().right);
    AIUI_CHECK(caretRight <= textRight);
  }
}

// 点击命中与光标位置必须互为逆运算：点在第 i 个字上，光标就落在第 i 个插入点；
// 再把光标画出来，必须落在点击处附近。两条路径若各用一套度量，输入框就会出现
// 「点这里、光标跳到别处」——用户感觉是输入框不可靠。
AIUI_TEST(input_box_click_and_caret_are_inverse) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  const std::wstring text = L"a_really_long_file_name_for_testing.gz";
  box->setText(text);
  root->root()->onExpose(Rect{0, 0, 400, 300});

  const int textLeft = 11;  // InputSpec::padX 默认值（96 DPI 下不缩放）
  const int charWidth = env.fake.platform().charWidth();

  // 逐个字符位置点一次，光标必须回到同一下标：命中与绘制共用偏移表。
  // 点在字符右侧 3/4 处：与左右两个插入点距离不等，命中结果唯一。
  const int n = static_cast<int>(text.size());
  for (int i = 0; i < n; ++i) {
    const int clickX = textLeft + i * charWidth + charWidth * 3 / 4;
    AIUI_CHECK(box->dispatchMouseDown(mouseAt(clickX, box->bounds().center().y, MouseButton::kLeft)));
    box->dispatchMouseUp(mouseAt(clickX, box->bounds().center().y, MouseButton::kLeft));
    root->root()->onExpose(Rect{0, 0, 400, 300});

    int caretRight = -1;
    for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
      if (fill.rect.width() <= 2 && fill.rect.height() > 10) caretRight = fill.rect.right;
    }
    // 点在第 i 个字符右半侧，光标落在该字符右缘。
    // 光标条宽 1px，所以 rect.right = 插入点坐标 + 1。
    AIUI_CHECK(caretRight == textLeft + (i + 1) * charWidth + 1);
  }

  // 点在文本之后（右侧留白里）：光标必须吸附到末位，而不是越界
  AIUI_CHECK(box->dispatchMouseDown(
      mouseAt(textLeft + (n + 5) * charWidth, box->bounds().center().y, MouseButton::kLeft)));
  box->dispatchMouseUp(
      mouseAt(textLeft + (n + 5) * charWidth, box->bounds().center().y, MouseButton::kLeft));
  root->root()->onExpose(Rect{0, 0, 400, 300});
  int endCaret = -1;
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.rect.width() <= 2 && fill.rect.height() > 10) endCaret = fill.rect.right;
  }
  AIUI_CHECK(endCaret == textLeft + n * charWidth + 1);
}

// 光标本体必须恒为 1 物理像素：GDI+ 的字符单元首尾相接，字间空隙常常只有
// 1~2 像素，2px 的竖线会把空隙填满、和相邻笔画粘成一根粗条，看着就像压字。
AIUI_TEST(input_box_caret_bar_is_hairline) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  box->setText(L"abcdef");
  root->root()->onExpose(Rect{0, 0, 400, 300});

  // 文本行高 > 10 而宽度为 1 的填充只有光标杆；背景/边框/选区都更宽
  int caretWidth = -1;
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.rect.width() == 1 && fill.rect.height() > 10) caretWidth = fill.rect.width();
  }
  AIUI_CHECK(caretWidth == 1);}

// ───────────────────────── 失焦与背景 ─────────────────────────

// 描边色必须跟着底色走：浅底叠淡墨、深底叠淡白。
// 写死灰色在深色主题上会糊掉，这条用来钉住派生规则。
AIUI_TEST(edge_color_adapts_to_background) {
  // 白底：得到比白略深的灰
  const Color onWhite = edgeOn(makeColor(0xff, 0xff, 0xff), 17);
  AIUI_CHECK(colorRed(onWhite) < 0xff);
  AIUI_CHECK(colorRed(onWhite) > 0xc0);
  // 纯灰：三通道一致，说明是等量叠墨而不是偏色
  AIUI_CHECK(colorRed(onWhite) == colorGreen(onWhite));
  AIUI_CHECK(colorGreen(onWhite) == colorBlue(onWhite));

  // 深底：要变亮而不是变暗，否则深色主题上完全看不见
  const Color dark = makeColor(0x1e, 0x1e, 0x1e);
  const Color onDark = edgeOn(dark, 17);
  AIUI_CHECK(colorRed(onDark) > colorRed(dark));
  // 也不能一路冲到纯白，否则深底上是一根刺眼的亮线
  AIUI_CHECK(colorRed(onDark) < 0xff);

  // 浓度越高，离底色越远
  const Color weak = edgeOn(kColorPane, 10);
  const Color strong = edgeOn(kColorPane, 30);
  AIUI_CHECK(colorRed(strong) < colorRed(weak));
}

// blendColor 的端点与中值
AIUI_TEST(blend_color_endpoints_and_middle) {
  const Color white = makeColor(0xff, 0xff, 0xff);
  const Color black = makeColor(0x00, 0x00, 0x00);
  AIUI_CHECK(blendColor(white, black, 0.0f) == white);
  AIUI_CHECK(blendColor(white, black, 1.0f) == black);
  // 50% 应是中灰
  const Color mid = blendColor(white, black, 0.5f);
  AIUI_CHECK_EQ(static_cast<int>(colorRed(mid)), 128);
}

// ───────────────────────── 主题 ─────────────────────────

// 主题沿父链继承：根上设一次，后代不用各自设置
AIUI_TEST(theme_inherits_down_the_view_tree) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* mid = new LinearLayout(Orientation::kVertical);
  auto* leaf = new FixedView(10, 10);
  mid->addView(leaf, LayoutParams{});
  root->addView(mid, LayoutParams{});

  // 没设主题时落到默认浅色
  AIUI_CHECK(leaf->theme().body == defaultTheme().body);

  root->setTheme(&kDarkTheme);
  AIUI_CHECK(leaf->theme().body == kDarkTheme.body);
  AIUI_CHECK(mid->theme().body == kDarkTheme.body);

  // 子树可以单独覆盖，不影响兄弟节点
  auto* sibling = new FixedView(10, 10);
  root->addView(sibling, LayoutParams{});
  mid->setTheme(&kLightTheme);
  AIUI_CHECK(leaf->theme().body == kLightTheme.body);
  AIUI_CHECK(sibling->theme().body == kDarkTheme.body);

  // 传 nullptr 恢复继承
  mid->setTheme(nullptr);
  AIUI_CHECK(leaf->theme().body == kDarkTheme.body);
}

// 按主题角色铺底：换主题后底色自动跟着变，宿主不用重新下发
AIUI_TEST(background_role_follows_theme_switch) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* panel = new FixedView(10, 10);
  panel->setBackgroundRole(&Theme::dialogBg);
  root->addView(panel, LayoutParams{});

  root->setTheme(&kLightTheme);
  AIUI_CHECK(panel->bg() == kLightTheme.dialogBg);
  AIUI_CHECK(panel->resolved() == kLightTheme.dialogBg);

  root->setTheme(&kDarkTheme);
  AIUI_CHECK(panel->bg() == kDarkTheme.dialogBg);
  // 深色与浅色必须真的不同，否则这条测试等于没测
  AIUI_CHECK(kDarkTheme.dialogBg != kLightTheme.dialogBg);

  // 显式设色会取消角色，避免两套来源互相覆盖
  const Color fixed = makeColor(0x12, 0x34, 0x56);
  panel->setBackgroundColor(fixed);
  AIUI_CHECK(panel->bg() == fixed);
  root->setTheme(&kLightTheme);
  AIUI_CHECK(panel->bg() == fixed);
}


// 切到别的软件时窗口失活，插入点必须收起来：
// 只停定时器会让光标停在最后一次可见状态，看起来像卡住了。
AIUI_TEST(input_box_hides_caret_when_window_deactivates) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);
  box->setText(L"hello");

  auto caretDrawn = [&] {
    root->root()->onExpose(Rect{0, 0, 400, 300});
    for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
      if (fill.rect.width() <= 2 && fill.rect.height() > 10) return true;
    }
    return false;
  };

  AIUI_CHECK(caretDrawn());

  // 窗口失去前台焦点：光标收起，但逻辑焦点保留
  root->root()->onFocusChanged(false);
  AIUI_CHECK(!caretDrawn());
  AIUI_CHECK(box->isFocused());

  // 回到前台：光标恢复
  root->root()->onFocusChanged(true);
  AIUI_CHECK(caretDrawn());
}

// 输入框圆角外不能有实色底：一旦上报背景色，View 会先铺满矩形，
// 四个角就变成白方块，放在非白底色上会露出明显的白角。
AIUI_TEST(input_box_leaves_rounded_corners_transparent) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new InputBox();
  root->addView(box, LayoutParams{}.withSize(LayoutParams::kMatchParent, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->onExpose(Rect{0, 0, 400, 300});

  // 圆角范围只该出现圆角填充，不该先有一块铺满整个控件的直角矩形
  for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
    if (fill.radius == 0 && fill.rect.left == box->bounds().left &&
        fill.rect.top == box->bounds().top &&
        fill.rect.width() == box->bounds().width() &&
        fill.rect.height() == box->bounds().height()) {
      AIUI_CHECK(false);
    }
  }
}

// ───────────────────────── 横向滚动 ─────────────────────────

AIUI_TEST(scroll_view_vertical_and_horizontal_are_independent) {
  TestEnv env;
  auto list = makeList(100);
  list->layout(Rect{0, 0, 400, 400});

  // 默认两个方向都开启
  AIUI_CHECK(list->isVerticalScrollEnabled());
  AIUI_CHECK(list->isHorizontalScrollEnabled());
  AIUI_CHECK(list->isScrollable());
  // ListView 只有纵向内容，横向不该溢出
  AIUI_CHECK(!list->isScrollableX());

  // 关掉纵向：既不滚也不画条
  list->setScrollOffset(500);
  AIUI_CHECK_EQ(list->scrollOffset(), 500);
  list->setVerticalScrollEnabled(false);
  AIUI_CHECK_EQ(list->scrollOffset(), 0);
  AIUI_CHECK(!list->isScrollable());
  AIUI_CHECK_EQ(list->scrollBarReserve(), 0);

  // 重新开启后回到可滚动
  list->setVerticalScrollEnabled(true);
  AIUI_CHECK(list->isScrollable());
  AIUI_CHECK(list->scrollBarReserve() > 0);
}

AIUI_TEST(tree_view_scrolls_horizontally_for_long_labels) {
  TestEnv env;
  auto tree = std::make_unique<TreeView>();
  auto root = std::make_unique<TreeNode>();
  root->loaded = true;
  root->expanded = true;
  auto node = std::make_unique<TreeNode>();
  // 足够长的标签，必须横向溢出一段可滚动的距离
  node->label = std::wstring(120, L'W');
  node->path = L"a";
  node->hasChildren = false;
  root->children.push_back(std::move(node));
  tree->setRoot(std::move(root));
  tree->layout(Rect{0, 0, 200, 400});

  AIUI_CHECK(tree->contentWidth() > tree->viewportWidth());
  AIUI_CHECK(tree->isScrollableX());
  AIUI_CHECK_EQ(tree->scrollOffsetX(), 0);

  // 横向滚动会夹紧到最大偏移
  tree->scrollByX(10000);
  AIUI_CHECK_EQ(tree->scrollOffsetX(), tree->maxScrollX());
  tree->scrollByX(-10000);
  AIUI_CHECK_EQ(tree->scrollOffsetX(), 0);

  // Shift + 滚轮滚横向
  tree->scrollByX(0);
  MouseEvent wheel;
  wheel.wheelDelta = -120;
  wheel.modifiers.shift = true;
  AIUI_CHECK(tree->dispatchMouseWheel(wheel));
  AIUI_CHECK(tree->scrollOffsetX() > 0);
  AIUI_CHECK_EQ(tree->scrollOffset(), 0);
}

AIUI_TEST(scroll_bar_thickens_only_on_thumb) {
  TestEnv env;
  auto list = makeList(100);
  AIUI_CHECK(list->create(nullptr, Rect{0, 0, 400, 400}));
  ViewRoot* root = list->root();
  AIUI_CHECK(root != nullptr);

  // 进入视图后滚动条出现
  list->dispatchMouseMove(mouseAt(200, 200));
  AIUI_CHECK(list->scrollBarVisible());

  // 停在内容区：滑块保持细的
  AIUI_CHECK_EQ(list->vThumbRect().width(), list->px(6));

  // 停在轨道空白（底部远离滑块）：仍然是细的，不该整条变粗
  const Rect track = list->vTrackRect();
  list->dispatchMouseMove(mouseAt(track.center().x, track.bottom - 4));
  AIUI_CHECK_EQ(list->vThumbRect().width(), list->px(6));

  // 只有停在滑块上才加粗，且只粗一点点
  const Rect thumb = list->vThumbRect();
  list->dispatchMouseMove(mouseAt(track.center().x, thumb.top + 2));
  AIUI_CHECK_EQ(list->vThumbRect().width(), list->px(9));
}

// ───────────────────────── id 与查找 ─────────────────────────

AIUI_TEST(make_id_is_stable_and_never_zero) {
  // 同一字符串必须每次算出同一个值：布局生成代码与宿主代码各自算常量
  AIUI_CHECK_EQ(makeId("tree"), makeId("tree"));
  // 不同字符串要能区分
  AIUI_CHECK(makeId("tree") != makeId("list"));
  // 0 保留给"没有 id"，空串也不能撞上
  AIUI_CHECK(makeId("") != 0);
  // 单个字符的边界
  AIUI_CHECK(makeId("a") != 0);
  AIUI_CHECK(makeId("a") != makeId("b"));
}

AIUI_TEST(find_view_by_string_id) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);

  auto* first = new TreeView();
  first->setId(makeId("tree"));
  root->addView(first, LayoutParams{}.withSize(100, 100));

  auto* second = new ListView();
  second->setId(makeId("list"));
  root->addView(second, LayoutParams{}.withSize(100, 100));

  // 按字符串找，且能拿到正确类型
  AIUI_CHECK(root->findView<TreeView>("tree") == first);
  AIUI_CHECK(root->findView<ListView>("list") == second);
  // 类型不符时返回 nullptr，而不是错拿
  AIUI_CHECK(root->findView<ListView>("tree") == nullptr);
  // 不存在的 id
  AIUI_CHECK(root->findView("missing") == nullptr);
  AIUI_CHECK(root->findView<TreeView>("missing") == nullptr);
}

AIUI_TEST(find_view_skips_views_without_id) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  // 没设 id 的控件不该被任何 id 命中
  auto* anonymous = new TreeView();
  root->addView(anonymous, LayoutParams{}.withSize(100, 100));

  AIUI_CHECK(anonymous->id() == 0);
  AIUI_CHECK(root->findViewById(0) == nullptr);
  AIUI_CHECK(root->findView("tree") == nullptr);
}

// ───────────────────────── ComboBox ─────────────────────────

AIUI_TEST(combo_popup_matches_control_width_and_height) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快", L"标准"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ComboSpec spec;
  spec.height = 30;
  combo->setSpec(spec);
  root->root()->layout(Rect{0, 0, 400, 300});

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  // 弹出层与显示框同宽，且从控件下沿开始
  const Rect popup = combo->overlayBounds();
  AIUI_CHECK_EQ(popup.left, combo->absoluteBounds().left);
  AIUI_CHECK_EQ(popup.right, combo->absoluteBounds().right);
  AIUI_CHECK_EQ(popup.top, combo->absoluteBounds().bottom + 2);
  // 行高与显示框一致，字号才不会跳变
  AIUI_CHECK_EQ(popup.height(), 30 * 3);
}

AIUI_TEST(combo_popup_click_selects_and_closes) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快", L"标准"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  int changed = -1;
  combo->setOnChange([&changed](int index) { changed = index; });

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  // 第三行：控件下沿 30 + 间距 2，再往下两行
  viewRoot->onMouseDown(mouseAt(20, 30 + 2 + 30 * 2 + 15));
  AIUI_CHECK_EQ(combo->selectedIndex(), 2);
  AIUI_CHECK_EQ(changed, 2);
  AIUI_CHECK(!combo->isPopupOpen());
}

AIUI_TEST(combo_popup_closes_when_clicking_outside) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快"});
  combo->setSelectedIndex(1);
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());
  // 点列表外：收起，且不该顺带清掉已选项
  viewRoot->onMouseDown(mouseAt(350, 280));
  AIUI_CHECK(!combo->isPopupOpen());
  AIUI_CHECK_EQ(combo->selectedIndex(), 1);
}

AIUI_TEST(combo_popup_closes_when_focus_moves_away) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  auto* other = new InputBox();
  // 纵向堆叠：输入框紧接在下拉框下面（y = 30..60）
  root->addView(other, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  // 焦点被程序化转移（Tab 导航、宿主设焦点）：列表必须自己收起，
  // 否则浮层会一直挂在屏幕上挡住下面的控件
  viewRoot->requestFocus(other);
  AIUI_CHECK(!combo->isPopupOpen());
}

AIUI_TEST(radio_group_centers_items_vertically) {
  TestEnv env;
  // 组高 30，选项行只有 22 高：应当上下留白均分，而不是贴顶
  auto group = std::make_unique<RadioGroup>();
  group->setOrientation(Orientation::kHorizontal);
  RadioButton* zip = group->addItem(L"ZIP");
  group->layout(Rect{0, 0, 200, 30});

  const int expectedTop = (30 - zip->bounds().height()) / 2;
  AIUI_CHECK_EQ(zip->bounds().top, expectedTop);
  AIUI_CHECK(zip->bounds().top > 0);
}

AIUI_TEST(combo_popup_focus_moves_in_one_click) {
  TestEnv env;
  // 横向摆放：弹层向下展开，右侧输入框不会被弹层盖住
  auto root = std::make_unique<LinearLayout>(Orientation::kHorizontal);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  auto* other = new InputBox();
  root->addView(other, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  // 点列表外的其它控件：收起的同时焦点要落过去，不能要求用户点两下
  viewRoot->onMouseDown(mouseAt(250, 15, MouseButton::kLeft));
  AIUI_CHECK(!combo->isPopupOpen());
  AIUI_CHECK(viewRoot->focused() == other);
}

AIUI_TEST(combo_popup_border_is_painted_after_rows) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"存储", L"最快", L"标准"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  // 先移到末行再回首行：hotRow 有变化才会标脏，随后补一次绘制
  viewRoot->onMouseMove(mouseAt(20, 30 + 2 + 30 * 2 + 15));
  viewRoot->onMouseMove(mouseAt(20, 30 + 2 + 15));
  viewRoot->onExpose(Rect{0, 0, 400, 300});
  const auto& fills = env.fake.platform().lastWindow()->lastFills();

  // 浮层描边必须排在所有行底色之后：行底色是矩形填充，
  // 先画描边再铺行会把面板边界整条盖掉
  int lastRowFill = -1;
  int borderStroke = -1;
  const Rect popup = combo->overlayBounds();
  for (size_t i = 0; i < fills.size(); ++i) {
    const FillCall& call = fills[i];
    // 行底色：直角矩形（radius == 0）、与浮层等宽、落在浮层内
    if (!call.stroke && call.radius == 0 && call.rect.left == popup.left &&
        call.rect.right == popup.right && call.rect.top >= popup.top &&
        call.rect.bottom <= popup.bottom) {
      lastRowFill = static_cast<int>(i);
    }
    if (call.stroke) borderStroke = static_cast<int>(i);
  }
  AIUI_CHECK(lastRowFill >= 0);
  AIUI_CHECK(borderStroke > lastRowFill);
}

AIUI_TEST(combo_popup_stays_inside_client_area) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  // 列表比窗口剩下的空间高：此时浮层仍要整体落在客户区内
  combo->setItems({L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  const Rect client{0, 0, 400, 120};
  AIUI_CHECK(root->create(nullptr, client));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  const Rect popup = combo->overlayBounds();
  AIUI_CHECK(popup.top >= client.top);
  AIUI_CHECK(popup.bottom <= client.bottom);
}

// 浮层只该接管自己范围内的鼠标。列表展开时鼠标移到标题栏按钮上，
// 若浮层无条件消费 move，那些区域就永远收不到悬停，关闭按钮不会变红。
AIUI_TEST(combo_popup_does_not_swallow_hover_outside) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* combo = new ComboBox();
  combo->setItems({L"一", L"二", L"三"});
  root->addView(combo, LayoutParams{}.withSize(180, 30));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 15, MouseButton::kLeft));
  AIUI_CHECK(combo->isPopupOpen());

  const Point outside{350, 250};
  AIUI_CHECK(!combo->overlayBounds().contains(outside));
  // 范围外必须放行，ViewRoot 才能把悬停交给下面的控件
  AIUI_CHECK(!combo->onOverlayMouseMove(outside));
  // 范围外移动不该在列表里留下高亮行
  AIUI_CHECK(combo->hotRow() == -1);

  // 范围内仍由浮层接管
  const Rect popup = combo->overlayBounds();
  AIUI_CHECK(combo->onOverlayMouseMove(Point{popup.left + 10, popup.top + 5}));
}

// ───────────────────────── 事件冒泡 ─────────────────────────

namespace {

// 记录自己收到了哪些阶段的探针容器
class ProbeGroup : public ViewGroup {
 public:
  std::vector<std::wstring> log;
  bool interceptDown = false;
  bool consumeDown = false;

 protected:
  bool onInterceptMouseDown(const MouseEvent&) override {
    log.push_back(L"capture");
    return interceptDown;
  }
  bool onMouseDown(const MouseEvent&) override {
    log.push_back(L"down");
    return consumeDown;
  }
  void onMouseUp(const MouseEvent&) override { log.push_back(L"up"); }
};

// 记录收到的探针叶子
class ProbeLeaf : public View {
 public:
  std::vector<std::wstring> log;
  bool consumeDown = false;
  bool stopUp = false;

 protected:
  bool onMouseDown(const MouseEvent& e) override {
    log.push_back(L"down");
    if (consumeDown) {
      e.stopPropagation();
      return true;
    }
    return false;
  }
  void onMouseUp(const MouseEvent& e) override {
    log.push_back(L"up");
    // 抬起的 stopPropagation 会让外层收不到
    if (stopUp) e.stopPropagation();
  }
};

}  // namespace

AIUI_TEST(mouse_down_bubbles_from_target_up_to_root) {
  TestEnv env;
  auto root = std::make_unique<ProbeGroup>();
  auto* leaf = new ProbeLeaf();
  root->addView(leaf, LayoutParams{}.withSize(400, 300));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  // 叶子不消费：事件要继续冒泡到父容器
  root->root()->onMouseDown(mouseAt(20, 20, MouseButton::kLeft));
  AIUI_CHECK_EQ(leaf->log.size(), static_cast<size_t>(1));
  AIUI_CHECK_EQ(root->log.size(), static_cast<size_t>(2));
  // 顺序是先捕获、再目标
  AIUI_CHECK(root->log[0] == L"capture");
  AIUI_CHECK(root->log[1] == L"down");
}

AIUI_TEST(stop_propagation_in_target_blocks_bubble) {
  TestEnv env;
  auto root = std::make_unique<ProbeGroup>();
  auto* leaf = new ProbeLeaf();
  leaf->consumeDown = true;
  root->addView(leaf, LayoutParams{}.withSize(400, 300));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  root->root()->onMouseDown(mouseAt(20, 20, MouseButton::kLeft));
  // 叶子消费后父容器只收到捕获，不再收到 down
  AIUI_CHECK_EQ(leaf->log.size(), static_cast<size_t>(1));
  AIUI_CHECK_EQ(root->log.size(), static_cast<size_t>(1));
  AIUI_CHECK(root->log[0] == L"capture");
}

AIUI_TEST(container_can_intercept_mouse_down_in_capture_phase) {
  TestEnv env;
  auto root = std::make_unique<ProbeGroup>();
  root->interceptDown = true;
  auto* leaf = new ProbeLeaf();
  root->addView(leaf, LayoutParams{}.withSize(400, 300));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  root->root()->onMouseDown(mouseAt(20, 20, MouseButton::kLeft));
  // 容器在捕获阶段拦下：叶子完全没收到
  AIUI_CHECK(leaf->log.empty());
  AIUI_CHECK_EQ(root->log.size(), static_cast<size_t>(2));
  AIUI_CHECK(root->log[0] == L"capture");
  AIUI_CHECK(root->log[1] == L"down");
}

AIUI_TEST(mouse_up_bubbles_and_can_be_stopped) {
  TestEnv env;
  auto root = std::make_unique<ProbeGroup>();
  auto* leaf = new ProbeLeaf();
  root->addView(leaf, LayoutParams{}.withSize(400, 300));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 20, MouseButton::kLeft));
  viewRoot->onMouseUp(mouseAt(20, 20, MouseButton::kLeft));
  AIUI_CHECK_EQ(leaf->log.size(), static_cast<size_t>(2));
  // 叶子没阻止，父容器应当收到 up
  AIUI_CHECK_EQ(root->log.size(), static_cast<size_t>(3));
  AIUI_CHECK(root->log[2] == L"up");
}

AIUI_TEST(mouse_up_stays_with_intercepting_container) {
  TestEnv env;
  auto root = std::make_unique<ProbeGroup>();
  root->interceptDown = true;
  auto* leaf = new ProbeLeaf();
  root->addView(leaf, LayoutParams{}.withSize(400, 300));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));

  ViewRoot* viewRoot = root->root();
  viewRoot->onMouseDown(mouseAt(20, 20, MouseButton::kLeft));
  // 按下被容器拦下后，抬起也必须回到容器：否则拖动状态无法复位
  viewRoot->onMouseUp(mouseAt(20, 20, MouseButton::kLeft));
  AIUI_CHECK(leaf->log.empty());
  AIUI_CHECK_EQ(root->log.size(), static_cast<size_t>(3));
  AIUI_CHECK(root->log[2] == L"up");
}

// ───────────────────────── PasswordBox ─────────────────────────

// 点击眼睛只切换显隐，不能顺带移动插入点：光标被甩到别处是最难忍的手感问题
AIUI_TEST(password_box_tail_icon_toggles_without_moving_caret) {
  TestEnv env;
  auto root = std::make_unique<LinearLayout>(Orientation::kVertical);
  auto* box = new PasswordBox();
  root->addView(box, LayoutParams{}.withSize(240, 34));
  AIUI_CHECK(root->create(nullptr, Rect{0, 0, 400, 300}));
  root->root()->requestFocus(box);

  box->setText(L"abcdef");
  AIUI_CHECK(!box->revealed());

  // 先把光标放到第 2 个字符之后，再看点击图标是否动它
  const int textLeft = 11;
  const int charWidth = env.fake.platform().charWidth();
  const int caretX = textLeft + charWidth;
  box->dispatchMouseDown(mouseAt(caretX, box->bounds().center().y, MouseButton::kLeft));
  box->dispatchMouseUp(mouseAt(caretX, box->bounds().center().y, MouseButton::kLeft));

  auto caretRight = [&] {
    root->root()->onExpose(Rect{0, 0, 400, 300});
    int right = -1;
    for (const FillCall& fill : env.fake.platform().lastWindow()->lastFills()) {
      if (fill.rect.width() <= 2 && fill.rect.height() > 10) right = fill.rect.right;
    }
    return right;
  };
  const int before = caretRight();
  AIUI_CHECK(before > 0);

  // 尾部图标在右侧内边距之前，点在它的中心
  const int iconX = 240 - (11 + 2) - 8;
  box->dispatchMouseDown(mouseAt(iconX, box->bounds().center().y, MouseButton::kLeft));
  box->dispatchMouseUp(mouseAt(iconX, box->bounds().center().y, MouseButton::kLeft));

  AIUI_CHECK(box->revealed());
  // 光标仍在原处：点击图标不该被当成「点到文本尾部」
  AIUI_CHECK_EQ(caretRight(), before);
}

// 掩码与图标必须是同一份状态：显示明文时是睁眼，掩码时是眼睛加斜线
AIUI_TEST(password_box_masks_text_and_swaps_icon) {
  TestEnv env;
  auto box = std::make_unique<PasswordBox>();
  box->layout(Rect{0, 0, 240, 34});
  box->setText(L"secret");

  // setSpec 走的是 XML 注入路径，不能把掩码状态冲掉
  InputSpec spec;
  spec.cue = L"不加密";
  box->setSpec(spec);
  AIUI_CHECK(!box->revealed());
  AIUI_CHECK(box->spec().password);
  AIUI_CHECK(box->spec().hasTailIcon);
  AIUI_CHECK(box->spec().tailIcon == IconId::kEyeOff);

  box->setRevealed(true);
  AIUI_CHECK(!box->spec().password);
  AIUI_CHECK(box->spec().tailIcon == IconId::kEye);
}
