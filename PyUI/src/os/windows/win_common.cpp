#include "win_common.h"

#include "pyui/surface.h"

#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pyui {
namespace win32 {
namespace {

// UTF-16 → UTF-8 的核心转换，独立出来便于复用到 writeUtf8。
void appendUtf8(std::string& out, std::wstring_view text) {
  out.reserve(out.size() + text.size() * 3);
  for (size_t i = 0; i < text.size(); ++i) {
    std::uint32_t cp = static_cast<std::uint16_t>(text[i]);
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
      const std::uint32_t low = static_cast<std::uint16_t>(text[i + 1]);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
}

}  // namespace

std::wstring toWide(std::string_view text) {
  if (text.empty()) return std::wstring();
  // 不用 MultiByteToWideChar：它依赖代码页，且对孤立代理对的行为不明确。
  // 自己转能保证「非法字节 → U+FFFD」，不会因为一次坏字节吞掉整串。
  std::wstring out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0;
    size_t length = 1;
    if (lead < 0x80) {
      cp = lead;
    } else if ((lead >> 5) == 0x6 && i + 1 < text.size()) {
      cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
      length = 2;
    } else if ((lead >> 4) == 0xE && i + 2 < text.size()) {
      cp = ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
      length = 3;
    } else if ((lead >> 3) == 0x1E && i + 3 < text.size()) {
      cp = ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
      length = 4;
    } else {
      cp = 0xFFFD;  // 非法首字节或截断
    }
    i += length;

    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

std::string toUtf8(std::wstring_view text) {
  std::string out;
  appendUtf8(out, text);
  return out;
}

std::string lastError(const char* what) {
  const DWORD code = ::GetLastError();
  if (code == 0) return std::string(what) + "：没有错误码";

  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

  std::string detail;
  if (length > 0 && buffer != nullptr) {
    std::wstring message(buffer, length);
    // 去掉末尾的换行与句点：拼进日志里会断行
    while (!message.empty() &&
           (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ' ||
            message.back() == L'.')) {
      message.pop_back();
    }
    detail = toUtf8(message);
  }
  if (buffer != nullptr) ::LocalFree(buffer);

  char codeText[32];
  std::snprintf(codeText, sizeof(codeText), " (0x%08lX)", static_cast<unsigned long>(code));
  return std::string(what) + "：" + detail + codeText;
}

ATOM registerWindowClass(const wchar_t* className, WNDPROC proc, HICON icon, HCURSOR cursor,
                         UINT style) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  // 默认不带 CS_HREDRAW / CS_VREDRAW：脏区由我们自己算，系统整块重画会让
  // 「只失效一行」失去意义（拖动窗口时会闪）。需要 CS_DBLCLKS 的调用方传进来。
  wc.style = style;
  wc.lpfnWndProc = proc;
  wc.hInstance = moduleHandle();
  wc.hIcon = icon;
  wc.hCursor = cursor;
  wc.hbrBackground = nullptr;  // 背景由 ViewRoot 铺，系统刷白色会闪
  wc.lpszClassName = className;
  wc.hIconSm = icon;

  const ATOM atom = ::RegisterClassExW(&wc);
  // 类已存在不是失败：manifest 里声明过、或同进程开了第二个窗口，
  // 都会走到这里。必须返回非 0，否则调用方会当成「注册失败」直接放弃——
  // 症状是「第一个窗口正常，之后所有窗口都建不出来」，且没有任何报错。
  if (atom == 0) {
    if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
    return 1;  // 已存在：只要不是 0，调用方就认为可用
  }
  return atom;
}

HINSTANCE moduleHandle() {
  // GetModuleHandleW(nullptr) 返回本进程 exe；dll 里的控件应当用 exe 的
  // 资源（图标都在 exe 里），所以这里刻意不用 dll 自己的 HINSTANCE。
  return ::GetModuleHandleW(nullptr);
}

Point screenToClientPoint(HWND hwnd, POINT pt) {
  ::ScreenToClient(hwnd, &pt);
  return Point{static_cast<int>(pt.x), static_cast<int>(pt.y)};
}

int dpiForWindow(HWND hwnd) {
  // 运行时探测：Win10 1607 之前没有 GetDpiForWindow，静态链接会直接
  // 加载失败。用 GetProcAddress 探测，老系统退回设备上下文 DPI。
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static const GetDpiForWindowFn getDpi = reinterpret_cast<GetDpiForWindowFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (getDpi != nullptr && hwnd != nullptr) {
    const UINT dpi = getDpi(hwnd);
    if (dpi != 0) return static_cast<int>(dpi);
  }

  HDC dc = ::GetDC(hwnd);
  if (dc == nullptr) return 96;
  const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
  ::ReleaseDC(hwnd, dc);
  return dpi > 0 ? dpi : 96;
}

bool isSystemDarkMode() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD) return false;
  return value == 0;
}

