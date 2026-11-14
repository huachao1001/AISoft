// Windows 的系统能力实现
//
// 继承 src/os/os.h 的纯接口，本文件只声明不实现细节；实现在
// platform_windows.cpp（文件、进程、对话框、外壳、线程、单实例）、
// install_windows.cpp（安装与快捷方式）、shell_menu_register.cpp（右键组件落地）。
#pragma once

#include "../os.h"

namespace AiUI {
namespace os {
namespace windows {

class WindowsBackend final : public Backend {
 public:
  // ── 文件与目录 ──
  std::wstring appDataDir(const std::wstring& appName) override;
  std::vector<DirEntry> listDirectory(const std::wstring& dir, bool skipHidden) override;
  bool pathExists(const std::wstring& path) override;
  bool isDirectory(const std::wstring& path) override;
  bool createDirectory(const std::wstring& path) override;
  uint64_t fileSize(const std::wstring& path) override;
  int64_t modifiedTime(const std::wstring& path) override;
  bool setModifiedTime(const std::wstring& path, int64_t millis) override;
  std::string readFile(const std::wstring& path, size_t maxBytes) override;
  bool writeFile(const std::wstring& path, const std::string& data) override;
  bool deleteFile(const std::wstring& path) override;
  bool moveToTrash(const std::wstring& path) override;
  std::unique_ptr<FileStream> openFileStream(const std::wstring& path, bool openWrite) override;
  std::unique_ptr<FileStream> openFileStreamForUpdate(const std::wstring& path) override;
  bool moveFile(const std::wstring& from, const std::wstring& to) override;
  std::wstring absolutePath(const std::wstring& path) override;
  std::wstring tempDir() override;
  std::wstring specialFolder(SpecialFolder folder) override;
  std::vector<DriveInfo> listDrives() override;

  // ── 进程 ──
  std::wstring executablePath() override;
  std::vector<std::wstring> commandLineArgs() override;
  uint32_t processId() override;
  bool isUiThread() override;
  void sleepMillis(int ms) override;
  float scaleFactor() override;

  // ── 路径与文本比较 ──
  bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) override;
  wchar_t pathSeparator() override;
  int compareText(const std::wstring& a, const std::wstring& b) override;
  bool containsTextIgnoreCase(const std::wstring& text, const std::wstring& needle) override;
  bool equalsPath(const std::wstring& a, const std::wstring& b) override;

  // ── 对话框与弹出菜单 ──
  MessageBoxResult showMessageBox(NativeHandle owner, const MessageBoxParams& params) override;
  bool canOwnModal(NativeHandle window) override;
  int showPopupMenu(NativeHandle owner, const Point& position,
                    const std::vector<MenuItem>& items) override;
  Point cursorPosition() override;
  void bringToFront(NativeHandle window) override;
  std::wstring pickFolder(NativeHandle owner, const std::wstring& title,
                          const std::wstring& initialDir) override;
  std::wstring pickSaveFile(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir, const std::wstring& defaultName,
                            const std::wstring& defaultExt,
                            const std::vector<std::wstring>& filters) override;
  std::wstring pickOpenFile(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir,
                            const std::vector<std::wstring>& filters) override;
  std::vector<std::wstring> pickOpenFiles(NativeHandle owner, const std::wstring& title,
                                          const std::wstring& initialDir,
                                          const std::vector<std::wstring>& filters) override;

  // ── 剪贴板 ──
  bool setClipboardText(const std::wstring& text) override;
  std::wstring clipboardText() override;

  // ── 外壳集成 ──
  bool openWithSystem(const std::wstring& path) override;
  bool showProperties(NativeHandle owner, const std::wstring& path) override;
  std::wstring registerShellVerbs(const std::vector<ShellVerb>& verbs) override;
  std::wstring unregisterShellVerbs(const std::vector<ShellVerb>& verbs) override;
  bool areShellVerbsRegistered(const std::vector<ShellVerb>& verbs) override;
  std::wstring registerShellMenuHandler(const ShellMenuHandler& handler) override;
  std::wstring unregisterShellMenuHandler(const ShellMenuHandler& handler) override;
  bool isShellMenuHandlerRegistered(const ShellMenuHandler& handler) override;
  InstallScope detectShellMenuScope(const ShellMenuHandler& handler) override;

