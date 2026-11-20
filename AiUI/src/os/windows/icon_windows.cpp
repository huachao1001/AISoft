// 图标与位图后端
//
// 矢量图标取自 Segoe Fluent Icons / Segoe MDL2 Assets，用 GDI+ 绘制并按
// 墨迹框贴合目标矩形，任意尺寸都居中且不越界。
// 位图由 GDI+ 解码，覆盖 png/jpeg/gif/bmp/ico/tiff。
#include "backend.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {

using namespace Gdiplus;

namespace {

// 运行时选定可用的图标字体；都没有时返回空串，退化为不画
const wchar_t* iconFace() {
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

// 按像素尺寸缓存图标字体
std::map<int, Gdiplus::Font*>& iconFontCache() {
  static std::map<int, Gdiplus::Font*> cache;
  return cache;
}

Gdiplus::Font* iconFont(int pixelSize) {
  auto& cache = iconFontCache();
  auto it = cache.find(pixelSize);
  if (it != cache.end()) return it->second;

  Gdiplus::Font* font = nullptr;
  if (*iconFace()) {
    font = new Gdiplus::Font(iconFace(), static_cast<REAL>(pixelSize), FontStyleRegular,
                             UnitPixel);
  }
  cache[pixelSize] = font;
  return font;
}

// 图标字形码点，取自 MDL2 字表
constexpr wchar_t glyphFor(IconId id) {
  switch (id) {
    case IconId::kRefresh: return 0xE72C;
    case IconId::kExtract: return 0xE8DE;
    case IconId::kExtractAll: return 0xE896;
    case IconId::kAdd: return 0xE710;
    case IconId::kTrash: return 0xE74D;
    case IconId::kSave: return 0xE74E;
    case IconId::kSaveAs: return 0xE792;
    case IconId::kArrowBack: return 0xE72B;
    case IconId::kChevron: return 0xE76C;
    case IconId::kChevronDown: return 0xE70D;
    case IconId::kChevronUp: return 0xE70E;
    case IconId::kHome: return 0xE80F;
    case IconId::kFolder: return 0xE8B7;
    case IconId::kFile: return 0xE8A5;
    case IconId::kSearch: return 0xE721;
    case IconId::kCheck: return 0xE73E;
    // 眼睛与眼睛加斜线：MDL2 的 View / Hide，线条粗细与其它图标一致
    case IconId::kEye: return 0xE7B3;
    case IconId::kEyeOff: return 0xED1A;
    // 缩放与旋转：ZoomIn / ZoomOut / FitPage / Rotate / Redo 的对应字形
    case IconId::kZoomIn: return 0xE8A3;
    case IconId::kZoomOut: return 0xE71F;
    // 实际大小：MDL2 的 AspectRatio（方框里两条竖线），与放大镜区分开
    case IconId::kZoomActual: return 0xE799;
    case IconId::kFitWindow: return 0xE9A6;
    case IconId::kRotateLeft: return 0xE7AD;
    case IconId::kRotateRight: return 0xE1CD;
    case IconId::kImage: return 0xEB9F;
    // 全屏：MDL2 的 FullScreen（四角向外）
    case IconId::kFullscreen: return 0xE740;
    // 修复 / 卸载 / 挽留：MDL2 的 Repair / Delete / Heart
    case IconId::kRepair: return 0xE90F;
    case IconId::kUninstall: return 0xE74D;
    case IconId::kHeart: return 0xEB51;
    case IconId::kMinimize: return 0xE921;
    case IconId::kMaximize: return 0xE922;
    case IconId::kRestore: return 0xE923;
    case IconId::kClose: return 0xE8BB;
    default: return 0;
  }
}

// 字形在参考字号下的包围盒：宽高与原点偏移都按字号归一，任意尺寸可复用
struct InkBox {
  double ratioW = 1.0;
  double ratioH = 1.0;
  double originX = 0.0;  // 从笔位到墨迹左边界
  double originY = 0.0;  // 从基线到墨迹上边界，向上为正
};

InkBox measureInk(wchar_t glyph) {
  static std::map<wchar_t, InkBox> cache;
  auto it = cache.find(glyph);
  if (it != cache.end()) return it->second;

  constexpr int kRef = 64;
  InkBox box;
  Gdiplus::Font* font = iconFont(kRef);
  if (font) {
    // GraphicsPath 给出的是含曲线的精确包围盒，比 GGO_BITMAP 的整数格更可靠
    FontFamily family;
    font->GetFamily(&family);
    GraphicsPath path;
    if (path.AddString(&glyph, 1, &family, FontStyleRegular, static_cast<REAL>(kRef),
                       Gdiplus::PointF(0.0f, 0.0f), nullptr) == Ok) {
      RectF bounds;
      if (path.GetBounds(&bounds) == Ok && bounds.Width > 0.0f && bounds.Height > 0.0f) {
        box.ratioW = bounds.Width / kRef;
        box.ratioH = bounds.Height / kRef;
        box.originX = bounds.X / kRef;
        box.originY = bounds.Y / kRef;
      }
    }
  }
  cache[glyph] = box;
  return box;
}

// HICON → 32bpp ARGB 位图。
// Bitmap::FromHICON 对只有 AND mask 的旧式图标会把透明区画成黑色，
// 这里手工合成 alpha：图标自带 alpha 就直接用，否则按 mask 判断透明。
Gdiplus::Bitmap* bitmapFromIcon(HICON icon) {
  ICONINFO info{};
  if (!::GetIconInfo(icon, &info)) return nullptr;

  auto releaseBitmaps = [&info] {
    if (info.hbmColor) ::DeleteObject(info.hbmColor);
    if (info.hbmMask) ::DeleteObject(info.hbmMask);
  };

  BITMAP bm{};
  if (!info.hbmColor || !::GetObject(info.hbmColor, sizeof(bm), &bm) || bm.bmWidth <= 0 ||
      bm.bmHeight <= 0) {
    releaseBitmaps();
    return nullptr;
  }

  const int width = bm.bmWidth;
  const int height = bm.bmHeight;
  const size_t count = static_cast<size_t>(width) * height;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // 负高度 = 自上而下
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  std::vector<uint32_t> pixels(count, 0);
  HDC dc = ::GetDC(nullptr);
  const int got = ::GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bi, DIB_RGB_COLORS);

  std::vector<uint32_t> mask;
  if (info.hbmMask) {
    mask.assign(count, 0);
    ::GetDIBits(dc, info.hbmMask, 0, height, mask.data(), &bi, DIB_RGB_COLORS);
  }
  ::ReleaseDC(nullptr, dc);
  releaseBitmaps();

  if (got != height) return nullptr;

  // 有非零 alpha 说明图标自带透明信息；否则用 mask 的白色位当透明
  bool hasAlpha = false;
  for (uint32_t px : pixels) {
    if ((px >> 24) != 0) {
      hasAlpha = true;
      break;
    }
  }
  if (!hasAlpha) {
    for (size_t i = 0; i < count; ++i) {
      const bool transparent = i < mask.size() && mask[i] != 0;
      pixels[i] = transparent ? 0u : (pixels[i] | 0xFF000000u);
    }
  }

  auto* bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
  Gdiplus::Rect lockRect(0, 0, width, height);
  Gdiplus::BitmapData data{};
  if (bitmap->LockBits(&lockRect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) !=
      Ok) {
    delete bitmap;
    return nullptr;
  }
  for (int y = 0; y < height; ++y) {
    auto* dst = static_cast<uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
    memcpy(dst, pixels.data() + static_cast<size_t>(y) * width,
           static_cast<size_t>(width) * sizeof(uint32_t));
  }
  bitmap->UnlockBits(&data);
  return bitmap;
}

}  // namespace

