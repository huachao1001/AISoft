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
  ui = make(10, FW_NORMAL);
  uiBold = make(10, FW_SEMIBOLD);
  uiSmall = make(9, FW_NORMAL);
}

void Fonts::Destroy() {
  for (HFONT* f : {&ui, &uiBold, &uiSmall}) {
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
  // 允许调用方继续用客户区绝对坐标绘制，位图内部只装 rc 这一块
  if (rc.left || rc.top) ::SetViewportOrgEx(mem_, -rc.left, -rc.top, nullptr);
}

MemDC::~MemDC() {
  if (bmp_) {
    ::SelectObject(mem_, oldBmp_);
    ::DeleteObject(bmp_);
  }
  if (mem_) ::DeleteDC(mem_);
}

void MemDC::Commit() {
  // BitBlt 的源坐标同样受视口原点影响，贴回前先还原
  ::SetViewportOrgEx(mem_, 0, 0, nullptr);
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

void StrokeRoundRect(HDC dc, const RECT& rc, COLORREF color, int radius, int width) {
  HPEN p = ::CreatePen(PS_SOLID, width, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc, p));
  // GDI 的描边以路径为中心，内缩半像素避免右侧/底边被裁掉
  ::RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, radius, radius);
  ::SelectObject(dc, oldB);
  ::SelectObject(dc, oldP);
  ::DeleteObject(p);
}

namespace {
void GradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom) {
  auto ch16 = [](BYTE v) { return static_cast<USHORT>(static_cast<UINT>(v) << 8); };
  auto put = [&](TRIVERTEX& tv, LONG x, LONG y, COLORREF c) {
    tv.x = x;
    tv.y = y;
    union {
      USHORT u;
      SHORT s;
    } r{}, g{}, b{}, a{};
    r.u = ch16(GetRValue(c));
    g.u = ch16(GetGValue(c));
    b.u = ch16(GetBValue(c));
    tv.Red = r.s;
    tv.Green = g.s;
    tv.Blue = b.s;
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

void DrawVLine(HDC dc, int top, int bottom, int x, COLORREF color) {
  RECT rc{x, top, x + 1, bottom};
  FillRect(dc, rc, color);
}

void DrawTextLeft(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  ::SetTextColor(dc, color);
  RECT r = rc;
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
  const int oldBk = ::SetBkMode(dc, TRANSPARENT);  // 默认不透明会用白底盖住行底色
  ::SetTextColor(dc, color);
  RECT r = rc;
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  ::SetBkMode(dc, oldBk);
  ::SelectObject(dc, old);
}

int TextWidth(HDC dc, const std::wstring& text, HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  SIZE sz{};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
  ::SelectObject(dc, old);
  return sz.cx;
}

int TextHeight(HDC dc, HFONT font) {
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font ? font : fonts.ui));
  TEXTMETRICW tm{};
  ::GetTextMetricsW(dc, &tm);
  ::SelectObject(dc, old);
  return tm.tmHeight;
}

// ───────────────────────── 线描图标 ─────────────────────────
// 全部按 24x24 网格描述，绘制时按目标尺寸等比缩放。
// 与 ui.html 的 SVG 一致：1.8 单位线宽、圆头圆角、只描边不填充。
namespace {

constexpr double kGrid = 24.0;
constexpr int kIconSs = 4;  // 超采样倍数，抵消 GDI 无抗锯齿

struct GridPen {
  HDC dc;
  double ox, oy, u;
  HPEN pen;
  HGDIOBJ oldPen;
  HGDIOBJ oldBrush;

