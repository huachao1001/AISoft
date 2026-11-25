// `fs` pylib 的内部接口：打开文件、读全部、写全部。
//
// ── 为什么只剩这些 ──
//
// 路径转换、存在性判断、错误描述、换行归一都搬去了 `aipy/io/paths.h`：
// `os` 与 `os.path` 也要用那些，只有「真的读写文件」是 `fs.open` 独有的。
// 混在一起会让一个只想判存在的产物带上整套读写缓冲。
//
// ── 为什么到处都是宽字符 ──
//
// 脚本里的路径是 UTF-8，而 Windows 的 ANSI API（`fopen`）按**系统代码页**
// 解释。在中文用户名 / 中文目录下，`C:\Users\张三\a.txt` 直接打不开，并且
// 报的错是「找不到文件」——那会把用户引向完全错误的方向。
//
// 所以 Windows 上一律用 `_wfopen`，路径先经 `toNativePath` 转成宽字符。
#pragma once

#include <string>

namespace aipy {
namespace fs {

// 打开文件。成功返回非空句柄，失败返回 nullptr 并在 errno 里留原因。
//
// 单独一个函数是因为 fopen/fread 的路径参数在两个平台上类型不同（宽/窄），
// 而 fs 各函数都要读文件。
void* openForRead(const std::string& path);
void* openForWrite(const std::string& path, bool append);
// 独占创建（Python 的 "x" 模式）：文件已存在就失败，不覆盖。
// 用 O_EXCL 而不是「先查存在再打开」，后者在并发下会静默覆盖。
void* openExclusive(const std::string& path, bool plus);
// 打开已有的读写句柄（"r+" / "a+"）。plus 表示同时要读。
void* openForUpdate(const std::string& path, bool append);

// 读全部内容。失败返回 false，error 里给可读原因。
//
// text 为真时做 universal newlines：`\r\n` 与单独的 `\r` 都归一成 `\n`。
// 这是 CPython 文本模式的读语义——不归一的话，Windows 上 `readline()` 会
// 带着 `\r\n` 回来，脚本再 `strip("\n")` 就留下一个 `\r`。
bool readAll(const std::string& path, std::string* content, std::string* error,
             bool text = true);
// 写全部内容（覆盖或追加）。text 为真时把 `\n` 换成本平台换行。
bool writeAll(const std::string& path, const std::string& content, bool append,
              std::string* error, bool text = true);

}  // namespace fs
}  // namespace aipy
