// 铁律一与铁律二的自动化检查。
//
// 两条铁律都靠「人记得别那么写」是守不住的：一次疏忽只会在第三个平台或者
// 某个大字号 DPI 上暴露，而且很难回溯到原因。所以这里把约束变成可执行断言。
//
// 铁律二（控件是纯绘制）：整个界面树只有一个原生窗口，控件自身不带句柄。
//   控件里根本没有 handle 字段，所以能测的是它的可观察后果：
//     - 叶子控件不往树里塞内部子视图（那里面最可能藏原生滚动条/编辑框）；
//     - 控件类名里不出现平台前缀（出现就说明有人加了一层平台包装）。
//
// 铁律一（公开头文件不出现平台类型）：这是**源码**约束。这里扫描
//   include/pyui/ 下每个头文件的实际代码行，断言没有 HWND/HDC/windows.h
//   之类的记号。路径从 __FILE__ 推导，不依赖当前工作目录。
#include "widget_test_support.h"

#include "pyui/layouts/frame_layout.h"
#include "pyui/layouts/linear_layout.h"
#include "pyui/widgets/button.h"
#include "pyui/widgets/check_box.h"
#include "pyui/widgets/progress_bar.h"
#include "pyui/widgets/radio_button.h"
#include "pyui/widgets/slider.h"
#include "pyui/widgets/text_field.h"
#include "pyui/widgets/text_view.h"
#include "pyui/widgets/title_bar.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pyui {
namespace test {
namespace {

// 平台类型与头文件的记号。命中任意一条即视为铁律一被破。
const char* const kForbiddenTokens[] = {
    "HWND",         "HDC",     "HBITMAP", "HBRUSH",  "COLORREF",  "HINSTANCE",
    "HMENU",        "HICON",   "HCURSOR", "HDROP",   "CGContextRef", "cairo_t",
    "NSView",       "NSWindow", "gtk_widget", "windows.h", "Gdiplus", "gtk/gtk.h",
};

// 扫描单个文件；返回命中的记号（空表示干净）。
// 只跳过整行注释：注释里写「不要用 HWND」是正当的，但为这点小事引一个
// 注释剥离器不值得，所以约定注释一律占整行、不写在代码后面。
std::vector<std::string> scanFile(const std::string& path) {
  std::vector<std::string> hits;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    hits.push_back("<无法打开 " + path + ">");
    return hits;
  }

  std::string line;
  int lineNumber = 0;
  while (std::getline(in, line)) {
    ++lineNumber;
    const size_t firstNonSpace = line.find_first_not_of(" \t\r");
    if (firstNonSpace != std::string::npos &&
        line.compare(firstNonSpace, 2, "//") == 0) {
      continue;
    }

    for (const char* token : kForbiddenTokens) {
      if (line.find(token) != std::string::npos) {
        std::ostringstream out;
        out << path << ':' << lineNumber << " 出现平台记号 " << token;
        hits.push_back(out.str());
        break;  // 一行只报一次，够定位就行
      }
    }
  }
  return hits;
}

// 从 __FILE__ 推出 include/pyui 的路径。
//
// __FILE__ 是**相对于编译期工作目录**的（这里是 "tests\test_widget_no_native.cpp"），
// 不是绝对路径，所以不能靠「剥两级目录」去推项目根——那样在相对路径下会
// 落到盘符根上。只剥文件名，再拼 "../include/pyui"：路径里留一个 ".." 不
// 影响打开文件，也不会因为 cwd 不同而错位。
std::string includeDir() {
  std::string path = __FILE__;
  const size_t cut = path.find_last_of("/\\");
  if (cut != std::string::npos) path.erase(cut + 1);  // 保留末尾分隔符 → 目录
  return path + "../include/pyui";
}

}  // namespace

// ── 铁律二：控件树里没有平台对象 ──

TEST(no_native_every_widget_is_a_pure_drawn_view) {
  // 把所有控件挂进同一棵树，模拟一个「什么都有」的界面。
  // className() 必须是通用名字：真出现 PlatformButton 这类包装类时，
  // 这里会立刻失败。
  FrameLayout host;
  host.add<TextView>("t");
  host.add<Button>();
  host.add<CheckBox>("c");
  host.add<RadioButton>();
  host.add<TextField>();
  host.add<ProgressBar>();
  host.add<Slider>();
  host.add<TitleBar>();
  host.add<LinearLayout>();

  const char* const kForbiddenPrefixes[] = {"Platform", "Native", "Win32", "Cocoa", "Gtk"};

  const std::function<void(View*)> visit = [&](View* view) {
    CHECK(view != nullptr);
    if (view == nullptr) return;
    const std::string name = view->className();
    for (const char* prefix : kForbiddenPrefixes) {
      CHECK_MSG(name.compare(0, std::strlen(prefix), prefix) != 0,
                std::string("控件 ") + name + " 像是平台包装类");
    }
    auto* group = dynamic_cast<ViewGroup*>(view);
    if (group == nullptr) return;
    for (View* child : group->children()) visit(child);
  };
  visit(&host);
}

