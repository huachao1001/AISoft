// `sys.executable()` —— 当前 exe 的绝对路径。
//
// 与 CPython 的 `sys.executable` 同义。关联文件、托盘图标这类能力都要写
// 「指向自己的绝对路径」，脚本自己拼 `argv[0]` 不可靠：它可能是相对路径、
// 也可能没有扩展名。
#include "aipy/pylib.h"
#include "aipy/vm.h"

#include <string>
#include <vector>

namespace {

aipy::Value executableImpl(const aipy::Value*, int) {
  const std::vector<std::string>& args = aipy::vm::commandLine();
  return aipy::Value(args.empty() ? std::string() : args.front());
}

}  // namespace

AIPY_PYLIB_FUNCTION(sys, executable, executableImpl);
