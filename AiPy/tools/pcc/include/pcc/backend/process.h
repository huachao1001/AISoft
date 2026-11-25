// 启动子进程并收集输出。
//
// ── 为什么不用 `std::system` 或 `_popen` ──
//
// 两者都只能拿 stdout（system 连这个都拿不到），而需要的是「逐字参数数组」：
// 输出路径里可能有空格，拼命令行少一对引号就会静默失败。
//
// ── 为什么 stdout / stderr 必须分开 ──
//
// `link.exe /dump /symbols` 的输出要按行解析符号名。混进 stderr 的警告会
// 被当成符号，然后「这个库没有提供函数级符号」——按需链接悄悄退化成整库。
// 所以两个管道、两条读取路径。
#pragma once

#include <string>
#include <vector>

namespace pcc {

struct ProcessResult {
  int exitCode = 0;
  std::string out;
  std::string err;
  bool started = false;  // false = 进程根本没起来（路径错等）
  unsigned long lastError = 0;  // started 为 false 时的 Win32 错误码
};

// environment 是**增量**：父进程的环境会先继承下来，同名项被覆盖。
// 这与 Python 版 `env = dict(os.environ); env['LIB'] = ...` 一致——
// 只传增量而丢掉 PATH，会让 link.exe 找不到自己的 DLL。
ProcessResult runProcess(const std::vector<std::string>& arguments,
                         const std::vector<std::string>& environment);

}  // namespace pcc