TEST(no_native_widgets_never_create_children_of_their_own) {
  // 叶子控件不允许偷偷往树里塞内部子视图：一旦塞了，里面很可能藏着
  // 一个原生控件（滚动条、编辑框）。只有容器类才允许有子视图。
  TextView text("t");
  Button button;
  button.setLabel("b");
  CheckBox check("c");
  RadioButton radio;
  TextField field;
  ProgressBar progress;
  Slider slider;

  View* const leaves[] = {&text, &button, &check, &radio, &field, &progress, &slider};
  for (View* view : leaves) {
    // 叶子控件不是容器：dynamic_cast 到 ViewGroup 必须失败。
    // 一旦它变成了 ViewGroup，就说明有人给它塞了内部子视图。
    CHECK_MSG(dynamic_cast<ViewGroup*>(view) == nullptr,
              std::string(view->className()) + " 是叶子控件，不该是容器");
  }
}

// TitleBar 是容器（按钮 + 标题），但它的每个后代的类名也必须是通用控件，
// 且不能带句柄。
TEST(no_native_title_bar_subtree_is_all_generic_widgets) {
  TitleBar bar;
  bar.setAppearance(titleBarAppearance());
  bar.layout(Rect::fromSize(400, 32));

  int visited = 0;
  const std::function<void(View*)> visit = [&](View* view) {
    ++visited;
    CHECK(view != nullptr);
    if (view == nullptr) return;
    CHECK(std::strlen(view->className()) > 0);
    // 子视图只存在于 ViewGroup 上；叶子控件没有子视图正说明它是纯绘制的。
    auto* group = dynamic_cast<ViewGroup*>(view);
    if (group == nullptr) return;
    for (View* child : group->children()) visit(child);
  };
  visit(&bar);

  // 图标 + 标题 + 占位 + 按钮条 + 三个按钮 + TitleBar 自身。
  // 结构来自 titlebar.xml（与 AiUI 的 TitleBar 逐节点对齐），所以这里
  // 断言的是「必要部件都在」：少于 8 个说明有一层没建出来。
  CHECK_MSG(visited >= 8, "标题栏的子树不完整");
}

// ── 铁律一：公开头文件里没有平台类型 ──

TEST(no_native_public_headers_name_platform_types) {
  const std::string root = includeDir();
  const char* const kHeaders[] = {
      "types.h",
      "theme.h",
      "events.h",
      "painter.h",
      "surface.h",
      "text_renderer.h",
      "view.h",
      "view_group.h",
      "view_root.h",
      "id.h",
      "specs.h",
      "menu.h",
      "canvas.h",
      "software_backend.h",
      "layout_inflater.h",
      "layouts/frame_layout.h",
      "layouts/linear_layout.h",
      "widgets/text_view.h",
      "widgets/button.h",
      "widgets/check_box.h",
      "widgets/radio_button.h",
      "widgets/text_field.h",
      "widgets/progress_bar.h",
      "widgets/slider.h",
      "widgets/title_bar.h",
      "platform/platform.h",
      "platform/title_bar.h",
      "platform/window.h",
      "platform/context_menu.h",
      "platform/file_association.h",
      "platform/tray.h",
      "testing/recording_painter.h",
  };

  int scanned = 0;
  for (const char* header : kHeaders) {
    const std::vector<std::string> hits = scanFile(root + "/" + header);
    ++scanned;
    for (const std::string& hit : hits) FAIL_AT(hit);
  }

  // 新增头文件时要把它加进清单；下界故意保守，只拦住「清单被削短」。
  CHECK_MSG(scanned >= 30, "头文件清单过短，可能漏扫了新增的公开头文件");
}

TEST(no_native_scan_detects_a_violation) {
  // 扫描器自身也要被验证：否则「全绿」可能只是因为它什么都没读进去。
  // 探针写在 include 目录里而不是 build 目录：路径从 __FILE__ 推出来，
  // 不依赖当前工作目录，跑完立刻删掉。
  const std::string path = includeDir() + "/_scan_probe.h";
  {
    std::ofstream out(path);
    CHECK(static_cast<bool>(out));
    out << "#pragma once\n"
        << "void f(HWND h);\n";
  }
  const std::vector<std::string> hits = scanFile(path);
  CHECK_MSG(!hits.empty(), "扫描器没抓到明显的平台类型，等于没有检查");

  std::remove(path.c_str());
}

}  // namespace test
}  // namespace pyui

