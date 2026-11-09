// 工具栏：自绘渐变背景 + 大图标按钮，对齐 HaoZip 顶部样式
#include "ui_toolbar.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_list.h"

#include <windowsx.h>

namespace ui {
namespace {

constexpr const wchar_t* kToolbarClass = L"AIZipToolbar";
constexpr int kBtnWidth = 74;
constexpr int kBtnHeight = 62;
constexpr int kIconSize = 34;

enum BtnId { kBtnAdd = 1, kBtnExtract, kBtnDelete, kBtnMin, kBtnMax, kBtnClose };

struct Btn {
  BtnId id;
  const wchar_t* label;
  IconId icon;
};

const Btn kButtons[] = {
    {kBtnAdd, L"添加", IconId::kAdd},
    {kBtnExtract, L"解压到", IconId::kExtract},
    {kBtnDelete, L"删除", IconId::kDelete},
};
constexpr int kBtnCount = 3;

// 右上角系统按钮：无标题栏后由这里提供最小化/最大化/关闭。
// 图标框取 20，配合 58x44 的按钮，字形占满后不会显得局促。
constexpr int kSysBtnW = 58;
constexpr int kSysBtnH = 44;
constexpr int kSysIconSize = 20;

int g_hot = 0;      // 主按钮热区（1..3）
int g_pressed = 0;
int g_sysHot = 0;   // 系统按钮热区（同 BtnId 后缀）
int g_sysPressed = 0;

RECT ButtonRect(int index, int dpi) {
  const int x = ::MulDiv(10, dpi, 96) + index * ::MulDiv(kBtnWidth, dpi, 96);
  const int y = ::MulDiv(6, dpi, 96);
  return RECT{x, y, x + ::MulDiv(kBtnWidth - 6, dpi, 96), y + ::MulDiv(kBtnHeight, dpi, 96)};
}

// 系统按钮靠右贴边排列：关闭在最右上角
RECT SysButtonRect(int index, int clientW, int dpi) {
  const int w = ::MulDiv(kSysBtnW, dpi, 96);
  const int h = ::MulDiv(kSysBtnH, dpi, 96);
  const int right = clientW - (2 - index) * w;
  return RECT{right - w, 0, right, h};
}

int HitTest(POINT pt, int dpi) {
  for (int i = 0; i < kBtnCount; ++i) {
    RECT rc = ButtonRect(i, dpi);
    if (::PtInRect(&rc, pt)) return i + 1;
  }
  return 0;
}

int SysHitTest(POINT pt, int clientW, int dpi) {
  for (int i = 0; i < 3; ++i) {
    RECT rc = SysButtonRect(i, clientW, dpi);
    if (::PtInRect(&rc, pt)) return i + 1;
  }
  return 0;
}

void PaintToolbar(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  ui::MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);

  // 顶部近白、底部极淡蓝灰：只做很轻的过渡，避免大片死白显得廉价
  FillGradientV(dc, rc, kToolbarTop, kToolbarBottom);

  // 底部两层线：浅色高光 + 实分隔，让工具栏和地址栏之间有清晰分界
  DrawHLine(dc, rc.left, rc.right, rc.bottom - 2, RGB(255, 255, 255));
  DrawHLine(dc, rc.left, rc.right, rc.bottom - 1, kToolbarLine);

