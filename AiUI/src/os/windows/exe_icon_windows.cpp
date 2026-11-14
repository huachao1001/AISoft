// 改写 exe 的图标资源
//
// 卸载程序是安装器在运行期复制出来的，图标却早在构建期就编进了资源段，
// 所以只能在复制之后改写。这里向 shell 要系统的回收站图标，按几个常用
// 档位取出，再以标准 .ico 结构写进目标 exe 的 RT_ICON / RT_GROUP_ICON。
//
// 为什么不在安装器里带一份 ico：图标是系统资产，随 Windows 版本变化
// （新视觉、深色模式各不同）。向 shell 要当前系统的那个，卸载程序看起来
// 就与资源管理器一致，也不必给每个应用多带一份资源。
//
// 为什么全程只用 GDI 与系统图标提取、不碰 GDI+：这个函数在安装线程上跑，
// 而 UI 线程同时在用 GDI+ 绘制窗口。GDI+ 的初始化/关闭与位图缓存都是进程级
// 的单例，从别的线程去动它会让正在绘制的窗口直接崩掉。
#include "backend.h"
#include "windows_backend.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {
namespace {

struct IconImage {
  int size = 0;
  std::vector<uint8_t> data;  // RT_ICON 的原始内容
};

void putU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

// 收集同名资源
BOOL CALLBACK enumIconGroups(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  (void)module;
  (void)type;
  auto* names = reinterpret_cast<std::vector<std::wstring>*>(param);
  // 整数 id 用 MAKEINTRESOURCE 表示，指针高位为零
  if (reinterpret_cast<ULONG_PTR>(name) >> 16) names->push_back(name);
  return TRUE;
}

// HICON → RT_ICON 需要的 DIB（BITMAPINFOHEADER + BGRA + AND 掩码）。
//
// 布局要点：
//   - 头里的高度写成两倍：上半是颜色位图，下半是 AND 掩码。
//   - 掩码每行按 4 字节对齐，长度按位算；少算会读串，多算图标会花。
//   - 现代图标自带 alpha，掩码全 0 即可（掩码不再参与透明判断），
//     但结构上必须留着，缺了它系统会把图像读歪。
std::vector<uint8_t> iconToDib(HICON icon, int& width, int& height) {
  width = 0;
  height = 0;
  if (!icon) return {};

  ICONINFO info{};
  if (!::GetIconInfo(icon, &info)) return {};

  BITMAP bm{};
  const bool hasColor =
      info.hbmColor && ::GetObject(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0;
  if (!hasColor) {
    if (info.hbmColor) ::DeleteObject(info.hbmColor);
    if (info.hbmMask) ::DeleteObject(info.hbmMask);
    return {};
  }

  width = bm.bmWidth;
  height = bm.bmHeight;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // 负高度 = 自上而下
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  std::vector<uint32_t> pixels(count, 0);
  std::vector<uint32_t> mask(count, 0);

  HDC dc = ::GetDC(nullptr);
  const int got = ::GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bi, DIB_RGB_COLORS);
  if (info.hbmMask) {
    ::GetDIBits(dc, info.hbmMask, 0, height, mask.data(), &bi, DIB_RGB_COLORS);
  }
  ::ReleaseDC(nullptr, dc);

  ::DeleteObject(info.hbmColor);
  if (info.hbmMask) ::DeleteObject(info.hbmMask);

  if (got != height) {
    width = 0;
    height = 0;
    return {};
  }

  // 老式图标没有 alpha 通道，只能用掩码判断透明：掩码为白的位置透明。
  bool hasAlpha = false;
  for (const uint32_t px : pixels) {
    if ((px >> 24) != 0) {
      hasAlpha = true;
      break;
    }
  }
  if (!hasAlpha) {
    for (size_t i = 0; i < count; ++i) {
      pixels[i] = mask[i] != 0 ? 0u : (pixels[i] | 0xFF000000u);
    }
  }

  // 掩码按 4 字节对齐：每行字节数 = ((width + 31) / 32) * 4
  const size_t maskStride = ((static_cast<size_t>(width) + 31) / 32) * 4;
  std::vector<uint8_t> out;
  out.resize(sizeof(BITMAPINFOHEADER) + count * sizeof(uint32_t) + maskStride * height);

  auto* header = reinterpret_cast<BITMAPINFOHEADER*>(out.data());
  header->biSize = sizeof(BITMAPINFOHEADER);
  header->biWidth = width;
  header->biHeight = height * 2;  // 颜色 + 掩码
  header->biPlanes = 1;
  header->biBitCount = 32;
  header->biCompression = BI_RGB;
  header->biSizeImage = static_cast<DWORD>(count * sizeof(uint32_t));

  std::memcpy(out.data() + sizeof(BITMAPINFOHEADER), pixels.data(), count * sizeof(uint32_t));
  // 掩码区保持全 0：alpha 已经表达了透明度
  return out;
}

// 系统图标在哪个文件、是第几个资源。回收站图标在 imageres.dll 里，
// 用「文件 + 索引」才能按任意边长重新提取，拿到清晰的大图。
struct StockIconLocation {
  std::wstring path;
  int index = -1;
};

StockIconLocation locateStockIcon(SHSTOCKICONID id) {
  StockIconLocation location;
  SHSTOCKICONINFO info{};
  info.cbSize = sizeof(info);
  if (SUCCEEDED(::SHGetStockIconInfo(id, SHGSI_ICONLOCATION, &info)) && info.szPath[0] != L'\0' &&
      info.iIcon > 0) {
    location.path = info.szPath;
    location.index = info.iIcon;
  }
  return location;
}

// 按指定边长取一张图标
HICON extractIconAtSize(const StockIconLocation& location, int size) {
  if (location.path.empty() || location.index < 0) return nullptr;

  HICON icon = nullptr;
  UINT extracted = 0;
  if (::PrivateExtractIconsW(location.path.c_str(), location.index, size, size, &icon, &extracted, 1,
                             0) == 0 ||
      !icon) {
    return nullptr;
  }
  return icon;
}

// 取系统图标的各个档位。
//
// 优先按「文件 + 索引」逐档重新提取：系统会从图标资源里挑最接近的原始档位
// 再缩放，比拿一张 32x32 硬放大清楚得多。全部失败时退回 SHGSI_ICON 给的
// 那一张，至少确保资源表里有图标。
std::vector<IconImage> loadStockIconImages(SHSTOCKICONID id) {
  std::vector<IconImage> images;
  const StockIconLocation location = locateStockIcon(id);

  // 常用档位：列表、标题栏、桌面、高分屏、超大图标
  for (const int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
    HICON icon = extractIconAtSize(location, size);
    if (!icon) continue;

    int width = 0;
    int height = 0;
    std::vector<uint8_t> dib = iconToDib(icon, width, height);
    ::DestroyIcon(icon);
    if (dib.empty()) continue;

    IconImage image;
    image.size = width;  // 目录项按实际边长写，系统给的可能略小于请求值
    image.data = std::move(dib);
    images.push_back(std::move(image));
  }

  if (!images.empty()) return images;

  // 兜底：SHGSI_ICON 直接给一张现成的
  SHSTOCKICONINFO info{};
  info.cbSize = sizeof(info);
  if (SUCCEEDED(::SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &info)) && info.hIcon) {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> dib = iconToDib(info.hIcon, width, height);
    ::DestroyIcon(info.hIcon);
    if (!dib.empty()) {
      IconImage image;
      image.size = width;
      image.data = std::move(dib);
      images.push_back(std::move(image));
    }
  }
  return images;
}

}  // namespace

