// 体积归因：逐层加功能的探针。
//
// 存在的理由：预算表不能靠估。每加一层就链接一次、量一次，得到的是
// 真实边际开销；估出来的数字在换编译器版本后立刻失效。
//
// 用宏选择层次，一次编译一个版本，避免「所有层都编进去再猜谁占了多少」。
#include <windows.h>

#if LEVEL >= 1
#include "pyui/py_value.h"
#include "pyui/py_script.h"
#include "pyui/app_window.h"
#include "pyui/layout_inflater.h"
#include "pyui/view_group.h"
#endif

#if LEVEL >= 2
#include "pyui/widgets/button.h"
#include "pyui/widgets/text_view.h"
#endif

namespace {

#if LEVEL >= 1
class ProbeScript : public pyui::py::Script {
 public:
  void initState() override {
#if LEVEL >= 2
    // find() 是 Script 的成员：它要拿到 root_ 才能找控件。
    pyui::py::Value label = find(pyui::py::Value("label"));
    pyui::py::setText(label, pyui::py::Value("Hello"));
#endif
  }
};
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
#if LEVEL == 0
  // 只有 CRT 与 wWinMain：这是静态 CRT 的固定成本。
  return 0;
#else
  ProbeScript script;
  return pyui::py::run(script);
#endif
}
