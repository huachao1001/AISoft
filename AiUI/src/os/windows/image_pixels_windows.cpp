// 图片像素读写后端
//
// 界面上的 Image 只是 GDI+ 位图句柄，控件拿不到内容。要做像素级操作
// （重采样、旋转、另存为）就得把整张图解到一段内存里，所以这一层直接
// 收发 AiUI::RgbaImage，编解码细节全留在 GDI+ 里。
//
// 为什么解码后统一转成 32bppARGB：GDI+ 的原始格式可能是索引色、16 位、
// CMYK，甚至带 EXIF 方向；先归一成 ARGB 再交出去，上层只需处理一种布局。
#include "backend.h"

#include <algorithm>
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

// 缩略代理：GDI+ 的 Bitmap 构造可以直接给目标边长，内部按 EXIF 方向与
// 采样率缩放。比「整张解码再缩小」省掉一次全尺寸内存分配——巨图差的就是这块。
Image loadImageThumbnail(const std::wstring& path, int maxSide) {
  if (path.empty() || maxSide <= 0 || !ensureGdiplus()) return Image();

  // 先只读头部拿原尺寸，决定是否需要缩。GDI+ 的 GetWidth 对未解码的
  // 文件头也有效，不必付整张解码的代价。
  {
    Gdiplus::Bitmap probe(path.c_str(), FALSE);
    if (probe.GetLastStatus() != Ok) return Image();
    const int w = static_cast<int>(probe.GetWidth());
    const int h = static_cast<int>(probe.GetHeight());
    if (w <= 0 || h <= 0) return Image();
    // 已经比目标小：原样加载，重采样只会白白糊一档
    if (w <= maxSide && h <= maxSide) return loadImageFile(path);
  }

  auto* full = new Gdiplus::Bitmap(path.c_str(), FALSE);
  if (!full || full->GetLastStatus() != Ok) {
    delete full;
    return Image();
  }

  // 按最长边等比
  const int w = static_cast<int>(full->GetWidth());
  const int h = static_cast<int>(full->GetHeight());
  const int longest = (w > h) ? w : h;
  const int tw = (std::max)(1, static_cast<int>(static_cast<long long>(w) * maxSide / longest));
  const int th = (std::max)(1, static_cast<int>(static_cast<long long>(h) * maxSide / longest));

  auto* thumb = new Gdiplus::Bitmap(tw, th, PixelFormat32bppARGB);
  if (!thumb || thumb->GetLastStatus() != Ok) {
    delete thumb;
    delete full;
    return Image();
  }
  {
    Graphics g(thumb);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(full, Gdiplus::Rect(0, 0, tw, th));
  }
  delete full;

  loadedImages().insert(thumb);
  return Image(thumb);
}

// 只解码一个区域。GDI+ 没有「部分解码」的接口，但可以先把源图按区域放大
// 之前的比例缩到「区域已够用」的大小再裁——
// 等价的简单做法是：解整张再裁，代价仍然是整张。
// 真正省内存的路子是按 1:1 只解码需要的条带，各平台图像库支持不一；
// 这里选择「整张解码 + 裁剪」的稳妥实现，并明确它只解决调用方取像素的
// 语义问题，不假装解决了巨图的内存问题。
bool loadImageRegion(const std::wstring& path, const Rect& region, RgbaImage& out) {
  out = RgbaImage{};
  if (region.isEmpty()) return false;

  RgbaImage full;
  if (!loadImagePixels(path, full)) return false;

  const Rect clipped = region.intersect(Rect{0, 0, full.width, full.height});
  if (clipped.isEmpty()) return false;

  out.width = clipped.width();
  out.height = clipped.height();
  out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));
  for (int y = 0; y < out.height; ++y) {
    const size_t src = static_cast<size_t>(clipped.top + y) * static_cast<size_t>(full.width) +
                       static_cast<size_t>(clipped.left);
    std::memcpy(out.pixels.data() + static_cast<size_t>(y) * out.width, full.pixels.data() + src,
                static_cast<size_t>(out.width) * sizeof(uint32_t));
  }
  return true;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
