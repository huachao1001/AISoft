#include "AUI/platform.h"

namespace AUI {

// GetDpiForWindow 只在 Win10 1703+ 存在，动态取，避免抬高最低系统要求
int DpiForWindow(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static auto fn = reinterpret_cast<GetDpiForWindowFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (fn && hwnd) return static_cast<int>(fn(hwnd));

  HDC dc = ::GetDC(nullptr);
  const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
  ::ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

// 含点且小写；点出现在目录部分时不算扩展名
std::wstring FileExtension(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) return L"";
  const size_t sep = path.find_last_of(L"\\/");
  if (sep != std::wstring::npos && dot < sep) return L"";

  std::wstring ext = path.substr(dot);
  for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
  return ext;
}

}  // namespace AUI
