// 跑一份字节码，把脚本的 `print` 输出打到 stdout——「像 python 那样跑个脚本」。
//
// ── 为什么和 aipy_vm_dump 分开 ──
//
// 两个工具的 stdout 含义不同，混在一起必然互相破坏：
//
//   aipy_run  stdout = 程序的输出（`print` 打的东西）
//   aipy_vm_dump  stdout = 机器读的 JSON（全局状态）
//
// `aipy_vm_dump` 是给测试和调试用的——语义对拍脚本要解析它的 stdout，
// 所以脚本的 `print` 在那里只能留在 stderr。日常「跑一下看看」用这个工具。
#include "aipy/bytecode.h"
#include "aipy/vm.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "用法: aipy_run <file.bc>\n");
    return 2;
  }

  // 脚本的 `print` 走 log sink。语言层的默认实现写 stderr 并加 `[aipy]`
  // 前缀，那是调试诊断的口径；跑脚本时用户期待的是「和 python 一样」，
  // 所以这里换成写到 stdout、不加前缀。
  aipy::setLogSink([](const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
  });
  // print(end=...) 要求结尾完全可控，所以另装一条不补换行的通道。
  aipy::setWriteSink([](const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
  });

  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "读不到 %s\n", argv[1]);
    return 2;
  }
  const std::vector<char> raw{std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>()};
  const std::vector<std::uint8_t> data(raw.begin(), raw.end());

  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    std::fprintf(stderr, "%s: %s\n", argv[1], loaded.error.c_str());
    return 1;
  }

  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(loaded.program);
  if (result.failed) {
    // 行号是脚本里的行号，不是字节码偏移——这是「能查出来」和「查不出来」
    // 的分界，所以一定要打。
    if (result.line > 0) {
      std::fprintf(stderr, "%s:%d: %s\n", argv[1], result.line, result.error.c_str());
    } else {
      std::fprintf(stderr, "%s: %s\n", argv[1], result.error.c_str());
    }
    return 1;
  }
  return 0;
}