// 系统图标缓存：键为 "<dir>" 或扩展名，句柄在本进程内复用
std::map<std::wstring, Image>& fileIconCache() {
  static std::map<std::wstring, Image> cache;
  return cache;
}

// 显式加载的位图集合：退出时统一释放
std::set<Gdiplus::Bitmap*>& loadedImages() {
  static std::set<Gdiplus::Bitmap*> set;
  return set;
}

void drawIconGlyph(Gdiplus::Graphics* g, IconId id, const Rect& rc, Color color) {
  if (!g) return;
  const wchar_t glyph = glyphFor(id);
  if (!glyph) return;
  const int w = rc.width();
  const int h = rc.height();
  if (w <= 0 || h <= 0) return;

  // 按包围盒最长边贴合目标矩形，避免字形撑出矩形压到相邻文字
  const InkBox ink = measureInk(glyph);
  const int side = w < h ? w : h;
  const double ratio = ink.ratioW > ink.ratioH ? ink.ratioW : ink.ratioH;
  const int fontSize = static_cast<int>(std::lround(side / (ratio > 0 ? ratio : 1.0)));
  Gdiplus::Font* font = iconFont(fontSize > 0 ? fontSize : 1);
  if (!font) return;

  // 反转笔位：包围盒中心对准目标矩形中心
  const double inkW = ink.ratioW * fontSize;
  const double inkH = ink.ratioH * fontSize;
  const Point center = rc.center();
  const REAL inkLeft = static_cast<REAL>(center.x - inkW / 2.0);
  const REAL inkTop = static_cast<REAL>(center.y - inkH / 2.0);
  const REAL x = static_cast<REAL>(std::lround(inkLeft - ink.originX * fontSize));
  const REAL y = static_cast<REAL>(std::lround(inkTop - ink.originY * fontSize));

  SolidBrush brush(
      Gdiplus::Color(colorAlpha(color), colorRed(color), colorGreen(color), colorBlue(color)));
  g->SetTextRenderingHint(TextRenderingHintAntiAlias);
  g->DrawString(&glyph, 1, font, Gdiplus::PointF(x, y), &brush);
}

void releaseIconCaches() {
  for (auto& kv : fileIconCache()) {
    delete static_cast<Gdiplus::Bitmap*>(kv.second.handle());
  }
  fileIconCache().clear();

  for (Gdiplus::Bitmap* bmp : loadedImages()) delete bmp;
  loadedImages().clear();

  for (auto& kv : iconFontCache()) delete kv.second;
  iconFontCache().clear();
}

