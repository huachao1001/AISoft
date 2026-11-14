// 操作系统能力接口
//
// AiUI 与系统之间的唯一契约：文件、进程、对话框、外壳集成、安装、托盘、
// 线程投递、单实例这些「系统能力」全部声明在这里。每个平台在
// src/os/<platform>/ 下实现一份，再接到 include/AiUI/platform.h 的门面上，
// 通用代码与应用只看见平台无关的门面。
//
// 约束：本文件虽然位于 src/os/，但要求和公开头一样——不得出现任何系统类型
// （HWND / HKEY / COM 接口等），只用 AiUI 的中性结构。否则换平台就不是
// 「补一份实现」，而是全量重写。
#pragma once

#include "AiUI/install.h"
#include "AiUI/menu.h"
#include "AiUI/platform.h"
#include "AiUI/shell.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AiUI {
namespace os {

// 一个平台必须提供的系统能力。窗口与绘制不在其中：那是 Platform 与
// PaintBackend 的职责，这里只管「非窗口」的系统交互。
class Backend {
 public:
  virtual ~Backend() = default;

  // ── 文件与目录 ──
  virtual std::wstring appDataDir(const std::wstring& appName) = 0;
  virtual std::vector<DirEntry> listDirectory(const std::wstring& dir, bool skipHidden) = 0;
  virtual bool pathExists(const std::wstring& path) = 0;
  virtual bool isDirectory(const std::wstring& path) = 0;
  virtual bool createDirectory(const std::wstring& path) = 0;
  virtual uint64_t fileSize(const std::wstring& path) = 0;
  virtual int64_t modifiedTime(const std::wstring& path) = 0;
  virtual bool setModifiedTime(const std::wstring& path, int64_t millis) = 0;
  virtual std::string readFile(const std::wstring& path, size_t maxBytes) = 0;
  virtual bool writeFile(const std::wstring& path, const std::string& data) = 0;
  virtual bool deleteFile(const std::wstring& path) = 0;
  virtual bool moveToTrash(const std::wstring& path) = 0;
  virtual std::unique_ptr<FileStream> openFileStream(const std::wstring& path, bool openWrite) = 0;
  virtual bool moveFile(const std::wstring& from, const std::wstring& to) = 0;
  virtual std::wstring absolutePath(const std::wstring& path) = 0;
  virtual std::wstring tempDir() = 0;
  virtual std::wstring specialFolder(SpecialFolder folder) = 0;
  virtual std::vector<DriveInfo> listDrives() = 0;

  // ── 进程 ──
  virtual std::wstring executablePath() = 0;
  virtual std::vector<std::wstring> commandLineArgs() = 0;
  virtual uint32_t processId() = 0;
  virtual bool isUiThread() = 0;
  virtual void sleepMillis(int ms) = 0;
  // 界面缩放比（DPI / 96）；纯系统查询，与窗口无关
  virtual float scaleFactor() = 0;

  // ── 路径与文本比较 ──
  // 大小写规则、分隔符、排序观感各不相同，必须由平台回答。
  virtual bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) = 0;
  virtual wchar_t pathSeparator() = 0;
  virtual int compareText(const std::wstring& a, const std::wstring& b) = 0;
  virtual bool containsTextIgnoreCase(const std::wstring& text, const std::wstring& needle) = 0;
  virtual bool equalsPath(const std::wstring& a, const std::wstring& b) = 0;

  // ── 对话框与弹出菜单 ──
  virtual MessageBoxResult showMessageBox(NativeHandle owner, const MessageBoxParams& params) = 0;
  virtual bool canOwnModal(NativeHandle window) = 0;
  virtual int showPopupMenu(NativeHandle owner, const Point& position,
                            const std::vector<MenuItem>& items) = 0;
  virtual Point cursorPosition() = 0;
  virtual void bringToFront(NativeHandle window) = 0;
  virtual std::wstring pickFolder(NativeHandle owner, const std::wstring& title,
                                  const std::wstring& initialDir) = 0;
  virtual std::wstring pickSaveFile(NativeHandle owner, const std::wstring& title,
                                    const std::wstring& initialDir,
                                    const std::wstring& defaultName,
                                    const std::wstring& defaultExt,
                                    const std::vector<std::wstring>& filters) = 0;
  virtual std::wstring pickOpenFile(NativeHandle owner, const std::wstring& title,
                                    const std::wstring& initialDir,
                                    const std::vector<std::wstring>& filters) = 0;

  // ── 剪贴板 ──
  virtual bool setClipboardText(const std::wstring& text) = 0;
  virtual std::wstring clipboardText() = 0;

