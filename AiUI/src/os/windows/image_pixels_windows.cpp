// 图片像素读写后端
//
// 界面上的 Image 只是 GDI+ 位图句柄，控件拿不到内容。要做像素级操作
// （重采样、旋转、另存为）就得把整张图解到一段内存里，所以这一层直接
// 收发 AiUI::RgbaImage，编解码细节全留在 GDI+ 里。
//
// 为什么解码后统一转成 32bppARGB：GDI+ 的原始格式可能是索引色、16 位、
// CMYK，甚至带 EXIF 方向；先归一成 ARGB 再交出去，上层只需处理一种布局。
#include "backend.h"

#include <cstring>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {

using namespace Gdiplus;

namespace {

// 按 MIME 类型找编码器 CLSID；GDI+ 只按这个方式暴露编码器
bool encoderFor(const wchar_t* mime, CLSID& out) {
  UINT count = 0;
  UINT size = 0;
  if (GetImageEncodersSize(&count, &size) != Ok || size == 0) return false;

  std::vector<uint8_t> buffer(size);
  auto* infos = reinterpret_cast<ImageCodecInfo*>(buffer.data());
  if (GetImageEncoders(count, size, infos) != Ok) return false;

  for (UINT i = 0; i < count; ++i) {
    if (infos[i].MimeType && ::wcscmp(infos[i].MimeType, mime) == 0) {
      out = infos[i].Clsid;
      return true;
    }
  }
  return false;
}

// 把任意像素格式的源图整块拷到 32bppARGB 画布上。
// 用 SourceCopy + 最近邻：这一步只做格式转换，不该有任何混色或重采样。
Gdiplus::Bitmap* convertToArgb(Gdiplus::Image& source) {
  const int w = static_cast<int>(source.GetWidth());
  const int h = static_cast<int>(source.GetHeight());
  if (w <= 0 || h <= 0) return nullptr;

  auto* canvas = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
  if (!canvas || canvas->GetLastStatus() != Ok) {
    delete canvas;
    return nullptr;
  }

  Graphics g(canvas);
  g.SetCompositingMode(CompositingModeSourceCopy);
  g.SetInterpolationMode(InterpolationModeNearestNeighbor);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);
  g.DrawImage(&source, Gdiplus::Rect(0, 0, w, h));
  return canvas;
}

// 逐行读出像素；GDI+ 的 stride 可能大于 width*4，必须按行取
bool readPixels(Gdiplus::Bitmap& bitmap, RgbaImage& out) {
  const int w = static_cast<int>(bitmap.GetWidth());
  const int h = static_cast<int>(bitmap.GetHeight());
  if (w <= 0 || h <= 0) return false;

  Gdiplus::Rect lockRect(0, 0, w, h);
  Gdiplus::BitmapData data{};
  if (bitmap.LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
    return false;
  }

  out.width = w;
  out.height = h;
  out.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    const auto* src = static_cast<const uint8_t*>(data.Scan0) +
                      static_cast<ptrdiff_t>(y) * data.Stride;
    std::memcpy(out.pixels.data() + static_cast<size_t>(y) * w, src,
                static_cast<size_t>(w) * sizeof(uint32_t));
  }
  bitmap.UnlockBits(&data);
  return true;
}

// 反向：把 ARGB 像素塞进一张新位图
bool writePixels(Gdiplus::Bitmap& bitmap, const RgbaImage& image) {
  const int w = static_cast<int>(bitmap.GetWidth());
  const int h = static_cast<int>(bitmap.GetHeight());
  if (w <= 0 || h <= 0 || image.width != w || image.height != h) return false;

  Gdiplus::Rect lockRect(0, 0, w, h);
  Gdiplus::BitmapData data{};
  if (bitmap.LockBits(&lockRect, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok) {
    return false;
  }
  for (int y = 0; y < h; ++y) {
    auto* dst = static_cast<uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
    std::memcpy(dst, image.pixels.data() + static_cast<size_t>(y) * w,
                static_cast<size_t>(w) * sizeof(uint32_t));
  }
  bitmap.UnlockBits(&data);
  return true;
}

}  // namespace

bool loadImagePixels(const std::wstring& path, RgbaImage& out) {
  out = RgbaImage{};
  if (path.empty() || !ensureGdiplus()) return false;

  Gdiplus::Bitmap source(path.c_str(), FALSE);
  if (source.GetLastStatus() != Ok) return false;

  Gdiplus::Bitmap* argb = convertToArgb(source);
  if (!argb) return false;
  const bool ok = readPixels(*argb, out);
  delete argb;
  if (!ok) out = RgbaImage{};
  return ok;
}

bool saveImagePixels(const std::wstring& path, const RgbaImage& image, ImageFormat format) {
  if (path.empty() || image.isEmpty() || !ensureGdiplus()) return false;

  const wchar_t* mime = L"image/png";
  switch (format) {
    case ImageFormat::kJpeg: mime = L"image/jpeg"; break;
    case ImageFormat::kBmp: mime = L"image/bmp"; break;
    default: break;
  }

  CLSID clsid{};
  if (!encoderFor(mime, clsid)) return false;

  // 先用 LockBits 直接写入像素，省掉一次中间转换
  Gdiplus::Bitmap bitmap(image.width, image.height, PixelFormat32bppARGB);
  if (bitmap.GetLastStatus() != Ok) return false;

  if (!writePixels(bitmap, image)) return false;

  // JPEG 没有 alpha，质量给高一点避免另存一次就肉眼可见地糊
  EncoderParameters params{};
  params.Count = 1;
  ULONG quality = 92;
  if (format == ImageFormat::kJpeg) {
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;
  }

  const Status status = format == ImageFormat::kJpeg
                            ? bitmap.Save(path.c_str(), &clsid, &params)
                            : bitmap.Save(path.c_str(), &clsid, nullptr);
  return status == Ok;
}

Image imageFromPixels(const RgbaImage& image) {
  if (image.isEmpty() || !ensureGdiplus()) return Image();

  auto* bitmap = new Gdiplus::Bitmap(image.width, image.height, PixelFormat32bppARGB);
  if (!bitmap || bitmap->GetLastStatus() != Ok) {
    delete bitmap;
    return Image();
  }
  if (!writePixels(*bitmap, image)) {
    delete bitmap;
    return Image();
  }
  loadedImages().insert(bitmap);
  return Image(bitmap);
}
}  // namespace windows
}  // namespace os
}  // namespace AiUI
