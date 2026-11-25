// 文件与路径的小工具。
//
// 单独一层是因为「编译器」与「打包器」都要读文件、枚举目录、算绝对路径，
// 而标准库的这两件事在 Windows 上都要宽字符 API。把这些包成 UTF-8 接口后，
// 上层完全不必出现 `wchar_t`。
//
// 只用 <cstdio>/Win32，不引 <filesystem>：后者会带进一整套 locale 与
// 迭代器机器，而这个工具是会被拷进 dist/ 的发布件。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcc {

bool fileExists(const std::string& path);
bool directoryExists(const std::string& path);

// 读整个文件。失败返回 false（不区分「不存在」与「读不了」——调用方
// 的报错文案里已经有路径，再细分只会让文案变啰嗦）。
bool readFileBytes(const std::string& path, std::vector<std::uint8_t>* out);
bool readFileText(const std::string& path, std::string* out);
bool writeFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& data);
bool writeFileText(const std::string& path, const std::string& text);

std::uint64_t fileSize(const std::string& path);
// 最后修改时间（Windows FILETIME 的 100ns 计数）。用于「构建目录不止一个，
// 挑最新的 launcher」。
std::uint64_t lastWriteTime(const std::string& path);

// 目录下的直接子项名字（不含路径）。
std::vector<std::string> listDirectory(const std::string& path);

std::string joinPath(const std::string& base, const std::string& name);
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
std::string absolutePath(const std::string& path);
std::string normalizePath(const std::string& path);
// 小写扩展名，带点（`.lib`）。没有扩展名返回空串。
std::string extensionOf(const std::string& path);
// 去掉最后一个扩展名。
std::string stripExtension(const std::string& path);
std::string pathSeparator();

// 逐个替代 `split(';')`：Windows 的路径列表都是这个分隔符。
std::vector<std::string> splitList(const std::string& text);

// 进程临时目录（%TEMP%），尾部不带分隔符。
std::string tempDirectory();

}  // namespace pcc
