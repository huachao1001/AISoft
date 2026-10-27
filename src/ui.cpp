#include "ui.h"

#include "util.h"

#include <shlobj.h>
#include <shellapi.h>

#include <cmath>
#include <map>

namespace ui {

Fonts fonts;

void Fonts::Create(int dpi) {
  Destroy();
  auto make = [&](int pt, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return ::CreateFontIndirectW(&lf);
  };
  ui = make(9, FW_NORMAL);
  uiBold = make(9, FW_SEMIBOLD);
}

void Fonts::Destroy() {
  for (HFONT* f : {&ui, &uiBold}) {
    if (*f) {
      ::DeleteObject(*f);
      *f = nullptr;
    }
  }
}

// ───────────────────────── MemDC ─────────────────────────

MemDC::MemDC(HDC target, const RECT& rc) : target_(target), rc_(rc) {
  mem_ = ::CreateCompatibleDC(target);
  bmp_ = ::CreateCompatibleBitmap(target, rc.right - rc.left, rc.bottom - rc.top);
  if (bmp_) oldBmp_ = static_cast<HBITMAP>(::SelectObject(mem_, bmp_));
  ::SetBkMode(mem_, TRANSPARENT);
}

MemDC::~MemDC() {
  if (bmp_) {
    ::SelectObject(mem_, oldBmp_);
    ::DeleteObject(bmp_);
  }
  if (mem_) ::DeleteDC(mem_);
}

void MemDC::Commit() {
  ::BitBlt(target_, rc_.left, rc_.top, rc_.right - rc_.left, rc_.bottom - rc_.top, mem_, 0, 0,
           SRCCOPY);
}

// ───────────────────────── 基础绘制 ─────────────────────────

void FillRect(HDC dc, const RECT& rc, COLORREF color) {
  HBRUSH b = ::CreateSolidBrush(color);
  ::FillRect(dc, &rc, b);
  ::DeleteObject(b);
}

void FillRoundRect(HDC dc, const RECT& rc, COLORREF color, int radius) {
  HBRUSH b = ::CreateSolidBrush(color);
  HPEN p = ::CreatePen(PS_SOLID, 1, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, b));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc, p));
  ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
  ::SelectObject(dc, oldB);
  ::SelectObject(dc, oldP);
  ::DeleteObject(b);
  ::DeleteObject(p);
}

// 圆角矩形描边（内部保持原样），用于让浅色块有边界、不糊成一片
void StrokeRoundRect(HDC dc, const RECT& rc, COLORREF color, int radius, int width) {
  HPEN p = ::CreatePen(PS_SOLID, width, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc, p));
  ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
  ::SelectObject(dc, oldB);
  ::SelectObject(dc, oldP);
  ::DeleteObject(p);
}

namespace {
void GradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom) {
  // TRIVERTEX 是 16 位定点色（8 位 << 8），高位会超 SHORT 正数范围，按位模式写
  auto ch16 = [](BYTE v) { return static_cast<USHORT>(static_cast<UINT>(v) << 8); };
  auto put = [&](TRIVERTEX& tv, LONG x, LONG y, COLORREF c) {
    tv.x = x;
    tv.y = y;
    union {
      USHORT u;
      SHORT s;
    } r{}, g{}, b{};
    r.u = ch16(GetRValue(c));
    g.u = ch16(GetGValue(c));
    b.u = ch16(GetBValue(c));
    tv.Red = r.s;
    tv.Green = g.s;
    tv.Blue = b.s;
    union {
      USHORT u;
      SHORT s;
    } a{};
    a.u = 0xFF00;
    tv.Alpha = a.s;
  };
  TRIVERTEX tv[2]{};
  put(tv[0], rc.left, rc.top, top);
  put(tv[1], rc.right, rc.bottom, bottom);
  GRADIENT_RECT gr{0, 1};
  ::GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}
}  // namespace

void FillGradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom) {
  GradientV(dc, rc, top, bottom);
}

