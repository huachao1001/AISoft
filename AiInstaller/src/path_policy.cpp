// 安装路径策略
//
// 「优先非系统盘」是安装器的通用决定，不是某个应用的知识，所以放在这里。
// 盘符枚举、系统盘判定这些系统事实由 AiUI 的后端回答，本文件只做选择。
#include "AiInstaller/path_policy.h"

#include "AiUI/platform.h"

#include <algorithm>
#include <string>

namespace AiInstaller {
namespace {

// AiUI 的卷描述已经区分了可移动盘与固定盘，这里只做筛选
bool isFixedVolume(const AiUI::DriveInfo& drive) { return !drive.removable; }

}  // namespace

std::vector<Volume> listInstallVolumes() {
  std::vector<Volume> volumes;
  for (const auto& drive : AiUI::platform()->listDrives()) {
    if (!isFixedVolume(drive)) continue;
    volumes.push_back(Volume{drive.path, drive.system, 0});
  }
  // 按盘符排序：策略依赖「字母序靠前」，不能指望后端返回顺序
  std::sort(volumes.begin(), volumes.end(),
            [](const Volume& a, const Volume& b) { return a.root < b.root; });
  return volumes;
}

std::wstring pickInstallVolume() {
  const std::vector<Volume> volumes = listInstallVolumes();
  if (volumes.empty()) return {};

  // 先找非系统盘：系统盘装满系统与页面文件，装到别处更稳
  for (const auto& volume : volumes) {
    if (!volume.system) return volume.root;
  }
  // 只有系统盘可用时才退而求其次
  return volumes.front().root;
}

std::wstring joinInstallDir(const std::wstring& volumeRoot, const std::wstring& subdir) {
  if (volumeRoot.empty()) return subdir;
  if (subdir.empty()) return volumeRoot;

  std::wstring dir = volumeRoot;
  // 统一成「盘根 + 单个反斜杠 + 子目录」
  if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
  std::wstring tail = subdir;
  while (!tail.empty() && (tail.front() == L'\\' || tail.front() == L'/')) tail.erase(0, 1);
  dir += tail;
  // 去掉尾部多余分隔符
  while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return dir;
}

std::wstring defaultInstallDir(const AppInfo& app) {
  const std::wstring volume = pickInstallVolume();
  if (volume.empty()) return {};
  return joinInstallDir(volume, app.installSubdir);
}

std::wstring perUserInstallDir(const AppInfo& app) {
  // 装到用户自己的 Programs 目录：不需要提权，也不和别的账户互相干扰
  const std::wstring local = AiUI::platform()->specialFolder(AiUI::SpecialFolder::kHome);
  if (local.empty()) return defaultInstallDir(app);

  std::wstring dir = local;
  if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
  dir += L"Programs\\";
  dir += app.installSubdir;
  return dir;
}

}  // namespace AiInstaller
