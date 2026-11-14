// 安装器内部的小工具：路径拼接与目录创建
//
// 放在 src/ 里而不是公开头：这些是库内部约定，不构成对外契约。
// 全部经 AiUI 的平台接口完成，不出现任何系统 API。
#pragma once

#include <string>
#include <vector>

namespace AiInstaller {
namespace detail {

// "dir" + "name"，自动补一个分隔符
std::wstring joinPath(const std::wstring& dir, const std::wstring& name);

// 去掉最后一段后的目录；没有分隔符时返回空串
std::wstring directoryOf(const std::wstring& path);

// 逐级创建目录，已存在视为成功。
// 注意：不处理 UNC 共享路径，安装目录不会落在那里。
bool ensureDirectoryTree(const std::wstring& dir);

// 列出 dir 这条路径上「当前还不存在」的各级目录，从最外层到 dir 自身。
//
// 安装时用它记下「哪几级目录是我新建的」，卸载时照单收回。比按目录名
// 猜「这是不是系统目录」可靠：记录的每一级都确实由安装器创建。
std::vector<std::wstring> missingDirectories(const std::wstring& dir);

}  // namespace detail
}  // namespace AiInstaller