void FillGradientRoundRect(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom, int radius) {
  // 用圆角区域裁剪，再在裁剪区内画整块渐变，边缘自然是圆角
  HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
  ::SelectClipRgn(dc, rgn);
  GradientV(dc, rc, top, bottom);
  ::SelectClipRgn(dc, nullptr);
  ::DeleteObject(rgn);
}

void DrawHLine(HDC dc, int left, int right, int y, COLORREF color) {
  RECT rc{left, y, right, y + 1};
  FillRect(dc, rc, color);
}

void DrawTextLeft(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  ::SetTextColor(dc, color);
  RECT r = rc;
  r.left += 2;
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, old);
}

void DrawTextCenter(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  ::SetTextColor(dc, color);
  RECT r = rc;
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, old);
}

void DrawTextEllipsis(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color,
                      HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  ::SetTextColor(dc, color);
  RECT r = rc;
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  ::SelectObject(dc, old);
}

// ───────────────────────── 自绘图标 ─────────────────────────

namespace {

// GDI 没有抗锯齿。这里先在放大 4 倍的离屏画布上绘制，再用 HALFTONE 缩回：
// 先把目标区域放大复制过去当底图，降采样时边缘才能和真实背景混合
// （GDI 无 alpha，直接画会在边缘留一圈白边）。
constexpr int kIconSs = 4;

void FillPoly(HDC dc, const POINT* pts, int count, COLORREF color) {
  HBRUSH b = ::CreateSolidBrush(color);
  HPEN p = ::CreatePen(PS_SOLID, 1, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, b));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc, p));
  ::Polygon(dc, pts, count);
  ::SelectObject(dc, oldB);
  ::SelectObject(dc, oldP);
  ::DeleteObject(b);
  ::DeleteObject(p);
}

// 端头半圆的条块：拼加号、箭头杆、托盘、垃圾桶都用它，圆角自然
void FillPill(HDC dc, int left, int top, int width, int height, COLORREF color) {
  FillRoundRect(dc, RECT{left, top, left + width, top + height}, color,
                width < height ? width : height);
}

// 折线描边，圆头圆角：垃圾桶外壳等“镂空”图形用
void StrokePoly(HDC dc, const POINT* pts, int count, COLORREF color, int width, bool closed) {
  LOGBRUSH lb{BS_SOLID, color, 0};
  HPEN p = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND, width, &lb,
                          0, nullptr);
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc, p));
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
  ::Polyline(dc, pts, count);
  if (closed) {
    POINT tail[2] = {pts[count - 1], pts[0]};
    ::Polyline(dc, tail, 2);
  }
  ::SelectObject(dc, oldB);
  ::SelectObject(dc, oldP);
  ::DeleteObject(p);
}

