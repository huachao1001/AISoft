#include "test_framework.h"

#include <cstdio>

namespace pyui {
namespace test {

std::vector<Case>& registry() {
  // 函数内静态：注册用的静态对象在别的翻译单元里初始化，谁先谁后不确定，
  // 用全局对象会撞上「静态初始化顺序」问题（注册表还没构造就 insert）。
  static std::vector<Case> cases;
  return cases;
}

int& currentFailures() {
  static int failures = 0;
  return failures;
}

void reportFailure(const char* file, int line, const std::string& message) {
  ++currentFailures();
  std::printf("  FAIL %s:%d\n    %s\n", file, line, message.c_str());
}

}  // namespace test
}  // namespace pyui

int main() {
  using namespace pyui::test;

  int failedCases = 0;
  for (const Case& item : registry()) {
    // 逐个刷新并在**开跑前**打一行：崩在用例里时，最后一行就是它。
    // 不加这行的话，段错误会把缓冲区里攒的输出全吞掉，看起来像
    // 「停在某个无关的用例上」。
    std::printf("...  %s\n", item.name.c_str());
    std::fflush(stdout);
    const int before = currentFailures();
    item.fn();
    const int added = currentFailures() - before;
    if (added == 0) {
      std::printf("ok   %s\n", item.name.c_str());
    } else {
      ++failedCases;
      // 用例名放在失败信息之后输出，才能一眼看出这一串断言属于哪个控件：
      // 一次跑几十个用例时，先看到名字更容易定位。
      std::printf("FAIL %s (%d 条断言)\n", item.name.c_str(), added);
    }
  }

  std::printf("\n%d 个用例，%d 个失败，%d 条断言失败\n",
              static_cast<int>(registry().size()), failedCases, currentFailures());
  // 退出码非 0 才能让 CI 与构建脚本察觉；只看输出容易被漏掉。
  return failedCases == 0 ? 0 : 1;
}
