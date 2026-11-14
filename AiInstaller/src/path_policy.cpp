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

// 探针文件名：带进程 id，避免两台安装器同时探测时互相删掉对方刚建的文件。
std::wstring probeFileName() {
  return L"aisetup_write_probe_" +
         std::to_wstring(AiUI::platform()->processId()) + L".tmp";
}

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

bool isVolumeWritable(const std::wstring& root) {
  if (root.empty()) return false;

  const std::wstring probe = joinInstallDir(root, probeFileName());
  if (!AiUI::platform()->writeFile(probe, std::string())) return false;
  // 删不掉不影响判断：能写就说明装得进去，残留一个 0 字节文件比误判更轻。
  AiUI::platform()->deleteFile(probe);
  return true;
}

std::wstring pickInstallVolume() {
  const std::vector<Volume> volumes = listInstallVolumes();
  if (volumes.empty()) return {};

  // 先找非系统盘：系统盘装满系统与页面文件，装到别处更稳。
  // 只跳过不可写的——盘存在但装不进去，选中它等于让用户等到最后一步才失败。
  for (const auto& volume : volumes) {
    if (!volume.system && isVolumeWritable(volume.root)) return volume.root;
  }
  // 没有可用的非系统盘时才退而求其次用系统盘
  for (const auto& volume : volumes) {
    if (isVolumeWritable(volume.root)) return volume.root;
  }
  return {};
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

std::wstring perUserInstallDir(const AppInfo& app) { return defaultInstallDir(app); }

}  // namespace AiInstaller