  POINT P(double x, double y) const {
    return POINT{static_cast<LONG>(std::lround(ox + x * u)),
                 static_cast<LONG>(std::lround(oy + y * u))};
  }
  void Line(double x1, double y1, double x2, double y2) const {
    POINT pts[2] = {P(x1, y1), P(x2, y2)};
    ::Polyline(dc, pts, 2);
  }
  void Poly(const POINT* pts, int n, bool closed) const {
    ::Polyline(dc, pts, n);
    if (closed) {
      POINT tail[2] = {pts[n - 1], pts[0]};
      ::Polyline(dc, tail, 2);
    }
  }
  void PolyD(std::initializer_list<std::pair<double, double>> list, bool closed = false) const {
    std::vector<POINT> pts;
    pts.reserve(list.size());
    for (const auto& p : list) pts.push_back(P(p.first, p.second));
    Poly(pts.data(), static_cast<int>(pts.size()), closed);
  }
  // 圆弧近似：角度以屏幕坐标（y 向下）计
  void Arc(double cx, double cy, double r, double a0, double a1) const {
    constexpr int kSteps = 28;
    std::vector<POINT> pts;
    pts.reserve(kSteps + 1);
    for (int i = 0; i <= kSteps; ++i) {
      const double deg = a0 + (a1 - a0) * i / kSteps;
      const double rad = deg * 3.14159265358979 / 180.0;
      pts.push_back(P(cx + r * std::cos(rad), cy + r * std::sin(rad)));
    }
    Poly(pts.data(), static_cast<int>(pts.size()), false);
  }
};

void DrawStrokeIcon(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  COLORREF ink = color;
  if (!enabled) {
    const int y = (GetRValue(color) * 30 + GetGValue(color) * 59 + GetBValue(color) * 11) / 100;
    const int m = 255 - (255 - y) * 45 / 100;
    ink = RGB(m, m, m);
  }

  const int side = (w < h ? w : h);
  GridPen g;
  g.dc = dc;
  g.u = static_cast<double>(side) / kGrid;
  g.ox = rc.left + (w - side) / 2.0;
  g.oy = rc.top + (h - side) / 2.0;
  const int width = side >= 44 ? 3 : (side >= 26 ? 2 : 1);
  LOGBRUSH lb{BS_SOLID, ink, 0};
  g.pen = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND, width, &lb, 0,
                         nullptr);
  g.oldPen = ::SelectObject(dc, g.pen);
  g.oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