void DrawIconShape(HDC dc, IconId id, const RECT& rc, bool enabled, COLORREF inkOverride) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  const int s = (w < h ? w : h);

  // 禁用态去饱和并提亮，贴近系统禁用控件观感
  auto C = [enabled](int r, int g, int b) {
    if (enabled) return RGB(r, g, b);
    const int y = (r * 30 + g * 59 + b * 11) / 100;
    const int m = 255 - (255 - y) * 45 / 100;
    return RGB(m, m, m);
  };

  const int arrowHalfW = s * 3 / 10;
  const int arrowHalfH = s * 4 / 10;
  const int stroke = (s + 12) / 24;

  // 工具栏三个按钮共用一套底板与字形尺度，保证并排时视觉重量一致
  const int inset = s / 24;
  const RECT tile{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
  const int tileRadius = s * 5 / 16;
  const int t = s / 8;  // 字形笔画粗细
  const int u = s / 32;  // 字形网格单位；底板内可用范围约 ±14u
  const COLORREF ink = C(255, 255, 255);
  // 窗口按钮需要按背景反色（关闭键悬停转白），其余图标走固定配色
  auto G = [&](int r, int g, int b) {
    return inkOverride != CLR_INVALID ? inkOverride : C(r, g, b);
  };

  switch (id) {
    case IconId::kAdd: {
      FillGradientRoundRect(dc, tile, C(78, 158, 240), C(24, 106, 196), tileRadius);
      StrokeRoundRect(dc, tile, C(18, 84, 164), tileRadius, stroke);
      const int arm = u * 10;
      FillPill(dc, cx - arm, cy - t / 2, arm * 2, t, ink);
      FillPill(dc, cx - t / 2, cy - arm, t, arm * 2, ink);
      break;
    }
    case IconId::kExtract: {
      FillGradientRoundRect(dc, tile, C(250, 194, 88), C(224, 140, 22), tileRadius);
      StrokeRoundRect(dc, tile, C(200, 122, 14), tileRadius, stroke);
      // 箭头落入托盘：箭头 + 开口朝上的 U 形盘，托盘必须留在底板内（±14u）
      FillPill(dc, cx - t / 2, cy - u * 13, t, u * 13, ink);
      POINT head[3] = {{cx - u * 6, cy}, {cx + u * 6, cy}, {cx, cy + u * 6}};
      FillPoly(dc, head, 3, ink);
      POINT tray[4] = {{cx - u * 11, cy + u * 8},
                       {cx - u * 11, cy + u * 13},
                       {cx + u * 11, cy + u * 13},
                       {cx + u * 11, cy + u * 8}};
      StrokePoly(dc, tray, 4, ink, u * 3, false);
      break;
    }
    case IconId::kDelete: {
      FillGradientRoundRect(dc, tile, C(242, 112, 102), C(202, 56, 48), tileRadius);
      StrokeRoundRect(dc, tile, C(180, 44, 38), tileRadius, stroke);
      // 提手 + 桶盖
      FillPill(dc, cx - u * 4, cy - u * 12, u * 8, u * 3, ink);
      FillPill(dc, cx - u * 11, cy - u * 8, u * 22, u * 3, ink);
      // 实心桶身 + 两道细缝，缝隙窄才不会看成“眼睛”
      RECT body{cx - u * 8, cy - u * 4, cx + u * 8, cy + u * 13};
      FillRoundRect(dc, body, ink, u * 2);
      const int slotW = u * 2;
      FillPill(dc, cx - u * 4, cy, slotW, u * 10, C(214, 72, 64));
      FillPill(dc, cx + u * 4 - slotW, cy, slotW, u * 10, C(214, 72, 64));
      break;
    }
    case IconId::kFolder: {
      RECT body{cx - s / 2 + 2, cy - s / 4, cx + s / 2 - 2, cy + s / 3};
      FillRoundRect(dc, body, C(250, 200, 96), s / 12);
      RECT tab{cx - s / 2 + 2, cy - s / 3, cx - 2, cy - s / 4};
      FillRoundRect(dc, tab, C(232, 176, 72), s / 12);
      DrawHLine(dc, cx - s / 2 + 2, cx + s / 2 - 2, cy - s / 4, C(216, 158, 56));
      break;
    }
    case IconId::kArrowBack: {
      POINT pts[3] = {{cx + arrowHalfW, cy - arrowHalfH},
                      {cx - arrowHalfW, cy},
                      {cx + arrowHalfW, cy + arrowHalfH}};
      FillPoly(dc, pts, 3, C(90, 104, 116));
      break;
    }
    case IconId::kArrowForward: {
      POINT pts[3] = {{cx - arrowHalfW, cy - arrowHalfH},
                      {cx + arrowHalfW, cy},
                      {cx - arrowHalfW, cy + arrowHalfH}};
      FillPoly(dc, pts, 3, C(90, 104, 116));
      break;
    }
    case IconId::kArrowUp: {
      POINT pts[3] = {{cx - arrowHalfH, cy + arrowHalfW},
                      {cx + arrowHalfH, cy + arrowHalfW},
                      {cx, cy - arrowHalfW}};
      FillPoly(dc, pts, 3, C(90, 104, 116));
      break;
    }
    case IconId::kRefresh: {
      // 圆弧 + 箭头：用 Polyline 近似 270° 圆环，比 Arc 更可控
      const int r = s / 2 - stroke;
      HPEN p = ::CreatePen(PS_SOLID, stroke, C(90, 104, 116));
      auto op = ::SelectObject(dc, p);
      auto ob = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

      // 从 40° 扫到 300°，留出缺口给箭头
      constexpr int kSteps = 24;
      POINT arc[kSteps + 1];
      for (int i = 0; i <= kSteps; ++i) {
        const double deg = 40.0 + (300.0 - 40.0) * i / kSteps;
        const double rad = deg * 3.14159265358979 / 180.0;
        arc[i].x = cx + static_cast<int>(r * cos(rad));
        arc[i].y = cy - static_cast<int>(r * sin(rad));
      }
      ::Polyline(dc, arc, kSteps + 1);

      // 缺口处的小三角箭头
      POINT tip[3] = {{cx + r, cy - r / 3},
                      {cx + r - s / 5, cy - r / 3 - s / 6},
                      {cx + r - s / 5, cy - r / 3 + s / 6}};
      FillPoly(dc, tip, 3, C(90, 104, 116));
      ::SelectObject(dc, ob);
      ::SelectObject(dc, op);
      ::DeleteObject(p);
      break;
    }
    case IconId::kWinMin: {
      // 字形占满框宽的 ~62%，与系统标题栏按钮一致
      const int lineW = s * 62 / 100;
      const int thick = (s / 11) > 0 ? (s / 11) : 1;
      FillPill(dc, cx - lineW / 2, cy - thick / 2, lineW, thick, G(70, 82, 94));
      break;
    }
    case IconId::kWinMax: {
      const int half = s * 31 / 100;
      const int thick = (s / 11) > 0 ? (s / 11) : 1;
      RECT box{cx - half, cy - half, cx + half, cy + half};
      StrokeRoundRect(dc, box, G(70, 82, 94), s / 20, thick);
      break;
    }
    case IconId::kWinRestore: {
      // 两个叠放的方块：后者只露右上角
      const int half = s * 27 / 100;
      const int off = s * 12 / 100;
      const int thick = (s / 11) > 0 ? (s / 11) : 1;
      RECT back{cx - half + off, cy - half - off, cx + half + off, cy + half - off};
      StrokeRoundRect(dc, back, G(70, 82, 94), s / 20, thick);
      RECT front{cx - half - off, cy - half + off, cx + half - off, cy + half + off};
      StrokeRoundRect(dc, front, G(70, 82, 94), s / 20, thick);
      break;
    }
    case IconId::kWinClose: {
      // 两条等宽斜条交叉成 X；用几何笔描线比多边形拼更干净
      const int half = s * 31 / 100;
      const int thick = (s / 11) > 0 ? (s / 11) : 1;
      POINT d1[2] = {{cx - half, cy - half}, {cx + half, cy + half}};
      POINT d2[2] = {{cx + half, cy - half}, {cx - half, cy + half}};
      StrokePoly(dc, d1, 2, G(70, 82, 94), thick, false);
      StrokePoly(dc, d2, 2, G(70, 82, 94), thick, false);
      break;
    }
  }
}

}  // namespace

