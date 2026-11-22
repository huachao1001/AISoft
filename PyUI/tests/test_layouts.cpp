// 布局编译链路的端到端测试（铁律四）。
//
// 这一份测试与其它控件测试不同：它不测某个控件的像素，而是测「XML 真的
// 变成了可用的控件树」。它验证的正是铁律四那两条硬性约束的可观察结果：
//
//   1. 只暴露 inflate("名字")，没有生成的布局类；
//   2. 取到的树里每个控件都真的被设上了 XML 里的属性。
//
// 它由 CMake 的 pyui_add_layouts() 把 assets/layouts/demo.xml 编译进来后
// 才有内容可测；没有生成代码时（例如手工跑单测脚本）这些用例会跳过而不是
// 报错——布局编译是构建期行为，不该让单文件测试依赖它。
#include "widget_test_support.h"

#include "pyui/layout_inflater.h"
#include "pyui/layouts/linear_layout.h"
#include "pyui/widgets/button.h"
#include "pyui/widgets/check_box.h"
#include "pyui/widgets/progress_bar.h"
#include "pyui/widgets/radio_button.h"
#include "pyui/widgets/slider.h"
#include "pyui/widgets/text_field.h"
#include "pyui/widgets/text_view.h"

namespace pyui {
namespace test {

// 生成代码没接进来时，这些用例全部跳过。用 has() 判断而不是链接期符号，
// 因为没有生成目标时连符号都不存在，会被链接器拒绝。
#define REQUIRE_LAYOUT(name)                        \
  do {                                              \
    if (!::pyui::LayoutInflater::has(name)) return;  \
  } while (false)

TEST(layout_registry_exposes_demo_by_name) {
  REQUIRE_LAYOUT("demo");

  // 名字必须是 XML 主名，不带路径也不带扩展名
  CHECK(LayoutInflater::has("demo"));
  CHECK(!LayoutInflater::has("demo.xml"));
  CHECK(!LayoutInflater::has("assets/layouts/demo"));
}

TEST(layout_inflate_returns_a_fresh_tree_each_time) {
  REQUIRE_LAYOUT("demo");

  auto first = LayoutInflater::inflate("demo");
  auto second = LayoutInflater::inflate("demo");
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  if (first == nullptr || second == nullptr) return;

  // 每次调用都建新树：两个实例不能是同一块内存，否则第二次 inflate 会
  // 把第一次的控件树抢走（双击打开两个窗口就会互相干扰）。
  CHECK_MSG(first.get() != second.get(), "inflate 必须每次建一棵新树");

  auto* a = first->findView<Button>("ok");
  auto* b = second->findView<Button>("ok");
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  if (a != nullptr && b != nullptr) CHECK_MSG(a != b, "同名控件必须是两个不同对象");
}

TEST(layout_demo_root_is_vertical_linear_with_padding_and_spacing) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  CHECK(root != nullptr);
  if (root == nullptr) return;

  auto* column = dynamic_cast<LinearLayout*>(root.get());
  CHECK(column != nullptr);
  if (column == nullptr) return;

  CHECK_EQ(static_cast<int>(column->orientation()), static_cast<int>(Orientation::kVertical));
  CHECK_EQ(column->spacing(), 8);
  // padding="12" 是四边全 12 的简写
  CHECK_EQ(column->padding().left, 12);
  CHECK_EQ(column->padding().top, 12);
  CHECK_EQ(column->padding().right, 12);
  CHECK_EQ(column->padding().bottom, 12);
}

TEST(layout_demo_text_styles_are_applied) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;
  auto* title = root->findView<TextView>("title");
  CHECK(title != nullptr);
  if (title == nullptr) return;

  CHECK_EQ(title->text(), std::string(u8"PyUI 布局样例"));
  CHECK_EQ(static_cast<int>(title->textStyle().size), static_cast<int>(TextSize::kHuge));
  CHECK(title->textStyle().bold);
  CHECK_EQ(title->textStyle().color, makeColor(0x22, 0x22, 0x22));
  // height="wrap" 落在布局参数上，不是控件内部状态
  CHECK_EQ(title->layoutParams().height, LayoutParams::kWrapContent);
}