std::wstring WindowsBackend::setExecutableSystemIcon(const std::wstring& exePath,
                                                     SystemIcon icon) {
  if (exePath.empty()) return L"目标程序为空";

  SHSTOCKICONID stockId = SIID_RECYCLER;
  switch (icon) {
    case SystemIcon::kRecycleBin:
      stockId = SIID_RECYCLER;
      break;
  }

  // shell 的图标查询走 COM。这是线程本地状态：本线程可能已经初始化过
  // （模式不一定是 STA），那就直接用，退出时也不要替别人反初始化。
  const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool ownCom = SUCCEEDED(comHr);

  std::vector<IconImage> images = loadStockIconImages(stockId);

  if (ownCom) ::CoUninitialize();

  if (images.empty()) return L"取不到系统图标：" + exePath;

  // 先记下旧图标组：留着旧组的话，资源管理器可能按编号取到旧图标，
  // 换图标就时灵时不灵。
  std::vector<std::wstring> oldGroups;
  HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (module) {
    ::EnumResourceNamesW(module, RT_GROUP_ICON, &enumIconGroups,
                         reinterpret_cast<LONG_PTR>(&oldGroups));
    ::FreeLibrary(module);
  }

  HANDLE update = ::BeginUpdateResourceW(exePath.c_str(), FALSE);
  if (!update) return L"无法打开资源句柄：" + exePath;

  const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
  for (const auto& name : oldGroups) {
    ::UpdateResourceW(update, RT_GROUP_ICON, name.c_str(), lang, nullptr, 0);
  }

  // 位图按 1..N 编号，索引表指向同样的编号
  for (size_t i = 0; i < images.size(); ++i) {
    const WORD id = static_cast<WORD>(i + 1);
    ::UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(id), lang,
                      const_cast<uint8_t*>(images[i].data.data()),
                      static_cast<DWORD>(images[i].data.size()));
  }

  // GRPICONDIR(6) + N 个 GRPICONDIRENTRY(14)
  std::vector<uint8_t> group;
  group.reserve(6 + images.size() * 14);
  putU16(group, 0);  // 保留字段
  putU16(group, 1);  // type = 图标
  putU16(group, static_cast<uint16_t>(images.size()));
  for (size_t i = 0; i < images.size(); ++i) {
    const IconImage& image = images[i];
    // 256 在目录项里写 0：字段只有一个字节，0 约定表示 256
    group.push_back(static_cast<uint8_t>(image.size >= 256 ? 0 : image.size));
    group.push_back(static_cast<uint8_t>(image.size >= 256 ? 0 : image.size));
    group.push_back(0);  // 调色板颜色数
    group.push_back(0);  // 保留
    putU16(group, 1);    // 色彩平面
    putU16(group, 32);   // 位深
    putU32(group, static_cast<uint32_t>(image.data.size()));
    putU16(group, static_cast<uint16_t>(i + 1));
  }

  // 编号 1 = 主图标组，资源管理器取编号最小的那个
  ::UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang,
                    const_cast<uint8_t*>(group.data()), static_cast<DWORD>(group.size()));

  if (!::EndUpdateResourceW(update, FALSE)) return L"提交图标资源失败：" + exePath;
  return {};
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
