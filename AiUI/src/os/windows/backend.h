// Windows 后端内部共享声明（不对外暴露）
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/platform.h"

#include <windows.h>

// GDI+ 头依赖 objidl.h（IStream），必须在它之前引入
#include <objidl.h>

#include <gdiplus.h>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace AiUI {
namespace os {
namespace windows {

// ── GDI+ 进程级初始化 ──
bool ensureGdiplus();
void releaseGdiplus();

// ── 平台后端选择 ──
Platform* backendPlatform();
// 测试替换：非空时 platform() 返回它，空则回到底层 Win32
Platform* testPlatformOverride();
void markTestOverride(Platform* platform);

// ── 绘制 ──
std::unique_ptr<PaintBackend> makePaintBackend(Gdiplus::Graphics* g);

// 离屏位图复用池：满窗重绘一次就是一张几十 MB 的图，每帧重建会拖慢
// 缩放/最大化动画，所以按窗口常驻一张，按需扩容。
class CanvasCache {
 public:
  CanvasCache() = default;
  ~CanvasCache();
  CanvasCache(const CanvasCache&) = delete;
  CanvasCache& operator=(const CanvasCache&) = delete;

  // 建一个覆盖 rc 的画布；不够大时会自动换更大的位图
  std::unique_ptr<Canvas> create(HDC target, const Rect& rc);

 private:
  HDC memdc_ = nullptr;
  HBITMAP dib_ = nullptr;
  HBITMAP oldBitmap_ = nullptr;
  void* bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// ── 字体 ──
Font makeFont(const wchar_t* family, float pointSize, bool bold, int dpi);
void releaseFont(void* handle);

// 编辑控件的排版格式：单行、不折行、不裁边、保留尾随空格。
// 必须在 GenericTypographic 基底上构建：GDI+ 的默认 StringFormat 会在左右
// 各补 em/6 的排版留白，使「布局原点」不等于「墨迹起点」，光标另算就会错位。
// 调用方式：
//   Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
//   applyRunFormat(fmt);
void applyRunFormat(Gdiplus::StringFormat& fmt);

// ── 图标 ──
void drawIconGlyph(Gdiplus::Graphics* g, IconId id, const Rect& rc, Color color);
void releaseIconCaches();
std::map<std::wstring, Image>& fileIconCache();

// ── 位图 ──
Image loadImageFile(const std::wstring& path);
Size imagePixelSize(const Image& image);
void releaseLoadedImage(Image image);
Gdiplus::Bitmap* asBitmap(const Image& image);
// HICON → ARGB 位图；句柄由调用方释放，返回值由 releaseLoadedImage 释放
Image imageFromIcon(HICON icon);
// 从可执行文件里取指定边长的原生图标；失败返回空句柄，调用方负责 DestroyIcon。
// 任务栏与 Alt+Tab 预览按尺寸向系统要图标，所以要按需取对应档位。
HICON loadAppIcon(int pixelSize);

// ── 平台能力（实现在 platform_windows.cpp）──
// 这些与窗口绘制无关，WindowsPlatform 只做转发
std::wstring appDataDirImpl(const std::wstring& appName);
std::vector<DirEntry> listDirectoryImpl(const std::wstring& dir, bool skipHidden);
bool pathExistsImpl(const std::wstring& path);
bool isDirectoryImpl(const std::wstring& path);
bool createDirectoryImpl(const std::wstring& path);
uint64_t fileSizeImpl(const std::wstring& path);
int64_t modifiedTimeImpl(const std::wstring& path);
bool setModifiedTimeImpl(const std::wstring& path, int64_t millis);
std::string readFileImpl(const std::wstring& path, size_t maxBytes);
bool writeFileImpl(const std::wstring& path, const std::string& data);
bool deleteFileImpl(const std::wstring& path);
bool moveToTrashImpl(const std::wstring& path);
std::unique_ptr<FileStream> openFileStreamImpl(const std::wstring& path, bool openWrite);
bool moveFileImpl(const std::wstring& from, const std::wstring& to);
std::wstring absolutePathImpl(const std::wstring& path);
std::wstring executablePathImpl();
std::vector<std::wstring> commandLineArgsImpl();
std::wstring tempDirImpl();
uint32_t processIdImpl();
std::wstring specialFolderImpl(SpecialFolder folder);
std::vector<DriveInfo> listDrivesImpl();

MessageBoxResult showMessageBoxImpl(NativeHandle owner, const MessageBoxParams& params);
bool canOwnModalImpl(NativeHandle window);
int showPopupMenuImpl(NativeHandle owner, const Point& position,
                      const std::vector<MenuItem>& items);
Point cursorPositionImpl();
void bringToFrontImpl(NativeHandle window);

std::wstring registerShellVerbsImpl(const std::vector<ShellVerb>& verbs);
std::wstring unregisterShellVerbsImpl(const std::vector<ShellVerb>& verbs);
bool areShellVerbsRegisteredImpl(const std::vector<ShellVerb>& verbs);

std::wstring registerShellMenuHandlerImpl(const ShellMenuHandler& handler);
std::wstring unregisterShellMenuHandlerImpl(const ShellMenuHandler& handler);
bool isShellMenuHandlerRegisteredImpl(const ShellMenuHandler& handler);

bool isRunningElevatedImpl();
bool relaunchElevatedImpl();
std::wstring defaultInstallDirImpl(InstallScope scope);
std::wstring installFilesImpl(const std::wstring& targetDir, const std::vector<InstallFile>& files);
std::wstring removeInstalledFilesImpl(const std::wstring& targetDir,
                                      const std::vector<InstallFile>& files);
std::wstring writeInstallMarkerImpl(const std::wstring& targetDir, const std::wstring& marker,
                                    const std::string& content);
std::wstring createShortcutsImpl(const std::wstring& targetDir, const std::wstring& exeName,
                                 const std::wstring& displayName, InstallScope scope);
std::wstring removeShortcutsImpl(const std::wstring& displayName, InstallScope scope);
int runAndWaitImpl(const std::wstring& exePath, const std::wstring& arguments);

std::unique_ptr<PlatformTray> createTrayIconImpl(PlatformTrayHost& host);

bool isUiThreadImpl();
void sleepMillisImpl(int ms);
float scaleFactorImpl();

std::wstring pickFolderImpl(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir);
std::wstring pickSaveFileImpl(NativeHandle owner, const std::wstring& title,
                              const std::wstring& initialDir, const std::wstring& defaultName,
                              const std::wstring& defaultExt,
                              const std::vector<std::wstring>& filters);
std::wstring pickOpenFileImpl(NativeHandle owner, const std::wstring& title,
                              const std::wstring& initialDir,
                              const std::vector<std::wstring>& filters);

bool openWithSystemImpl(const std::wstring& path);
bool showPropertiesImpl(NativeHandle owner, const std::wstring& path);

// UI 线程投递：窗口线程需在消息循环启动前调 prepareUiThreadDispatch
void prepareUiThreadDispatch();
void shutdownUiThreadDispatch();
void postToUiThread(std::function<void()> fn);
void invokeOnUiThread(std::function<void()> fn);

bool claimSingleInstanceImpl(const std::wstring& appId);
void setLaunchPayloadHandlerImpl(std::function<void(const std::wstring&)> fn);
bool forwardToRunningInstanceImpl(const std::wstring& appId, const std::wstring& payload);

}  // namespace windows
}  // namespace os
}  // namespace AiUI