TEST(layout_demo_align_is_two_axis) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;
  auto* crumb = root->findView<TextView>("crumb");
  CHECK(crumb != nullptr);
  if (crumb == nullptr) return;

  // align="start,center" 是 LayoutParams 的双轴对齐，与 textAlign 无关
  CHECK_EQ(static_cast<int>(crumb->layoutParams().horizontal), static_cast<int>(Gravity::kStart));
  CHECK_EQ(static_cast<int>(crumb->layoutParams().vertical), static_cast<int>(Gravity::kCenter));
}

TEST(layout_demo_button_variants_are_set) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;

  auto* ok = root->findView<Button>("ok");
  auto* more = root->findView<Button>("more");
  auto* stretch = root->findView<Button>("stretch");
  CHECK(ok != nullptr);
  CHECK(more != nullptr);
  CHECK(stretch != nullptr);
  if (ok == nullptr || more == nullptr || stretch == nullptr) return;

  CHECK_EQ(static_cast<int>(ok->spec().style), static_cast<int>(ButtonStyle::kPrimary));
  CHECK_EQ(ok->label(), std::string(u8"确定"));
  CHECK_EQ(ok->spec().radius, 6);

  CHECK_EQ(static_cast<int>(more->spec().style), static_cast<int>(ButtonStyle::kOutline));
  CHECK_EQ(static_cast<int>(more->spec().icon), static_cast<int>(IconId::kChevronDown));
  CHECK_EQ(static_cast<int>(more->spec().placement), static_cast<int>(IconPlacement::kLeft));

  // weight="1" 让这个按钮吃掉剩余空间，而不是固定 80
  CHECK_EQ(stretch->layoutParams().weight, 1.0f);
}

TEST(layout_demo_checked_states_survive_inflation) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;

  auto* checksum = root->findView<CheckBox>("optChecksum");
  auto* partial = root->findView<CheckBox>("optPartial");
  CHECK(checksum != nullptr);
  CHECK(partial != nullptr);
  if (checksum != nullptr) CHECK(checksum->isChecked());
  if (partial != nullptr) CHECK(partial->isTristate());

  auto* fast = root->findView<RadioButton>("optFast");
  CHECK(fast != nullptr);
  if (fast != nullptr) CHECK(fast->isChecked());
}

TEST(layout_demo_numeric_ranges_are_applied) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;

  auto* bar = root->findView<ProgressBar>("bar");
  auto* busy = root->findView<ProgressBar>("barBusy");
  CHECK(bar != nullptr);
  CHECK(busy != nullptr);
  if (bar != nullptr) {
    CHECK_NEAR(bar->progress(), 0.35f, 1e-6f);
    // 给了确定进度就不该留在不确定态
    CHECK(!bar->isIndeterminate());
  }
  if (busy != nullptr) CHECK(busy->isIndeterminate());

  auto* level = root->findView<Slider>("level");
  CHECK(level != nullptr);
  if (level != nullptr) {
    CHECK_NEAR(level->minimum(), 1.0, 1e-9);
    CHECK_NEAR(level->maximum(), 9.0, 1e-9);
    CHECK_NEAR(level->value(), 5.0, 1e-9);
    CHECK_NEAR(level->step(), 1.0, 1e-9);
  }
}

TEST(layout_demo_password_field_is_masked) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;
  auto* secret = root->findView<TextField>("secret");
  CHECK(secret != nullptr);
  if (secret != nullptr) CHECK(secret->isPassword());
}

TEST(layout_demo_measures_and_paints_without_a_window) {
  REQUIRE_LAYOUT("demo");

  auto root = LayoutInflater::inflate("demo");
  if (root == nullptr) return;

  // 离线可用是铁律四「代价与补救」里的要求：预览工具不应该需要窗口。
  // 这里走一遍完整测量 + 绘制，断言它不崩、且确实画出了东西。
  const Surface surface = renderDirect(*root, Size{360, 480});
  const Rect ink = alphaBounds(surface);
  CHECK_MSG(!ink.isEmpty(), "布局树在离线渲染时应该画出内容");
  CHECK_MSG(ink.right <= 360 && ink.bottom <= 480, "绘制不得越界");
}

TEST(layout_unknown_name_returns_null_not_crash) {
  // 拼错布局名是最常见的错误：必须是「返回空」而不是抛异常或崩。
  CHECK(LayoutInflater::inflate("definitely_not_a_layout") == nullptr);
  CHECK(!LayoutInflater::has("definitely_not_a_layout"));
}

}  // namespace test
}  // namespace pyui
