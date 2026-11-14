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

Module::~Module() { unload(); }

Module::Module(Module&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

Module& Module::operator=(Module&& other) noexcept {
  if (this != &other) {
    unload();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool Module::load(const std::wstring& path, std::wstring& reason) {
  unload();
  handle_ = platform()->loadModule(path);
  if (!handle_) {
    reason = L"无法加载模块：" + path;
    return false;
  }
  return true;
}

void* Module::symbol(const char* name) const {
  return handle_ ? platform()->moduleSymbol(handle_, name) : nullptr;
}

void Module::unload() {
  if (handle_) {
    platform()->unloadModule(handle_);
    handle_ = nullptr;
  }
}

std::wstring registerUninstallEntry(const std::wstring& displayName,
                                    const std::wstring& displayVersion,
                                    const std::wstring& publisher,
                                    const std::wstring& installDir,
                                    const std::wstring& uninstallCommand,
                                    const std::wstring& uninstallArgs, InstallScope scope) {
  return platform()->registerUninstallEntry(displayName, displayVersion, publisher, installDir,
                                            uninstallCommand, uninstallArgs, scope);
}

std::wstring unregisterUninstallEntry(const std::wstring& displayName, InstallScope scope) {
  return platform()->unregisterUninstallEntry(displayName, scope);
}

void writeInstallDirectories(const std::wstring& displayName,
                             const std::vector<std::wstring>& dirs, InstallScope scope) {
  platform()->writeInstallDirectories(displayName, dirs, scope);
}

std::vector<std::wstring> readInstallDirectories(const std::wstring& displayName,
                                                 InstallScope scope) {
  return platform()->readInstallDirectories(displayName, scope);
}

std::wstring setExecutableSystemIcon(const std::wstring& exePath, SystemIcon icon) {
  return platform()->setExecutableSystemIcon(exePath, icon);
}
void queryInstalled(const std::wstring& displayName, InstallScope scope, bool& found,
                    std::wstring& installDir, InstallScope& installedScope) {
  platform()->queryInstalled(displayName, scope, found, installDir, installedScope);
}

std::wstring copySelfTo(const std::wstring& destPath) { return platform()->copySelfTo(destPath); }

bool launchDetached(const std::wstring& exePath, const std::wstring& arguments,
                    const std::wstring& workDir) {
  return platform()->launchDetached(exePath, arguments, workDir);
}

bool deleteFileOnReboot(const std::wstring& path) { return platform()->deleteFileOnReboot(path); }

std::wstring installSingleResource(int resourceId, const std::wstring& targetPath) {
  return platform()->installSingleResource(resourceId, targetPath);
}

bool removeDirectoryIfEmpty(const std::wstring& dir) {
  return platform()->removeDirectoryIfEmpty(dir);
}

}  // namespace AiUI