  for (int i = 0; i < kBtnCount; ++i) {
    const int idNum = i + 1;
    RECT br = ButtonRect(i, g_app.dpi);
    const bool hot = (g_hot == idNum);
    const bool pressed = (g_pressed == idNum);

    if (hot || pressed) {
      RECT hl = br;
      ::InflateRect(&hl, g_app.px(1), g_app.px(1));
      const int radius = g_app.px(5);
      // 悬停是白底微凸，按下是淡蓝内凹，靠渐变方向反转表达
      if (pressed) {
        FillRoundRect(dc, hl, RGB(203, 226, 245), radius);
        StrokeRoundRect(dc, hl, RGB(172, 201, 226), radius, 1);
      } else {
        FillGradientRoundRect(dc, hl, RGB(255, 255, 255), RGB(237, 245, 253), radius);
        StrokeRoundRect(dc, hl, RGB(203, 224, 241), radius, 1);
      }
    }

    // 图标下移按下效果
    const int nudge = pressed ? g_app.px(1) : 0;
    RECT iconRc{br.left + (br.right - br.left - g_app.px(kIconSize)) / 2,
                br.top + g_app.px(4) + nudge,
                br.left + (br.right - br.left - g_app.px(kIconSize)) / 2 + g_app.px(kIconSize),
                br.top + g_app.px(4) + nudge + g_app.px(kIconSize)};
    DrawIcon(dc, kButtons[i].icon, iconRc);

    RECT textRc{br.left, iconRc.bottom + g_app.px(1), br.right, br.bottom};
    DrawTextCenter(dc, textRc, kButtons[i].label, kToolbarText, fonts.ui);
  }

  // 右上角系统按钮：无标题栏后由这里提供最小化/最大化/关闭
  const HWND main = ::GetParent(hwnd);
  const bool zoomed = main && ::IsZoomed(main);
  const IconId sysIcons[3] = {IconId::kWinMin, zoomed ? IconId::kWinRestore : IconId::kWinMax,
                              IconId::kWinClose};
  for (int i = 0; i < 3; ++i) {
    const int idNum = i + 1;
    RECT br = SysButtonRect(i, rc.right, g_app.dpi);
    const bool hot = (g_sysHot == idNum);
    const bool pressed = (g_sysPressed == idNum);

    if (hot || pressed) {
      // 关闭按钮用系统惯用的红底，其余是中性淡灰
      const bool isClose = (sysIcons[i] == IconId::kWinClose);
      COLORREF bg = pressed ? (isClose ? RGB(193, 46, 42) : RGB(206, 216, 226))
                            : (isClose ? RGB(232, 17, 35) : RGB(226, 234, 242));
      FillRect(dc, br, bg);
    }

    const int nudge = pressed ? g_app.px(1) : 0;
    const int sz = g_app.px(kSysIconSize);
    RECT iconRc{br.left + (br.right - br.left - sz) / 2,
                br.top + (br.bottom - br.top - sz) / 2 + nudge,
                br.left + (br.right - br.left - sz) / 2 + sz,
                br.top + (br.bottom - br.top - sz) / 2 + nudge + sz};
    // 关闭按钮悬停时图标转白，否则在红底上看不清
    const bool closeHot = hot || pressed;
    DrawIcon(dc, sysIcons[i], iconRc, true, closeHot && sysIcons[i] == IconId::kWinClose
                                                ? RGB(255, 255, 255)
                                                : CLR_INVALID);
  }

  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

LRESULT CALLBACK ToolbarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      return 0;