void DrawIcon(HDC dc, IconId id, const RECT& rc, bool enabled, COLORREF inkOverride) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  HDC buf = ::CreateCompatibleDC(dc);
  HBITMAP bmp = buf ? ::CreateCompatibleBitmap(dc, w * kIconSs, h * kIconSs) : nullptr;
  if (!buf || !bmp) {
    // 极端情况退化为直接绘制，至少保证图标不丢
    if (bmp) ::DeleteObject(bmp);
    if (buf) ::DeleteDC(buf);
    DrawIconShape(dc, id, rc, enabled, CLR_INVALID);
    return;
  }

  HGDIOBJ oldBmp = ::SelectObject(buf, bmp);
  ::SetStretchBltMode(buf, HALFTONE);
  ::SetBrushOrgEx(buf, 0, 0, nullptr);
  ::StretchBlt(buf, 0, 0, w * kIconSs, h * kIconSs, dc, rc.left, rc.top, w, h, SRCCOPY);

  DrawIconShape(buf, id, RECT{0, 0, w * kIconSs, h * kIconSs}, enabled, inkOverride);

  const int oldMode = ::SetStretchBltMode(dc, HALFTONE);
  ::SetBrushOrgEx(dc, 0, 0, nullptr);
  ::StretchBlt(dc, rc.left, rc.top, w, h, buf, 0, 0, w * kIconSs, h * kIconSs, SRCCOPY);
  ::SetStretchBltMode(dc, oldMode);

  ::SelectObject(buf, oldBmp);
  ::DeleteObject(bmp);
  ::DeleteDC(buf);
}

