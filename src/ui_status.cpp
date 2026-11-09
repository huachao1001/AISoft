// 底部状态栏：自绘，与 ui.html 的 36px 条一致
#include "ui_status.h"

#include "app.h"
#include "sevenzip_loader.h"
#include "ui.h"
#include "ui_list.h"
#include "util.h"

#include <string>

namespace ui {
namespace {

constexpr const wchar_t* kStatusClass = L"AIZipStatusBar";

HWND g_status = nullptr;
std::wstring g_left;      // 路径或包路径
std::wstring g_tag;       // 格式标签，空则不画
std::wstring g_size;      // 大小
std::wstring g_right;     // 右侧统计

void PaintStatus(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kToolBg);

  const int padX = g_app.px(16);
  const int cy = (rc.top + rc.bottom) / 2;
  int x = padX;

  // 右侧统计先算宽度，左侧被挤时用它做边界
  const int rightW = TextWidth(dc, g_right, fonts.uiSmall);
  const int rightLeft = rc.right - padX - rightW;
  const int limit = rightLeft - g_app.px(16);

  // 位置前的 home 图标（压缩包模式）
  if (g_app.archiveMode) {
    const int is = g_app.px(15);
    RECT ir{x, cy - is / 2, x + is, cy + is / 2};
    DrawIcon(dc, IconId::kHome, ir, kTagInk);
    x = ir.right + g_app.px(8);
  }

  // 路径：先按可用宽度收窄，再画（长度测量不能超过 limit，否则后面全部错位）
  RECT tr{x, rc.top, limit, rc.bottom};
  std::wstring shown = g_left;
  while (!shown.empty() && TextWidth(dc, shown, fonts.uiSmall) > (limit - x)) {
    shown.erase(shown.begin());
  }
  DrawTextEllipsis(dc, tr, shown, kTextMuted, fonts.uiSmall);
  x += TextWidth(dc, shown, fonts.uiSmall);
  if (x > limit) x = limit;

  // 格式标签
  if (!g_tag.empty()) {
    const int tagPadX = g_app.px(6);
    const int tagW = TextWidth(dc, g_tag, fonts.uiSmall) + tagPadX * 2;
    const int tagH = g_app.px(19);
    RECT tagRc{x + g_app.px(8), cy - tagH / 2, x + g_app.px(8) + tagW, cy + tagH / 2};
    if (tagRc.right < limit) {
      FillRoundRect(dc, tagRc, kTagBg, g_app.px(4));
      DrawTextCenter(dc, tagRc, g_tag, kTagInk, fonts.uiSmall);
      x = tagRc.right + g_app.px(8);
    }
  }

  // 大小
  if (!g_size.empty() && x < limit) {
    RECT sr{x, rc.top, limit, rc.bottom};
    DrawTextLeft(dc, sr, g_size, kTextMuted, fonts.uiSmall);
  }

  // 右侧统计
  if (rightLeft > 0) {
    RECT rr{rightLeft, rc.top, rc.right - padX, rc.bottom};
    DrawTextEllipsis(dc, rr, g_right, kTextMuted, fonts.uiSmall);
  }

  DrawHLine(dc, 0, rc.right, 0, kLine);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

LRESULT CALLBACK StatusProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kToolBg);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintStatus(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND CreateStatusBar(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kStatusClass, StatusProc, inst, CS_HREDRAW | CS_VREDRAW,
                                     nullptr);
  }
  g_status = ::CreateWindowExW(0, kStatusClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  return g_status;
}

void RefreshStatusBarData() {
  if (g_app.archiveMode) {
    g_left = g_app.archivePath;
    const std::wstring ext = util::FileExtension(g_app.archivePath);
    g_tag = z7::FormatNameFromExtension(ext);
    LARGE_INTEGER li{};
    HANDLE h = ::CreateFileW(g_app.archivePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::GetFileSizeEx(h, &li);
      ::CloseHandle(h);
    }
    g_size = util::FormatSize(static_cast<uint64_t>(li.QuadPart));
  } else {
    g_left = g_app.curDir;
    g_tag.clear();
    g_size.clear();
  }

  size_t files = 0, dirs = 0;
  uint64_t total = 0;
  for (int idx : VisibleRows()) {
    if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) continue;
    const Item& it = g_app.items[idx];
    if (it.isDir) {
      ++dirs;
    } else {
      ++files;
      total += it.size;
    }
  }

  if (g_app.archiveMode) {
    g_right = L"压缩包内：总计 " + std::to_wstring(dirs) + L" 个文件夹，" +
              std::to_wstring(files) + L" 个文件";
  } else if (files > 0 && dirs > 0) {
    g_right = L"总计 " + std::to_wstring(files) + L" 个文件、" + std::to_wstring(dirs) +
              L" 个文件夹";
  } else if (dirs > 0) {
    g_right = L"总计 " + std::to_wstring(dirs) + L" 个文件夹";
  } else if (files > 0) {
    g_right = L"总计 " + std::to_wstring(files) + L" 个文件";
  } else {
    g_right = L"没有项目";
  }
  if (!g_app.archiveMode && files > 0) {
    std::wstring size = L"（";
    size += util::FormatSize(total);
    size += L"）";
    g_right += size;
  }

  const int selCount = static_cast<int>(GetSelectedItems().size());
  if (selCount > 0) g_right += L"，已选中 " + std::to_wstring(selCount) + L" 项";

  InvalidateStatusBar();
}

void InvalidateStatusBar() {
  if (g_status) ::InvalidateRect(g_status, nullptr, FALSE);
}

}  // namespace ui
