// `fs` pylib 的内部共用接口。
//
// 为什么要一个头而不是各文件各写一份：路径处理（UTF-8 ↔ 宽字符）与
// 错误信息格式是每个函数都要用的，重复写就会各写一套、各错一处。
#pragma once

#include "aipy/value.h"

#include <string>
#include <string_view>
#include <vector>

namespace aipy {
namespace fs {

// 把 UTF-8 路径转成平台原生的路径表示。
//
// Windows 上必须是宽字符：脚本里的路径是 UTF-8，而 ANSI 版 API 会按系统
// 代码页解释，中文用户名、中文目录下直接失败（读不到文件，报「不存在」）。
//
// 失败返回空容器——调用方据此给出「路径无法解析」而不是「文件不存在」。
#if defined(_WIN32)
std::wstring toNativePath(const std::string& utf8);
#else
std::string toNativePath(const std::string& utf8);
#endif

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

// CPython 文本模式写入的换行：Windows 上是 `\r\n`，其余平台是 `\n`。
const char* platformNewline();

// 把文本里的 `\n` 换成本平台换行。
//
// 这与 CPython 一样是**盲目替换**：原本就写成 `\r\n` 的内容在 Windows 上
// 会变成 `\r\r\n`。看起来像 bug，但那是 CPython 的真实行为，对齐优先于
// 「看着更合理」（见不可对拍清单）。
std::string toPlatformNewlines(std::string_view text);

// universal newlines：`\r\n` 与单独的 `\r` 都归一成 `\n`。
std::string normalizeNewlines(std::string_view raw);

// 文件/目录是否存在。
bool pathExists(const std::string& path, bool* isDirectory);

// 一次错误描述的格式：`原因` 或 `路径: 原因`。
//
// 统一在这里拼，是为了让脚本看到的所有 fs 错误长得一样——
// 「打不开 a.txt: 系统找不到指定的文件」这种。
std::string describeError(const std::string& path, const std::string& reason);

}  // namespace fs
}  // namespace aipy
