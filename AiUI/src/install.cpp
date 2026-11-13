// 安装与卸载：把应用请求转给平台后端
#include "AiUI/install.h"

#include "AiUI/platform.h"

namespace AiUI {

bool isRunningElevated() { return platform()->isRunningElevated(); }

bool relaunchElevated(const std::wstring& extraArgs) {
  return platform()->relaunchElevated(extraArgs);
}

std::wstring defaultInstallDir(InstallScope scope) { return platform()->defaultInstallDir(scope); }

std::wstring installFiles(const std::wstring& targetDir, const std::vector<InstallFile>& files) {
  return platform()->installFiles(targetDir, files);
}

std::wstring removeInstalledFiles(const std::wstring& targetDir,
                                  const std::vector<InstallFile>& files) {
  return platform()->removeInstalledFiles(targetDir, files);
}

std::wstring createShortcuts(const std::wstring& targetDir, const std::wstring& exeName,
                             const std::wstring& displayName, InstallScope scope) {
  return platform()->createShortcuts(targetDir, exeName, displayName, scope);
}

std::wstring removeShortcuts(const std::wstring& displayName, InstallScope scope) {
  return platform()->removeShortcuts(displayName, scope);
}

int runAndWait(const std::wstring& exePath, const std::wstring& arguments) {
  return platform()->runAndWait(exePath, arguments);
}

}  // namespace AiUI