  switch (id) {
    case IconId::kRefresh: {
      // 对应 HTML 的双向刷新：两段圆弧 + 两个转角箭头（屏幕坐标 y 向下）
      g.Arc(12, 12, 9, 199.5, 315);   // 上弧：左中 → 右上
      g.Arc(12, 12, 9, 135, 19.5);    // 下弧：左下 → 右中
      g.PolyD({{23, 4}, {23, 10}, {17, 10}});   // 右上箭头
      g.PolyD({{1, 20}, {1, 14}, {7, 14}});     // 左下箭头
      break;
    }
    case IconId::kExtract: {
      // 托盘
      g.PolyD({{21, 15}, {21, 19}}, false);
      g.Arc(19, 19, 2, 0, 90);
      g.Line(17, 21, 7, 21);
      g.Arc(5, 19, 2, 90, 180);
      g.Line(3, 19, 3, 15);
      // 箭头
      g.Line(12, 3, 12, 15);
      g.PolyD({{7, 10}, {12, 15}, {17, 10}});
      break;
    }
    case IconId::kExtractAll: {
      g.PolyD({{21, 15}, {21, 19}}, false);
      g.Arc(19, 19, 2, 0, 90);
      g.Line(17, 21, 7, 21);
      g.Arc(5, 19, 2, 90, 180);
      g.Line(3, 19, 3, 15);
      g.Line(12, 2, 12, 14);
      g.PolyD({{8, 10}, {12, 14}, {16, 10}});
      break;
    }
    case IconId::kAdd: {
      g.PolyD({{21, 15}, {21, 19}}, false);
      g.Arc(19, 19, 2, 0, 90);
      g.Line(17, 21, 7, 21);
      g.Arc(5, 19, 2, 90, 180);
      g.Line(3, 19, 3, 15);
      g.Line(12, 5, 12, 15);
      g.Line(7, 10, 17, 10);
      break;
    }
    case IconId::kTrash: {
      g.Line(3, 6, 21, 6);
      g.PolyD({{19, 6}, {19, 20}}, false);
      g.Arc(17, 20, 2, 0, 90);
      g.Line(17, 22, 7, 22);
      g.Arc(5, 20, 2, 90, 180);
      g.Line(5, 20, 5, 6);
      g.PolyD({{9, 6}, {9, 4}}, false);
      g.Arc(11, 4, 2, 180, 270);
      g.Line(13, 2, 15, 2);
      g.Arc(15, 4, 2, 270, 360);
      g.Line(17, 4, 17, 6);
      break;
    }
    case IconId::kArrowBack: {
      g.Line(19, 12, 5, 12);
      g.PolyD({{10, 7}, {5, 12}, {10, 17}});
      break;
    }
    case IconId::kChevron: {
      g.PolyD({{9, 18}, {15, 12}, {9, 6}});
      break;
    }
    case IconId::kHome: {
      g.PolyD({{3, 9}, {12, 2}, {21, 9}, {21, 20}}, false);
      g.Arc(19, 20, 2, 0, 90);
      g.Line(17, 22, 7, 22);
      g.Arc(5, 20, 2, 90, 180);
      g.Line(9, 22, 9, 12);
      g.Line(9, 12, 15, 12);
      g.Line(15, 12, 15, 22);
      break;
    }
    case IconId::kFolder: {
      // 文件夹轮廓：左竖边 → 上边台阶 → 右上圆角 → 右竖边 → 底边
      g.Line(2, 20, 2, 5);
      g.Line(2, 5, 9, 5);
      g.Line(9, 5, 11, 8);
      g.Line(11, 8, 20, 8);
      g.Arc(20, 10, 2, 270, 360);
      g.Line(22, 10, 22, 20);
      g.Line(22, 20, 2, 20);
      break;
    }
    case IconId::kFile: {
      g.PolyD({{14, 2}, {6, 2}}, false);
      g.Arc(6, 4, 2, 180, 270);
      g.Line(4, 4, 4, 20);
      g.Arc(6, 20, 2, 90, 180);
      g.Line(6, 22, 18, 22);
      g.Arc(18, 20, 2, 0, 90);
      g.Line(20, 20, 20, 8);
      g.PolyD({{20, 8}, {14, 2}, {14, 8}, {20, 8}});
      break;
    }
    case IconId::kSearch: {
      g.Arc(11, 11, 7.5, 0, 360);
      g.Line(16.6, 16.6, 21, 21);
      break;
    }
  }

  ::SelectObject(dc, g.oldBrush);
  ::SelectObject(dc, g.oldPen);
  ::DeleteObject(g.pen);
}

}  // namespace

void DrawIcon(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  // 放大绘制再降采样：先把目标区域原样贴到离屏画布，边缘混合不会发白
  HDC buf = ::CreateCompatibleDC(dc);
  HBITMAP bmp = buf ? ::CreateCompatibleBitmap(dc, w * kIconSs, h * kIconSs) : nullptr;
  if (!buf || !bmp) {
    if (bmp) ::DeleteObject(bmp);
    if (buf) ::DeleteDC(buf);
    DrawStrokeIcon(dc, id, rc, color, enabled);
    return;
  }

  HGDIOBJ oldBmp = ::SelectObject(buf, bmp);
  ::SetStretchBltMode(buf, HALFTONE);
  ::SetBrushOrgEx(buf, 0, 0, nullptr);
  ::StretchBlt(buf, 0, 0, w * kIconSs, h * kIconSs, dc, rc.left, rc.top, w, h, SRCCOPY);

  DrawStrokeIcon(buf, id, RECT{0, 0, w * kIconSs, h * kIconSs}, color, enabled);

  const int oldMode = ::SetStretchBltMode(dc, HALFTONE);
  ::SetBrushOrgEx(dc, 0, 0, nullptr);
  ::StretchBlt(dc, rc.left, rc.top, w, h, buf, 0, 0, w * kIconSs, h * kIconSs, SRCCOPY);
  ::SetStretchBltMode(dc, oldMode);

  ::SelectObject(buf, oldBmp);
  ::DeleteObject(bmp);
  ::DeleteDC(buf);
}