// 可缩放感应带宽度（96 DPI 基准）。
// 客户区已铺满整个窗口，视觉上只剩 1px 边框，但抓取范围要宽一些才好用。
// 取 6 是为了不超过工具栏按钮的起始 y（也是 6），避免抢走按钮点击。
int ResizeBorder(int dpi) {
  constexpr int kBase = 6;
  return ::MulDiv(kBase, dpi, 96);
}

// ───────────────────────── Shell 图标缓存 ─────────────────────────
namespace {
std::map<std::wstring, int> g_iconMap;
HIMAGELIST g_smallList = nullptr;

void EnsureImageList() {
  if (g_smallList) return;
  SHFILEINFOW sfi{};
  g_smallList = reinterpret_cast<HIMAGELIST>(
      ::SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
}

int IconForAttrib(DWORD attrib) {
  SHFILEINFOW sfi{};
  ::SHGetFileInfoW(L"x", attrib, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX |
                                                       SHGFI_SMALLICON);
  return sfi.iIcon;
}
}  // namespace

int ShellIconIndex(const std::wstring& path, bool isDir) {
  EnsureImageList();
  const std::wstring ext = isDir ? L"<dir>" : path;
  auto it = g_iconMap.find(ext);
  if (it != g_iconMap.end()) return it->second;

  int idx;
  if (isDir) {
    idx = IconForAttrib(FILE_ATTRIBUTE_DIRECTORY);
  } else {
    SHFILEINFOW sfi{};
    const DWORD flags = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                            ? SHGFI_SYSICONINDEX | SHGFI_SMALLICON
                            : SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    ::SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags);
    idx = sfi.iIcon;
  }
  g_iconMap[ext] = idx;
  return idx;
}

void ClearIconCache() { g_iconMap.clear(); }

HIMAGELIST ShellImageList() {
  EnsureImageList();
  return g_smallList;
}

// ───────────────────────── 窗口/对话框公共设施 ─────────────────────────

bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = style;
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = bg;
  wc.lpszClassName = name;
  if (iconId) {
    wc.hIcon = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON, 32, 32, 0));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
  }
  // 已注册（例如同名类被重复创建）视为成功
  return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle) {
  const int dpi = util::DpiForWindow(parent);
  auto px = [dpi](int v) { return ::MulDiv(v, dpi, 96); };
  return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w),
                           px(h), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           nullptr);
}

void PaintDialogBackground(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  MemDC mem(target, rc);
  FillRect(mem.dc(), rc, kDialogBg);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

HBRUSH DialogCtlBrush(UINT msg, HWND ctl) {
  if (msg == WM_CTLCOLORBTN) {
    // 只有分组框/复选/单选需要底色，普通按钮保持系统观感
    const LONG type = ctl ? (::GetWindowLongW(ctl, GWL_STYLE) & BS_TYPEMASK) : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) return nullptr;
  }
  static HBRUSH brush = ::CreateSolidBrush(kDialogBg);
  return brush;
}

RECT DialogRect(HWND owner, int w, int h) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, w, h};
  RECT ownerRc{};
  if (owner && ::GetWindowRect(owner, &ownerRc)) {
    out.left = ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2;
    out.top = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2;
  }
  return out;
}

void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable) {
  if (owner) ::EnableWindow(owner, FALSE);
  MSG msg;
  while (::IsWindow(dlg) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(dlg, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
  if (onBeforeEnable) onBeforeEnable();
  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
}

}  // namespace ui
