// 顶部两条：40px 标题栏（系统按钮）+ 52px 工具按钮条
#include "ui_toolbar.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_list.h"

#include <windowsx.h>

namespace ui {
namespace {

constexpr const wchar_t* kTitleClass = L"AIZipTitleBar";
constexpr const wchar_t* kToolClass = L"AIZipToolBar";

// 标题栏系统按钮：36x30
constexpr int kSysBtnW = 36;
constexpr int kSysBtnH = 30;

// 工具按钮：padding 7px 12px，图标 18，间距 6
constexpr int kToolBtnH = 32;
constexpr int kToolPadX = 12;
constexpr int kToolIcon = 18;
constexpr int kToolGapIcon = 6;
constexpr int kToolGap = 8;
constexpr int kGroupGap = 10;

enum BtnId {
  kBtnRefresh = 1,
  kBtnExtractSel,
  kBtnExtractAll,
  kBtnAdd,
  kBtnDelete,
};

struct Btn {
  BtnId id;
  const wchar_t* label;
  IconId icon;
};

const Btn kButtons[] = {
    {kBtnRefresh, L"刷新", IconId::kRefresh},
    {kBtnExtractSel, L"提取选中", IconId::kExtract},
    {kBtnExtractAll, L"全部解压", IconId::kExtractAll},
    {kBtnAdd, L"添加文件", IconId::kAdd},
    {kBtnDelete, L"删除选中", IconId::kTrash},
};
constexpr int kBtnCount = 5;

// 分组：刷新 | 提取选中 全部解压 | 添加文件 删除选中
struct Group {
  int first;
  int count;
};
const Group kGroups[] = {{0, 1}, {1, 2}, {3, 2}};
constexpr int kGroupCount = 3;

int g_hot = 0;
int g_pressed = 0;
int g_sysHot = 0;
int g_sysPressed = 0;
HWND g_toolbar = nullptr;

// 缓存每个按钮的矩形，绘制与命中用同一份，避免两处算错
RECT g_btnRect[kBtnCount]{};
RECT g_groupLine[kGroupCount]{};
bool g_layoutValid = false;

int BtnWidth(int i, int dpi) {
  HDC dc = ::GetDC(nullptr);
  const int textW = TextWidth(dc, kButtons[i].label, fonts.ui);
  ::ReleaseDC(nullptr, dc);
  return ::MulDiv(kToolPadX, dpi, 96) * 2 + ::MulDiv(kToolIcon, dpi, 96) +
         ::MulDiv(kToolGapIcon, dpi, 96) + textW;
}

void RebuildLayout(int dpi) {
  const int gap = ::MulDiv(kToolGap, dpi, 96);
  const int groupGap = ::MulDiv(kGroupGap, dpi, 96);
  const int h = ::MulDiv(kToolBtnH, dpi, 96);
  const int top = (::MulDiv(kToolH, dpi, 96) - h) / 2;

  int x = ::MulDiv(16, dpi, 96);
  for (int gi = 0; gi < kGroupCount; ++gi) {
    if (gi > 0) {
      g_groupLine[gi] = RECT{x - groupGap / 2, top, x - groupGap / 2 + 1,
                             top + h};
      x += groupGap / 2;
    }
    for (int k = 0; k < kGroups[gi].count; ++k) {
      const int bi = kGroups[gi].first + k;
      const int w = BtnWidth(bi, dpi);
      g_btnRect[bi] = RECT{x, top, x + w, top + h};
      x += w + gap;
    }
    x += groupGap;
  }
  g_layoutValid = true;
}

int HitTest(POINT pt) {
  for (int i = 0; i < kBtnCount; ++i) {
    if (::PtInRect(&g_btnRect[i], pt)) return i + 1;
  }
  return 0;
}

RECT SysButtonRect(int index, int clientW, int dpi) {
  const int w = ::MulDiv(kSysBtnW, dpi, 96);
  const int h = ::MulDiv(kSysBtnH, dpi, 96);
  const int right = clientW - (2 - index) * w - ::MulDiv(8, dpi, 96);
  return RECT{right - w, 0, right, h};
}

int SysHitTest(POINT pt, int clientW, int dpi) {
  for (int i = 0; i < 3; ++i) {
    RECT rc = SysButtonRect(i, clientW, dpi);
    if (::PtInRect(&rc, pt)) return i + 1;
  }
  return 0;
}

// 按钮可用性：无选中时“提取选中/删除选中”置灰；压缩包模式下“添加文件”置灰
bool BtnEnabled(int idx) {
  switch (kButtons[idx].id) {
    case kBtnRefresh:
    case kBtnExtractAll:
      return true;
    case kBtnExtractSel:
    case kBtnDelete:
      return !GetSelectedItems().empty();
    case kBtnAdd:
      return !g_app.archiveMode;
    default:
      return true;
  }
}

// ───────────────────────── 标题栏 ─────────────────────────

void PaintTitleBar(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kTitleBg);