// ───────────────────────── Shell 图标缓存 ─────────────────────────
namespace {
std::map<std::wstring, HICON> g_hiconMap;
}  // namespace

// 直接拿 HICON，绘制时用 DrawIconEx 缩放。
// 系统图像列表的索引在不同 shell 版本下不总是能画出来（压缩包内无实体文件时尤其如此）。
HICON ShellIconFor(const std::wstring& path, bool isDir) {
  const std::wstring key = isDir ? L"<dir>" : util::FileExtension(path);
  auto it = g_hiconMap.find(key);
  if (it != g_hiconMap.end()) return it->second;

  SHFILEINFOW sfi{};
  const DWORD attrib = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  const DWORD flags = SHGFI_ICON | SHGFI_SMALLICON |
                      (isDir ? SHGFI_USEFILEATTRIBUTES
                             : (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                                    ? 0
                                    : SHGFI_USEFILEATTRIBUTES));
  ::SHGetFileInfoW(isDir ? L"dir" : path.c_str(), attrib, &sfi, sizeof(sfi), flags);
  g_hiconMap[key] = sfi.hIcon;
  return sfi.hIcon;
}

void ClearIconCache() {
  for (auto& kv : g_hiconMap) {
    if (kv.second) ::DestroyIcon(kv.second);
  }
  g_hiconMap.clear();
}

// ── 列表行高用的占位图像列表 ──
// ListView 的行高由图像列表单元格尺寸决定。这里用 rowHeight 见方的空格子，
// 只为了把行撑到与 ui.html 一致的 44px，图标本身在自绘时单独画。
namespace {
HIMAGELIST g_rowList = nullptr;
int g_rowCell = 0;

void FreeRowList() {
  if (g_rowList) {
    ::ImageList_Destroy(g_rowList);
    g_rowList = nullptr;
  }
  g_rowCell = 0;
}
}  // namespace

HIMAGELIST ListImageList(int rowHeight) {
  if (g_rowList && g_rowCell == rowHeight) return g_rowList;
  FreeRowList();
  g_rowCell = rowHeight;
  g_rowList = ::ImageList_Create(rowHeight, rowHeight, ILC_COLOR32, 1, 1);
  // 下标 0 是全透明的空位，行首图标由自绘负责
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bmp = ::CreateCompatibleBitmap(screen, rowHeight, rowHeight);
  HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem, bmp));
  RECT bg{0, 0, rowHeight, rowHeight};
  FillRect(mem, bg, kPaneBg);
  ::SelectObject(mem, old);
  ::ImageList_Add(g_rowList, bmp, nullptr);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return g_rowList;
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
    const LONG type = ctl ? (::GetWindowLongW(ctl, GWL_STYLE) & BS_TYPEMASK) : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) return nullptr;
  }
  static HBRUSH brush = ::CreateSolidBrush(kDialogBg);
  return brush;
}

RECT DialogRect(HWND owner, int w, int h) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, w, h};
  RECT ref{};
  if ((owner && ::GetWindowRect(owner, &ref)) ||
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0)) {
    out.left = ref.left + ((ref.right - ref.left) - w) / 2;
    out.top = ref.top + ((ref.bottom - ref.top) - h) / 2;
  }
  return out;
}

// 只有本进程自己的窗口才能当模态 owner，否则异常退出会锁死外部界面
bool IsOwnWindow(HWND h) {
  if (!h || !::IsWindow(h)) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  return pid == ::GetCurrentProcessId();
}

void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable) {
  if (!IsOwnWindow(owner)) owner = nullptr;
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
