#include "internal_util.h"

#include "AiUI/platform.h"

namespace AiInstaller {
namespace detail {

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  if (name.empty()) return dir;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

std::wstring directoryOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::vector<std::wstring> missingDirectories(const std::wstring& dir) {
  std::vector<std::wstring> out;
  if (dir.empty()) return out;

  // 从盘符之后开始逐段检查："D:\A\B" 看 "D:\A" 与 "D:\A\B"。
  // "D:" 本身不能当目录建（它是「D 盘当前目录」），所以跳过盘符。
  size_t start = 0;
  if (dir.size() >= 2 && dir[1] == L':') start = 2;

  for (size_t i = start; i < dir.size(); ++i) {
    if (dir[i] != L'\\' && dir[i] != L'/') continue;
    if (i <= start) continue;
    const std::wstring prefix = dir.substr(0, i);
    if (!AiUI::platform()->isDirectory(prefix)) out.push_back(prefix);
  }
  if (!AiUI::platform()->isDirectory(dir)) out.push_back(dir);
  return out;
}

bool ensureDirectoryTree(const std::wstring& dir) {
  if (dir.empty()) return true;
  if (AiUI::platform()->isDirectory(dir)) return true;

  // 从盘符之后开始逐段创建："D:\A\B" 先建 "D:\A" 再建 "D:\A\B"。
  // "D:" 本身不能当目录建（它是「D 盘当前目录」），所以跳过盘符。
  size_t start = 0;
  if (dir.size() >= 2 && dir[1] == L':') start = 2;

  for (size_t i = start; i < dir.size(); ++i) {
    if (dir[i] != L'\\' && dir[i] != L'/') continue;
    if (i <= start) continue;
    const std::wstring prefix = dir.substr(0, i);
    if (!AiUI::platform()->isDirectory(prefix)) AiUI::platform()->createDirectory(prefix);
  }
  return AiUI::platform()->isDirectory(dir) || AiUI::platform()->createDirectory(dir);
}

}  // namespace detail
}  // namespace AiInstaller