HICON loadApplicationIcon(int pixelSize) {
  if (pixelSize <= 0) pixelSize = 16;

  // 第一步：问 exe 要**精确尺寸**的内嵌图标。
  //
  // 为什么不用 LoadIconW：它只返回 32×32（或 SM_CXICON 那一档），在 150%
  // 屏上放大到 48 就是糊的。exe 里通常有 16/24/32/48/256 多档，按需取才对。
  static const std::wstring exePath = [] {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
      const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
      if (length == 0) return std::wstring();
      if (length < buffer.size() - 1) {
        buffer.resize(length);
        return buffer;
      }
      buffer.resize(buffer.size() * 2);
    }
  }();

  if (!exePath.empty()) {
    HICON icon = nullptr;
    UINT id = 0;
    // nIcons=1 时返回实际取到的数量；0 表示这个文件没有内嵌图标。
    if (::PrivateExtractIconsW(exePath.c_str(), 0, pixelSize, pixelSize, &icon, &id, 1, 0) > 0 &&
        icon != nullptr) {
      return icon;
    }
  }

  // 第二步：没有内嵌图标（脚本直接跑、或 exe 没绑资源）时退回 shell 关联。
  // SHGFI_ICON 只能给 16 与 32 两档，选接近的那一档，放大由绘制层做。
  SHFILEINFOW info{};
  const DWORD flags = SHGFI_ICON | (pixelSize <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON);
  if (!exePath.empty() &&
      ::SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info), flags) != 0 &&
      info.hIcon != nullptr) {
    return info.hIcon;
  }

  // 最后兜底：系统默认应用图标。LoadIconW 返回的是**共享**图标，调用方
  // 不能 DestroyIcon 它——契约是「返回的图标由调用方销毁」，所以复制一份。
  HICON shared = ::LoadIconW(nullptr, IDI_APPLICATION);
  return shared != nullptr ? ::CopyIcon(shared) : nullptr;
}

Image iconToImage(HICON icon, int pixelSize) {
  if (icon == nullptr || pixelSize <= 0) return Image();

  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) return Image();

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = pixelSize;
  info.bmiHeader.biHeight = -pixelSize;  // 自上而下
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC memory = bitmap != nullptr ? ::CreateCompatibleDC(screen) : nullptr;
  ::ReleaseDC(nullptr, screen);
  if (bitmap == nullptr || bits == nullptr || memory == nullptr) {
    if (memory != nullptr) ::DeleteDC(memory);
    if (bitmap != nullptr) ::DeleteObject(bitmap);
    return Image();
  }
  // CreateDIBSection 到 DeleteObject 之间的任何提前返回都会漏掉释放，
  // 所以上面先把两个句柄一起判完，再进入无分支的主体。

  HGDIOBJ previous = ::SelectObject(memory, bitmap);
  // 先清成全透明：DrawIconEx 只画不透明部分，DIB 的初值是垃圾。
  std::memset(bits, 0, static_cast<size_t>(pixelSize) * pixelSize * sizeof(std::uint32_t));
  ::DrawIconEx(memory, 0, 0, icon, pixelSize, pixelSize, 0, nullptr, DI_NORMAL);
  ::SelectObject(memory, previous);
  ::DeleteDC(memory);

  // 拷进 owned Surface **之后**才能删 DIB：DIB 一删 bits 就悬空了。
  auto* surface = new Surface(pixelSize, pixelSize);
  std::memcpy(surface->pixels(), bits,
              static_cast<size_t>(pixelSize) * pixelSize * sizeof(std::uint32_t));
  ::DeleteObject(bitmap);

  // DrawIconEx 出来的像素在某些 alpha 通道上是 0 但我们仍要它（老的 24 位
  // 图标没有 alpha）。凡是 RGB 非零的像素补成不透明，否则图标会整块消失。
  for (int y = 0; y < pixelSize; ++y) {
    std::uint32_t* row = surface->row(y);
    for (int x = 0; x < pixelSize; ++x) {
      const std::uint32_t pixel = row[x];
      if ((pixel & 0x00FFFFFFu) != 0 && (pixel >> 24) == 0) {
        row[x] = pixel | 0xFF000000u;
      }
    }
  }
  return Image(surface);
}

std::wstring toWidePath(const std::string& utf8) {
  // 路径不做 UTF-8 校验式转换：Windows 文件系统允许任意 UTF-16 序列，：Windows 文件系统允许任意 UTF-16 序列，
  // 用宽松转换保证「能打开用户在资源管理器里看到的那个文件」。
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(),
                        length);
  return out;
}

}  // namespace win32
}  // namespace pyui