  // ── 文件类型关联 ──
  std::wstring registerFileAssociations(const FileAssociation& association) override;
  std::wstring unregisterFileAssociations(const FileAssociation& association) override;
  bool areFileAssociationsRegistered(const FileAssociation& association) override;

  // ── 安装 ──
  bool isRunningElevated() override;
  bool relaunchElevated(const std::wstring& extraArgs = {}) override;
  std::wstring defaultInstallDir(InstallScope scope) override;
  std::wstring installFiles(const std::wstring& targetDir,
                            const std::vector<InstallFile>& files) override;
  std::wstring removeInstalledFiles(const std::wstring& targetDir,
                                    const std::vector<InstallFile>& files) override;
  std::wstring createShortcuts(const std::wstring& targetDir, const std::wstring& exeName,
                               const std::wstring& displayName, InstallScope scope) override;
  std::wstring removeShortcuts(const std::wstring& displayName, InstallScope scope) override;
  int runAndWait(const std::wstring& exePath, const std::wstring& arguments) override;

  // ── 模块加载 ──
  void* loadModule(const std::wstring& path) override;
  void* moduleSymbol(void* module, const char* name) override;
  void unloadModule(void* module) override;

  // ── 卸载入口 ──
  std::wstring registerUninstallEntry(const std::wstring& displayName,
                                      const std::wstring& displayVersion,
                                      const std::wstring& publisher,
                                      const std::wstring& installDir,
                                      const std::wstring& uninstallCommand,
                                      const std::wstring& uninstallArgs,
                                      InstallScope scope) override;
  std::wstring unregisterUninstallEntry(const std::wstring& displayName,
                                        InstallScope scope) override;
  // 改写 exe 的图标资源：卸载程序用系统的回收站图标
  std::wstring setExecutableSystemIcon(const std::wstring& exePath, SystemIcon icon) override;
  // 安装时新建的目录层级记在卸载项里：安装目录该只有应用自己的东西
  void writeInstallDirectories(const std::wstring& displayName,
                               const std::vector<std::wstring>& dirs,
                               InstallScope scope) override;
  std::vector<std::wstring> readInstallDirectories(const std::wstring& displayName,
                                                   InstallScope scope) override;
  // 是否已装过：看卸载项里的 InstallLocation 指向的目录还在不在
  void queryInstalled(const std::wstring& displayName, InstallScope scope, bool& found,
                      std::wstring& installDir, InstallScope& installedScope) override;

  // ── 卸载收尾 ──
  std::wstring copySelfTo(const std::wstring& destPath) override;
  bool launchDetached(const std::wstring& exePath, const std::wstring& arguments,
                      const std::wstring& workDir) override;
  bool deleteFileOnReboot(const std::wstring& path) override;

  // ── 清单式落盘 ──
  std::wstring installSingleResource(int resourceId, const std::wstring& targetPath) override;
  bool removeDirectoryIfEmpty(const std::wstring& dir) override;

  // ── 托盘 ──
  std::unique_ptr<PlatformTray> createTrayIcon(PlatformTrayHost& host) override;

  // ── 线程投递 ──
  void postToUiThread(std::function<void()> fn) override;
  void invokeOnUiThread(std::function<void()> fn) override;
  void prepareUiThreadDispatch() override;
  void shutdownUiThreadDispatch() override;

  // ── 单实例 ──
  bool claimSingleInstance(const std::wstring& appId) override;
  void setLaunchPayloadHandler(std::function<void(const std::wstring& payload)> fn) override;
  bool forwardToRunningInstance(const std::wstring& appId, const std::wstring& payload) override;
};

}  // namespace windows
}  // namespace os
}  // namespace AiUI
