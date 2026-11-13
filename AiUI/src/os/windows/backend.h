// Windows 后端内部共享声明（不对外暴露）
//
// 系统能力本身声明在 src/os/os.h，实现在 windows_backend.h。这里是
// Windows 独有的细节：GDI+ 初始化、绘制、图标缓存、字体。这些没有跨平台
// 可比性，不需要抽象。
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
// 绘图/系统能力是否已启动
void markStarted(bool started);
bool hasStarted();

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

}  // namespace windows
}  // namespace os
}  // namespace AiUI
