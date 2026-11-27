// `sys.argv()` —— 宿主传给脚本的命令行参数。
//
// 与 CPython 的 `sys.argv` 同义：下标 0 是 exe 自己的路径，之后是用户传的。
//
// ── 为什么叫函数而不是常量 ──
//
// AiPy 没有模块级常量（`import sys` 只登记名字，不构造模块对象），所以
// 想拿到「列表」只能调一次。写成 `sys.argv()` 而不是 `sys.argv`：后者会
// 被当成读一个不存在的名字，而前者与 `os.listdir()` 这类调用同形，脚本
// 作者不需要记第二种形态。
#include "aipy/pylib.h"
#include "aipy/vm.h"

#include <string>
#include <vector>

namespace {

aipy::Value argvImpl(const aipy::Value*, int) {
  const std::vector<std::string>& args = aipy::vm::commandLine();
  aipy::Value::List items;
  items.reserve(args.size());
  for (const std::string& arg : args) items.push_back(aipy::Value(arg));
  return aipy::Value::list(std::move(items));
}

}  // namespace

AIPY_PYLIB_FUNCTION(sys, argv, argvImpl);
