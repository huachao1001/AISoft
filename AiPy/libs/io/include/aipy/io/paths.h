// 路径与错误描述的共用层。
//
// ── 为什么与文件读写分开 ──
//
// `toNativePath` / `pathExists` / `describeError` 被 `fs`、`os`、`os.path`
// 三个库都要用；而 `readAll` / `writeAll` 只有 `fs.open` 用。
//
// 混在一个编译单元里的后果：只想判个「文件在不在」的产物，会把整套文本
// 换行归一与读写缓冲一起拖进去。分开之后，按需链接才真的按需。
//
// ── 为什么可以同时出现在几个 .pylib 里 ──
//
// 同一个 obj 被多个归档各带一份、一起链进产物时，MSVC 的 COMDAT 折叠加会
// 把它们并成一个（实测：两个库各带一份 shared.obj 链接零警告）。所以这里
// 不做「把共用代码塞进核心库」那种事——核心库保持零平台代码。
#pragma once

#include <string>

namespace aipy {
namespace io {

// 把 UTF-8 路径转成平台原生的路径表示。
//
// Windows 上必须是宽字符：脚本里的路径是 UTF-8，而 ANSI 版 API 会按系统
// 代码页解释，中文用户名、中文目录下直接失败（读不到文件，报「不存在」）。
//
// 失败返回空容器——调用方据此给出「路径无法解析」而不是「文件不存在」。
#if defined(_WIN32)
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

NativePath toNativePath(const std::string& utf8);

// 文件/目录是否存在。isDirectory 可为空；非空时写入「是不是目录」。
// 存在但没有权限时返回 false，与 CPython 在多数情况下的表现一致。
bool pathExists(const std::string& path, bool* isDirectory);

// 一次错误描述的格式：`原因` 或 `路径: 原因`。
//
// 统一在这里拼，是为了让脚本看到的所有错误长得一样
// ——「打不开 a.txt: 系统找不到指定的文件」这种。
std::string describeError(const std::string& path, const std::string& reason);

// 系统错误码 → 可读原因。errno 那天没被设置时返回泛化描述。
std::string reasonOf(int code);

// CPython 文本模式写入的换行：Windows 上是 `\r\n`，其余平台是 `\n`。
const char* platformNewline();

// 把文本里的 `\n` 换成本平台换行。
//
// 这与 CPython 一样是**盲目替换**：原本就写成 `\r\n` 的内容在 Windows 上
// 会变成 `\r\r\n`。看起来像 bug，但那是 CPython 的真实行为，对齐优先于
// 「看着更合理」。
std::string toPlatformNewlines(const std::string& text);

// universal newlines：`\r\n` 与单独的 `\r` 都归一成 `\n`。
std::string normalizeNewlines(const std::string& raw);

}  // namespace io
}  // namespace aipy