    case WM_ERASEBKGND: {
      // 拖动/缩放时新露出的区域要立刻填色，否则闪黑条
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillGradientV(dc, rc, kToolbarTop, kToolbarBottom);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintToolbar(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      const int hit = HitTest(pt, g_app.dpi);
      const int sys = SysHitTest(pt, cr.right, g_app.dpi);
      if (hit != g_hot || sys != g_sysHot) {
        g_hot = hit;
        g_sysHot = sys;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      g_hot = 0;
      g_sysHot = 0;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      const int sys = SysHitTest(pt, cr.right, g_app.dpi);
      if (sys) {
        g_sysPressed = sys;
        g_pressed = 0;
        ::SetCapture(hwnd);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int hit = HitTest(pt, g_app.dpi);
      // 空白区的按下已被 WM_NCHITTEST 转给主窗口的标题栏，这里只会收到按钮区
      if (hit == 0) return 0;
      g_pressed = hit;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_LBUTTONDBLCLK: {
      return 0;  // 空白区双击由系统标题栏逻辑处理
    }

    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      const int sys = SysHitTest(pt, cr.right, g_app.dpi);
      const int sysWas = g_sysPressed;
      g_sysPressed = 0;
      if (sysWas) ::ReleaseCapture();

      if (sys && sys == sysWas) {
        HWND main = ::GetParent(hwnd);
        switch (sys) {
          case 1:
            ::ShowWindow(main, SW_MINIMIZE);
            break;
          case 2:
            ::ShowWindow(main, ::IsZoomed(main) ? SW_RESTORE : SW_MAXIMIZE);
            break;
          case 3:
            ::PostMessageW(main, WM_CLOSE, 0, 0);
            break;
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      const int hit = HitTest(pt, g_app.dpi);
      const int wasPressed = g_pressed;
      g_pressed = 0;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      if (hit && hit == wasPressed) {
        HWND main = ::GetParent(hwnd);
        switch (hit) {
          case kBtnAdd: {
            std::vector<std::wstring> paths;
            if (g_app.archiveMode) {
              // 压缩包内不支持追加，提示
              ::MessageBoxW(main, L"当前处于压缩包浏览模式，无法添加文件。\n请先返回文件浏览。",
                            L"AIZip", MB_OK | MB_ICONINFORMATION);
              break;
            }
            // 默认把当前选中项（没有则当前目录内容）打包
            const auto sel = GetSelectedItems();
            if (!sel.empty()) {
              for (int idx : sel) {
                if (idx >= 0 && idx < static_cast<int>(g_app.items.size()))
                  paths.push_back(g_app.items[idx].fullPath);
              }
            } else {
              paths.push_back(g_app.curDir);
            }
            ShowCompressDialog(main, paths);
            break;
          }
          case kBtnExtract: {
            const auto sel = GetSelectedItems();
            ShowExtractDialog(main, sel);
            break;
          }
          case kBtnDelete: {
            const auto sel = GetSelectedItems();
            DeleteItems(main, sel);
            break;
          }
        }
      }
      return 0;
    }

    case WM_NCHITTEST: {
      // 空白区按标题栏应答（HTCAPTION），系统随即发来 WM_NCLBUTTON*：
      // 拖动、双击最大化、右键系统菜单都由下面的分支处理
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ::ScreenToClient(hwnd, &pt);
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      if (SysHitTest(pt, cr.right, g_app.dpi) != 0) return HTCLIENT;
      return HitTest(pt, g_app.dpi) == 0 ? HTCAPTION : HTCLIENT;
    }

    case WM_NCLBUTTONDOWN: {
      if (wParam != HTCAPTION) break;
      // 转给主窗口进系统移动循环：这是唯一能让窗口跟着鼠标走的正规路径
      ::SendMessageW(::GetParent(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, lParam);
      return 0;
    }

    case WM_NCLBUTTONDBLCLK: {
      if (wParam != HTCAPTION) break;
      HWND main = ::GetParent(hwnd);
      ::ShowWindow(main, ::IsZoomed(main) ? SW_RESTORE : SW_MAXIMIZE);
      return 0;
    }

    case WM_RBUTTONUP: {
      // 空白区右键等同点标题栏：弹出系统菜单（还原/移动/大小/最小化/关闭）
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (HitTest(pt, g_app.dpi) != 0) break;
      ::ClientToScreen(hwnd, &pt);
      HMENU sys = ::GetSystemMenu(::GetParent(hwnd), FALSE);
      if (!sys) break;
      ::SetForegroundWindow(hwnd);
      const UINT cmd = ::TrackPopupMenu(sys, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                        ::GetParent(hwnd), nullptr);
      if (cmd) ::PostMessageW(::GetParent(hwnd), WM_SYSCOMMAND, cmd, 0);
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

HWND CreateToolbarWindow(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kToolbarClass, ToolbarProc, inst,
                                     CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, nullptr);
  }
  return ::CreateWindowExW(0, kToolbarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

}  // namespace ui
