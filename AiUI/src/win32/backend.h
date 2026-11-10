// Win32 后端内部共享声明（不对外暴露）
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
namespace win32 {

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
std::unique_ptr<Canvas> makeCanvas(HDC target, const Rect& rect);

// ── 字体 ──
Font makeFont(const wchar_t* family, float pointSize, bool bold, int dpi);
void releaseFont(void* handle);

// ── 图标 ──
void drawIconGlyph(Gdiplus::Graphics* g, IconId id, const Rect& rc, Color color);
void releaseIconCaches();
std::map<std::wstring, Image>& fileIconCache();

// ── 位图 ──
Image loadImageFile(const std::wstring& path);
Size imagePixelSize(const Image& image);
void releaseLoadedImage(Image image);
Gdiplus::Bitmap* asBitmap(const Image& image);

}  // namespace win32
}  // namespace AiUI
