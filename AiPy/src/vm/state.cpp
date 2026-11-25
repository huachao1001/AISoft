// 热加载：把旧程序的状态搬到新程序上。
//
// ── 为什么按名字搬，而不是按下标 ──
//
// 改一行脚本重新生成字节码，但用户已经填进表单、点了几次按钮的状态不能丢。
// 按下标搬是错的：新程序里加一个变量，后面的槽会整体移位，于是 `count`
// 的值被灌进 `label`——不崩，只是界面上显示的数字悄悄换了意思。
//
// 按名字搬还有一个好处：脚本删掉的变量自然被丢弃，语义上正确（那个值
// 在新程序里已经没有意义），不需要额外的清理逻辑。
#include "aipy/vm.h"

namespace aipy {
namespace vm {

std::vector<Value> carryGlobals(const Program& from, const std::vector<Value>& values,
                                const Program& to) {
  std::vector<Value> carried(static_cast<std::size_t>(to.globalCount), Value());
  const std::size_t count = from.globalNames.size() < values.size()
                                ? from.globalNames.size()
                                : values.size();
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t j = 0; j < to.globalNames.size(); ++j) {
      if (to.globalNames[j] == from.globalNames[i]) {
        carried[j] = values[i];
        break;
      }
    }
  }
  return carried;
}

}  // namespace vm
}  // namespace aipy
