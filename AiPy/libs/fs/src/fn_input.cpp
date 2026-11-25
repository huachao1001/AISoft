// `input`：从标准输入读一行。
//
// ── 为什么在 fs.pylib 而不是 VM 内建 ──
//
// 与 `open` 同一条理由：读 stdin 是平台能力，核心 VM 不该认识它（铁律一）。
// CPython 里 `input` 是内建，但「是内建」说的是**脚本里不用 import**，
// 与「实现必须编在解释器里」是两件事。这里用 AIPY_PYLIB_BUILTIN 把它注册成
// 裸名，脚本照常写 `input("name? ")`。
//
// ── 与 CPython 的差异 ──
//
// 1. **不抛 EOFError**。AiPy 没有异常通道，stdin 到底时返回 None——与
//    `open` 失败返回 None 是同一条约定。
// 2. **不做编码转换**。按字节读，原样当 UTF-8 交给脚本。CPython 会按
//    `sys.stdin.encoding` 解码，而这里没有那个概念；宿主负责让控制台与
//    管道给出一致的字节序列（launcher 里切了 CP_UTF8）。
//
// ── 为什么行尾要剥掉 ──
//
// CPython 的 `input()` 返回**不带换行**的内容。剥掉 `\r\n` / `\n` / `\r`
// 三种是必要的：从 `echo name | app.exe` 管道进来的是 `\r\n`，只剥 `\n`
// 会留下一个不可见的 `\r`，用户看到的是「比较字符串总是不相等」。
#include "aipy/pylib.h"
#include "aipy/value.h"

#include <cstdio>
#include <string>

namespace {

// 读一行到 EOF 或换行。返回 false 表示一个字节都没读到（stdin 已关）。
bool readLine(std::string* out) {
  out->clear();
  for (;;) {
    const int ch = std::fgetc(stdin);
    if (ch == EOF) return !out->empty();
    if (ch == '\n') return true;
    out->push_back(static_cast<char>(ch));
  }
}

// 提示语必须**不带换行**地出现在输入光标之前。所以这里不走 `log`：
// log sink 会在末尾补一个 `\n`（那是 `print` 的语义），提示语会变成
// 「name? \n」再等输入，与 CPython 不一致。
//
// 直接写 stdout 是 CPython 自己也做的事（`input()` 的 prompt 走 stdout）。
// 写完必须 flush：不 flush 的话提示语留在缓冲区里，用户看到的是一个
// 光标对着空白，不知道程序在等什么。
void writePrompt(const std::string& prompt) {
  if (prompt.empty()) return;
  std::fwrite(prompt.data(), 1, prompt.size(), stdout);
  std::fflush(stdout);
}

aipy::Value inputImpl(const aipy::Value* args, int count) {
  const std::string prompt = count >= 1 ? args[0].toString() : std::string();
  writePrompt(prompt);

  std::string line;
  if (!readLine(&line)) return aipy::Value();
  // 去掉行尾的 `\r`：Windows 管道给的是 `\r\n`，fgetc 在 `\n` 处停下，
  // 于是 `\r` 留在了行内。
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return aipy::Value(std::move(line));
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, input, inputImpl);