  const HWND main = ::GetParent(hwnd);
  const bool zoomed = main && ::IsZoomed(main);
  const IconId icons[3] = {IconId::kWinMin, zoomed ? IconId::kWinRestore : IconId::kWinMax,
                           IconId::kWinClose};
  for (int i = 0; i < 3; ++i) {
    const int idNum = i + 1;
    RECT br = SysButtonRect(i, rc.right, g_app.dpi);
    const bool hot = (g_sysHot == idNum);
    const bool pressed = (g_sysPressed == idNum);
    const bool isClose = (icons[i] == IconId::kWinClose);

    if (hot || pressed) {
      FillRect(dc, br, isClose ? (pressed ? RGB(0xc1, 0x2e, 0x2a) : RGB(0xe5, 0x39, 0x35))
                               : (pressed ? RGB(0xc8, 0xc8, 0xca) : RGB(0xd8, 0xd8, 0xda)));
    }

    const int sz = g_app.px(14);
    RECT iconRc{br.left + (br.right - br.left - sz) / 2, br.top + (br.bottom - br.top - sz) / 2,
                br.left + (br.right - br.left - sz) / 2 + sz,
                br.top + (br.bottom - br.top - sz) / 2 + sz};
    const bool white = isClose && (hot || pressed);
    DrawIcon(dc, icons[i], iconRc, white ? RGB(255, 255, 255) : kInk);
  }

  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

LRESULT CALLBACK TitleProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kTitleBg);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintTitleBar(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      const int next = SysHitTest(pt, cr.right, g_app.dpi);
      if (next != g_sysHot) {
        g_sysHot = next;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
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
        ::SetCapture(hwnd);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      return 0;  // 空白区交给 WM_NCHITTEST 的 HTCAPTION 拖动
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      const int sys = SysHitTest(pt, cr.right, g_app.dpi);
      const int was = g_sysPressed;
      g_sysPressed = 0;
      if (was) ::ReleaseCapture();
      if (sys && sys == was) {
        HWND main = ::GetParent(hwnd);
        switch (sys) {
          case 1: ::ShowWindow(main, SW_MINIMIZE); break;
          case 2: ::ShowWindow(main, ::IsZoomed(main) ? SW_RESTORE : SW_MAXIMIZE); break;
          case 3: ::PostMessageW(main, WM_CLOSE, 0, 0); break;
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ::ScreenToClient(hwnd, &pt);
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      if (SysHitTest(pt, cr.right, g_app.dpi) != 0) return HTCLIENT;
      return HTCAPTION;
    }
    case WM_NCLBUTTONDOWN:
      if (wParam != HTCAPTION) break;
      ::SendMessageW(::GetParent(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, lParam);
      return 0;
    case WM_NCLBUTTONDBLCLK: {
      if (wParam != HTCAPTION) break;
      HWND main = ::GetParent(hwnd);
      ::ShowWindow(main, ::IsZoomed(main) ? SW_RESTORE : SW_MAXIMIZE);
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      if (SysHitTest(pt, cr.right, g_app.dpi) != 0) break;
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

// ───────────────────────── 工具按钮条 ─────────────────────────

void PaintToolBar(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kToolBg);

  if (!g_layoutValid) RebuildLayout(g_app.dpi);

  // 分组竖线
  for (int gi = 1; gi < kGroupCount; ++gi) {
    DrawVLine(dc, g_groupLine[gi].top, g_groupLine[gi].bottom, g_groupLine[gi].left,
              RGB(0xdc, 0xdc, 0xdc));
  }

  const int radius = g_app.px(7);
  for (int i = 0; i < kBtnCount; ++i) {
    const int idNum = i + 1;
    RECT br = g_btnRect[i];
    const bool enabled = BtnEnabled(i);
    const bool hot = enabled && g_hot == idNum;
    const bool pressed = enabled && g_pressed == idNum;

    FillRoundRect(dc, br, pressed ? kBtnDownBg : (hot ? kBtnHoverBg : kPaneBg), radius);
    StrokeRoundRect(dc, br, !enabled ? kBtnDisBorder : (hot ? kBtnHoverBorder : kBtnBorder),
                    radius, 1);

    const int padX = g_app.px(kToolPadX);
    const int iconSz = g_app.px(kToolIcon);
    RECT iconRc{br.left + padX, br.top + (br.bottom - br.top - iconSz) / 2,
                br.left + padX + iconSz, br.top + (br.bottom - br.top - iconSz) / 2 + iconSz};
    DrawIcon(dc, kButtons[i].icon, iconRc, enabled ? kInk : kTextFaint, enabled);

    RECT textRc{iconRc.right + g_app.px(kToolGapIcon), br.top, br.right - padX, br.bottom};
    DrawTextLeft(dc, textRc, kButtons[i].label, enabled ? kText : kTextFaint, fonts.ui);
  }

  // 底部分隔线
  DrawHLine(dc, 0, rc.right, rc.bottom - 1, kLine);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

void RunButton(HWND hwnd, int hit) {
  HWND main = ::GetParent(hwnd);
  switch (kButtons[hit - 1].id) {
    case kBtnRefresh:
      AppRefreshView();
      break;
    case kBtnExtractSel: {
      const auto sel = GetSelectedItems();
      ShowExtractDialog(main, sel);
      break;
    }
    case kBtnExtractAll:
      ShowExtractDialog(main, {});
      break;
    case kBtnAdd: {
      if (g_app.archiveMode) break;
      std::vector<std::wstring> paths;
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
    case kBtnDelete:
      DeleteItems(main, GetSelectedItems());
      break;
  }
  ::InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK ToolBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
      PaintToolBar(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int next = HitTest(pt);
      if (next != g_hot) {
        g_hot = next;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_hot = 0;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitTest(pt);
      if (hit && BtnEnabled(hit - 1)) {
        g_pressed = hit;
        ::SetCapture(hwnd);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitTest(pt);
      const int was = g_pressed;
      g_pressed = 0;
      if (was) ::ReleaseCapture();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      if (hit && hit == was && BtnEnabled(hit - 1)) RunButton(hwnd, hit);
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

HWND CreateTitleBar(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kTitleClass, TitleProc, inst, CS_HREDRAW | CS_VREDRAW,
                                     nullptr);
  }
  return ::CreateWindowExW(0, kTitleClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

HWND CreateToolBar(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kToolClass, ToolBarProc, inst,
                                     CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, nullptr);
  }
  HWND w = ::CreateWindowExW(0, kToolClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  g_toolbar = w;
  g_layoutValid = false;
  return w;
}

void InvalidateToolBar() {
  g_layoutValid = false;
  g_hot = 0;
  g_pressed = 0;
  // 按钮的可用性是在绘制时算的，必须重画才能看到灰/亮变化
  if (g_toolbar) ::InvalidateRect(g_toolbar, nullptr, TRUE);
}

}  // namespace ui