  // ── 外壳集成 ──
  virtual bool openWithSystem(const std::wstring& path) = 0;
  virtual bool showProperties(NativeHandle owner, const std::wstring& path) = 0;
  virtual std::wstring registerShellVerbs(const std::vector<ShellVerb>& verbs) = 0;
  virtual std::wstring unregisterShellVerbs(const std::vector<ShellVerb>& verbs) = 0;
  virtual bool areShellVerbsRegistered(const std::vector<ShellVerb>& verbs) = 0;
  virtual std::wstring registerShellMenuHandler(const ShellMenuHandler& handler) = 0;
  virtual std::wstring unregisterShellMenuHandler(const ShellMenuHandler& handler) = 0;
  virtual bool isShellMenuHandlerRegistered(const ShellMenuHandler& handler) = 0;
  virtual InstallScope detectShellMenuScope(const ShellMenuHandler& handler) = 0;
  // 文件类型关联：把本程序登记为这些后缀的候选打开方式
  virtual std::wstring registerFileAssociations(const FileAssociation& association) = 0;
  virtual std::wstring unregisterFileAssociations(const FileAssociation& association) = 0;
  virtual bool areFileAssociationsRegistered(const FileAssociation& association) = 0;

  // ── 安装 ──
  virtual bool isRunningElevated() = 0;
  virtual bool relaunchElevated(const std::wstring& extraArgs = {}) = 0;
  virtual std::wstring defaultInstallDir(InstallScope scope) = 0;
  virtual std::wstring installFiles(const std::wstring& targetDir,
                                    const std::vector<InstallFile>& files) = 0;
  virtual std::wstring removeInstalledFiles(const std::wstring& targetDir,
                                            const std::vector<InstallFile>& files) = 0;
  virtual std::wstring createShortcuts(const std::wstring& targetDir,
                                       const std::wstring& exeName,
                                       const std::wstring& displayName, InstallScope scope) = 0;
  virtual std::wstring removeShortcuts(const std::wstring& displayName, InstallScope scope) = 0;
  virtual int runAndWait(const std::wstring& exePath, const std::wstring& arguments) = 0;
  // 动态加载模块（Windows DLL / macOS dylib / Linux .so）与取导出符号
  virtual void* loadModule(const std::wstring& path) = 0;
  virtual void* moduleSymbol(void* module, const char* name) = 0;
  virtual void unloadModule(void* module) = 0;
  // 系统「应用与功能」卸载入口
  virtual std::wstring registerUninstallEntry(const std::wstring& displayName,
                                              const std::wstring& displayVersion,
                                              const std::wstring& publisher,
                                              const std::wstring& installDir,
                                              const std::wstring& uninstallCommand,
                                              const std::wstring& uninstallArgs,
                                              InstallScope scope) = 0;
  virtual std::wstring unregisterUninstallEntry(const std::wstring& displayName,
                                                InstallScope scope) = 0;
  // 卸载收尾：复制自身、脱离式启动、下次开机删除
  virtual std::wstring copySelfTo(const std::wstring& destPath) = 0;
  virtual bool launchDetached(const std::wstring& exePath, const std::wstring& arguments,
                              const std::wstring& workDir) = 0;
  virtual bool deleteFileOnReboot(const std::wstring& path) = 0;

  // ── 清单式落盘 ──
  // 把单个内嵌资源写到完整路径（允许带子目录）
  virtual std::wstring installSingleResource(int resourceId, const std::wstring& targetPath) = 0;
  // 目录为空时删除
  virtual bool removeDirectoryIfEmpty(const std::wstring& dir) = 0;

  // ── 托盘 ──
  virtual std::unique_ptr<PlatformTray> createTrayIcon(PlatformTrayHost& host) = 0;

  // ── 线程投递 ──
  virtual void postToUiThread(std::function<void()> fn) = 0;
  virtual void invokeOnUiThread(std::function<void()> fn) = 0;
  // 消息循环启动前的准备与收尾；不需要专门机制的平台留空即可
  virtual void prepareUiThreadDispatch() {}
  virtual void shutdownUiThreadDispatch() {}

  // ── 单实例 ──
  virtual bool claimSingleInstance(const std::wstring& appId) = 0;
  virtual void setLaunchPayloadHandler(std::function<void(const std::wstring& payload)> fn) = 0;
  virtual bool forwardToRunningInstance(const std::wstring& appId,
                                        const std::wstring& payload) = 0;
};

// 当前平台的后端实例。由各平台的实现文件提供，进程内唯一。
Backend& backend();

}  // namespace os
}  // namespace AiUI