Image loadImageFile(const std::wstring& path) {
  if (path.empty() || !ensureGdiplus()) return Image();
  auto* bmp = new Gdiplus::Bitmap(path.c_str(), FALSE);
  if (!bmp || bmp->GetLastStatus() != Ok) {
    delete bmp;
    return Image();
  }
  loadedImages().insert(bmp);
  return Image(bmp);
}

Size imagePixelSize(const Image& image) {
  Gdiplus::Bitmap* bmp = asBitmap(image);
  if (!bmp) return Size{};
  return Size{static_cast<int>(bmp->GetWidth()), static_cast<int>(bmp->GetHeight())};
}

void releaseLoadedImage(Image image) {
  auto* bmp = asBitmap(image);
  if (!bmp) return;
  // 缓存图标由 releaseIconCaches 统一释放
  if (loadedImages().erase(bmp)) delete bmp;
}

Gdiplus::Bitmap* asBitmap(const Image& image) {
  return static_cast<Gdiplus::Bitmap*>(image.handle());
}

Image imageFromIcon(HICON icon) {
  // bitmapFromIcon 在匿名命名空间里；这里包一层给窗口后端用
  if (!icon) return Image();
  Gdiplus::Bitmap* bmp = bitmapFromIcon(icon);
  if (!bmp) return Image();
  loadedImages().insert(bmp);
  return Image(bmp);
}

HICON loadAppIcon(int pixelSize) {
  wchar_t path[MAX_PATH] = {};
  if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return nullptr;

  // PrivateExtractIconsW 能按指定边长取图：任务栏预览、Alt+Tab 与系统标题栏
  // 各自要不同档位，ExtractIconEx 只按索引给，尺寸对不上就会被系统拉伸。
  const int size = pixelSize > 0 ? pixelSize : ::GetSystemMetrics(SM_CXICON);
  HICON icon = nullptr;
  UINT id = 0;
  if (::PrivateExtractIconsW(path, 0, size, size, &icon, &id, 1, 0) == 0 || !icon) {
    // 取不到时退回第一张；系统会自己缩放到需要的尺寸
    if (::ExtractIconExW(path, 0, &icon, nullptr, 1) == 0 || !icon) return nullptr;
  }
  return icon;
}

}  // namespace windows
}  // namespace os

// ── 对外接口 ──

Image fileIcon(const std::wstring& path, bool isDirectory, int pixelSize) {
  // 系统图标的原生档位。取「不小于需求的最小档」：多出来的交给 GDI+ 缩，
  // 缩是抗锯齿重采样；不够再放大就只能靠插值猜像素，边缘立刻发虚。
  static const int kIconLadder[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
  const int want = pixelSize > 0 ? pixelSize : 16;
  int size = 256;
  for (const int step : kIconLadder) {
    if (step >= want) {
      size = step;
      break;
    }
  }

  // 目录共用一份图标，文件按扩展名缓存，避免每个文件都查一次 shell。
  // 同一目标不同尺寸是不同位图，键必须带上档位，否则 16 的那张会顶替 48 的。
  std::wstring key = isDirectory ? L"<dir>" : path;
  if (!isDirectory) {
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) key = path.substr(dot);
  }
  key += L'@';
  key += std::to_wstring(size);

  auto& cache = os::windows::fileIconCache();
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;

  Image image;

  // 一、文件自带图标（exe / dll / ico）按需要的边长精确取。
  //     PrivateExtractIconsW 会挑最接近的原生档位再缩放，aizip.ico 里有
  //     16~256 十档，要 48 就给 48，不必从 16×16 拉大。
  if (!isDirectory) {
    HICON icon = nullptr;
    UINT extracted = 0;
    if (::PrivateExtractIconsW(path.c_str(), 0, size, size, &icon, &extracted, 1, 0) > 0 &&
        icon) {
      if (Gdiplus::Bitmap* bmp = os::windows::bitmapFromIcon(icon)) image = Image(bmp);
      ::DestroyIcon(icon);
    }
  }

  // 二、没有内嵌图标的（目录、文档）只能问 shell 要关联图标。shell 只提供
  //     小/大两档（16 / 32），再大的档位也是它自己放大出来的，问它不如自己
  //     取大档再缩。所以这里按尺寸选一档，不做更细的追问。
  if (!image.isValid()) {
    SHFILEINFOW info{};
    const DWORD attributes = isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    DWORD flags = SHGFI_ICON | (size > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    const bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (isDirectory || !exists) flags |= SHGFI_USEFILEATTRIBUTES;
    ::SHGetFileInfoW(isDirectory ? L"dir" : path.c_str(), attributes, &info, sizeof(info), flags);
    if (info.hIcon) {
      if (Gdiplus::Bitmap* bmp = os::windows::bitmapFromIcon(info.hIcon)) image = Image(bmp);
      ::DestroyIcon(info.hIcon);
    }
  }

  cache[key] = image;
  return image;
}


void releaseFileIcons() { os::windows::releaseIconCaches(); }

}  // namespace AiUI
