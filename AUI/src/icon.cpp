#include "AUI/icon.h"

#include "AUI/platform.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <cmath>
#include <map>
#include <string>

namespace AUI {
namespace {

// 运行时选定可用的图标字体；都没有时返回空串，调用方退化为不画
const wchar_t* IconFace() {
  static const std::wstring face = [] {
    for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
      HDC dc = ::GetDC(nullptr);
      LOGFONTW lf{};
      lf.lfHeight = -16;
      wcscpy_s(lf.lfFaceName, name);
      HFONT f = ::CreateFontIndirectW(&lf);
      std::wstring actual;
      if (f) {
        HGDIOBJ old = ::SelectObject(dc, f);
        wchar_t buf[64] = {};
        ::GetTextFaceW(dc, 64, buf);
        actual = buf;
        ::SelectObject(dc, old);
        ::DeleteObject(f);
      }
      ::ReleaseDC(nullptr, dc);
      if (::CompareStringOrdinal(actual.c_str(), -1, name, -1, TRUE) == CSTR_EQUAL) return actual;
    }
    return std::wstring();
  }();
  return face.c_str();
}

// 按像素高度缓存图标字体；不可用时返回 nullptr
HFONT IconFont(int pixelHeight) {
  static std::map<int, HFONT> cache;
  auto it = cache.find(pixelHeight);
  if (it != cache.end()) return it->second;

  const wchar_t* face = IconFace();
  HFONT f = nullptr;
  if (*face) {
    LOGFONTW lf{};
    lf.lfHeight = -pixelHeight;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    // 灰度抗锯齿：彩色亚像素会让图标边缘出现彩边
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    f = ::CreateFontIndirectW(&lf);
  }
  cache[pixelHeight] = f;
  return f;
}

// 图标字形码点，取自 MDL2 字表
constexpr wchar_t GlyphFor(IconId id) {
  switch (id) {
    case IconId::kRefresh: return 0xE72C;      // Refresh
    case IconId::kExtract: return 0xE8DE;      // MoveToFolder
    case IconId::kExtractAll: return 0xE896;   // Download
    case IconId::kAdd: return 0xE710;          // Add
    case IconId::kTrash: return 0xE74D;        // Delete
    case IconId::kArrowBack: return 0xE72B;    // Back
    case IconId::kChevron: return 0xE76C;      // ChevronRight
    case IconId::kChevronDown: return 0xE70D;  // ChevronDown
    case IconId::kChevronUp: return 0xE70E;    // ChevronUp
    case IconId::kHome: return 0xE80F;         // Home
    case IconId::kFolder: return 0xE8B7;       // Folder
    case IconId::kFile: return 0xE8A5;         // Document
    case IconId::kSearch: return 0xE721;       // Search
  }
  return 0;
}

// 字形墨迹框：宽高与原点偏移都按字号归一，便于任意尺寸复用
struct InkBox {
  double rw = 1.0;  // 墨迹宽 / 字号
  double rh = 1.0;  // 墨迹高 / 字号
  double ox = 0.0;  // 墨迹左边界相对字符原点
  double oy = 0.0;  // 墨迹上边界相对基线
};

// 以 64px 参考字号取字形墨迹；字体没有该字形时退回整块 em 框
InkBox MeasureInk(wchar_t glyph) {
  static std::map<wchar_t, InkBox> cache;
  auto it = cache.find(glyph);
  if (it != cache.end()) return it->second;

  constexpr int kRef = 64;
  InkBox box;
  HDC dc = ::CreateCompatibleDC(nullptr);
  HFONT font = IconFont(kRef);
  if (dc && font) {
    HGDIOBJ old = ::SelectObject(dc, font);
    MAT2 mat{};
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    GLYPHMETRICS gm{};
    if (::GetGlyphOutlineW(dc, glyph, GGO_BITMAP, &gm, 0, nullptr, &mat) != GDI_ERROR &&
        gm.gmBlackBoxX && gm.gmBlackBoxY) {
      box.rw = static_cast<double>(gm.gmBlackBoxX) / kRef;
      box.rh = static_cast<double>(gm.gmBlackBoxY) / kRef;
      box.ox = static_cast<double>(gm.gmptGlyphOrigin.x) / kRef;
      box.oy = static_cast<double>(gm.gmptGlyphOrigin.y) / kRef;
    }
    ::SelectObject(dc, old);
  }
  if (dc) ::DeleteDC(dc);
  cache[glyph] = box;
  return box;
}

std::map<std::wstring, HICON> g_shellIcons;

}  // namespace

void DrawGlyph(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  const wchar_t glyph = GlyphFor(id);
  if (!glyph) return;

  // 按墨迹最长边贴合目标矩形，避免字形撑出矩形压到相邻文字
  const InkBox ink = MeasureInk(glyph);
  const int side = w < h ? w : h;
  const double ratio = ink.rw > ink.rh ? ink.rw : ink.rh;
  const int fontSize = static_cast<int>(std::lround(side / ratio));
  HFONT font = IconFont(fontSize > 0 ? fontSize : 1);
  if (!font) return;

  COLORREF inkColor = color;
  if (!enabled) {
    const int y = (GetRValue(color) * 30 + GetGValue(color) * 59 + GetBValue(color) * 11) / 100;
    const int m = 255 - (255 - y) * 45 / 100;
    inkColor = RGB(m, m, m);
  }

  // 让墨迹框中心落在目标矩形中心
  const double inkW = ink.rw * fontSize;
  const double inkH = ink.rh * fontSize;
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  const int x = static_cast<int>(std::lround(cx - ink.ox * fontSize - inkW / 2));
  const int y = static_cast<int>(std::lround(cy + ink.oy * fontSize - inkH / 2));

  HFONT oldFont = static_cast<HFONT>(::SelectObject(dc, font));
  const int oldMode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF oldColor = ::SetTextColor(dc, inkColor);
  const UINT oldAlign = ::SetTextAlign(dc, TA_LEFT | TA_BASELINE);

  ::TextOutW(dc, x, y, &glyph, 1);

  ::SetTextAlign(dc, oldAlign);
  ::SetTextColor(dc, oldColor);
  ::SetBkMode(dc, oldMode);
  ::SelectObject(dc, oldFont);
}

// 直接拿 HICON，绘制时用 DrawIconEx 缩放。
// 系统图像列表的索引在不同 shell 版本下不总是能画出来（压缩包内无实体文件时尤其如此）。
HICON ShellIconFor(const std::wstring& path, bool isDir) {
  const std::wstring key = isDir ? L"<dir>" : FileExtension(path);
  auto it = g_shellIcons.find(key);
  if (it != g_shellIcons.end()) return it->second;

  SHFILEINFOW sfi{};
  const DWORD attrib = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  const DWORD flags = SHGFI_ICON | SHGFI_SMALLICON |
                      (isDir ? SHGFI_USEFILEATTRIBUTES
                             : (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                                    ? 0
                                    : SHGFI_USEFILEATTRIBUTES));
  ::SHGetFileInfoW(isDir ? L"dir" : path.c_str(), attrib, &sfi, sizeof(sfi), flags);
  g_shellIcons[key] = sfi.hIcon;
  return sfi.hIcon;
}

void ClearShellIconCache() {
  for (auto& kv : g_shellIcons) {
    if (kv.second) ::DestroyIcon(kv.second);
  }
  g_shellIcons.clear();
}

}  // namespace AUI
