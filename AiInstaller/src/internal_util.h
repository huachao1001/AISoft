// 安装器内部的小工具：路径拼接与目录创建
//
// 放在 src/ 里而不是公开头：这些是库内部约定，不构成对外契约。
// 全部经 AiUI 的平台接口完成，不出现任何系统 API。
#pragma once

#include <string>

namespace AiInstaller {
namespace detail {

// "dir" + "name"，自动补一个分隔符
std::wstring joinPath(const std::wstring& dir, const std::wstring& name);

// 路径最后一段（含扩展名）
std::wstring fileNameOf(const std::wstring& path);

// 去掉最后一段后的目录；没有分隔符时返回空串
std::wstring directoryOf(const std::wstring& path);

// 逐级创建目录，已存在视为成功。
// 注意：不处理 UNC 共享路径，安装目录不会落在那里。
bool ensureDirectoryTree(const std::wstring& dir);

}  // namespace detail
}  // namespace AiInstaller
