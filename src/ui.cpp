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

// ───────────────────────── 系统图标 ─────────────────────────
// 图标取自系统图标字体：优先 Segoe Fluent Icons（Win11），
// 缺失时回退 Segoe MDL2 Assets（Win10）。两者码点完全兼容。
namespace {

// 运行时选定可用的图标字体；都没有时返回空串，调用方退化为不画
const wchar_t* IconFace() {
  static const std::wstring face = [] {
    for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
      HDC dc = ::GetDC(nullptr);
      LOGFONTW lf{};
      lf.lfHeight = -16;
      wcscpy_s(lf.lfFaceName, name);
      HFONT f = ::CreateFontIndirectW(&lf);
      std::wstring actual;
      if (f) {
        HGDIOBJ old = ::SelectObject(dc, f);
        wchar_t buf[64] = {};
        ::GetTextFaceW(dc, 64, buf);
        actual = buf;
        ::SelectObject(dc, old);
        ::DeleteObject(f);
      }
      ::ReleaseDC(nullptr, dc);
      if (::CompareStringOrdinal(actual.c_str(), -1, name, -1, TRUE) == CSTR_EQUAL) return actual;
    }
    return std::wstring();
  }();
  return face.c_str();
}

// 按像素高度缓存图标字体；不可用时返回 nullptr
HFONT IconFont(int pixelHeight) {
  static std::map<int, HFONT> cache;
  auto it = cache.find(pixelHeight);
  if (it != cache.end()) return it->second;

  const wchar_t* face = IconFace();
  HFONT f = nullptr;
  if (*face) {
    LOGFONTW lf{};
    lf.lfHeight = -pixelHeight;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    // 灰度抗锯齿：彩色亚像素会让图标边缘出现彩边
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    f = ::CreateFontIndirectW(&lf);
  }
  cache[pixelHeight] = f;
  return f;
}

// 图标字形码点，取自 MDL2 字表
constexpr wchar_t GlyphFor(IconId id) {
  switch (id) {
    case IconId::kRefresh: return 0xE72C;      // Refresh
    case IconId::kExtract: return 0xE8DE;      // MoveToFolder
    case IconId::kExtractAll: return 0xE896;   // Download
    case IconId::kAdd: return 0xE710;          // Add
    case IconId::kTrash: return 0xE74D;        // Delete
    case IconId::kArrowBack: return 0xE72B;    // Back
    case IconId::kChevron: return 0xE76C;      // ChevronRight
    case IconId::kChevronDown: return 0xE70D;  // ChevronDown
    case IconId::kChevronUp: return 0xE70E;    // ChevronUp
    case IconId::kHome: return 0xE80F;         // Home
    case IconId::kFolder: return 0xE8B7;       // Folder
    case IconId::kFile: return 0xE8A5;         // Document
    case IconId::kSearch: return 0xE721;       // Search
  }
  return 0;
}

// 字形墨迹框：宽高与原点偏移都按字号归一，便于任意尺寸复用
struct InkBox {
  double rw = 1.0;  // 墨迹宽 / 字号
  double rh = 1.0;  // 墨迹高 / 字号
  double ox = 0.0;  // 墨迹左边界相对字符原点
  double oy = 0.0;  // 墨迹上边界相对基线
};

// 以 64px 参考字号取字形墨迹；字体没有该字形时退回整块 em 框
InkBox MeasureInk(wchar_t glyph) {
  static std::map<wchar_t, InkBox> cache;
  auto it = cache.find(glyph);
  if (it != cache.end()) return it->second;

  constexpr int kRef = 64;
  InkBox box;
  HDC dc = ::CreateCompatibleDC(nullptr);
  HFONT font = IconFont(kRef);
  if (dc && font) {
    HGDIOBJ old = ::SelectObject(dc, font);
    MAT2 mat{};
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    GLYPHMETRICS gm{};
    if (::GetGlyphOutlineW(dc, glyph, GGO_BITMAP, &gm, 0, nullptr, &mat) != GDI_ERROR &&
        gm.gmBlackBoxX && gm.gmBlackBoxY) {
      box.rw = static_cast<double>(gm.gmBlackBoxX) / kRef;
      box.rh = static_cast<double>(gm.gmBlackBoxY) / kRef;
      box.ox = static_cast<double>(gm.gmptGlyphOrigin.x) / kRef;
      box.oy = static_cast<double>(gm.gmptGlyphOrigin.y) / kRef;
    }
    ::SelectObject(dc, old);
  }
  if (dc) ::DeleteDC(dc);
  cache[glyph] = box;
  return box;
}

}  // namespace

void DrawIcon(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled) {
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  const wchar_t glyph = GlyphFor(id);
  if (!glyph) return;

  // 按墨迹最长边贴合目标矩形，避免字形撑出矩形压到相邻文字
  const InkBox ink = MeasureInk(glyph);
  const int side = w < h ? w : h;
  const double ratio = ink.rw > ink.rh ? ink.rw : ink.rh;
  const int fontSize = static_cast<int>(std::lround(side / ratio));
  HFONT font = IconFont(fontSize > 0 ? fontSize : 1);
  if (!font) return;

  COLORREF inkColor = color;
  if (!enabled) {
    const int y = (GetRValue(color) * 30 + GetGValue(color) * 59 + GetBValue(color) * 11) / 100;
    const int m = 255 - (255 - y) * 45 / 100;
    inkColor = RGB(m, m, m);
  }

  // 让墨迹框中心落在目标矩形中心
  const double inkW = ink.rw * fontSize;
  const double inkH = ink.rh * fontSize;
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  const int x = static_cast<int>(std::lround(cx - ink.ox * fontSize - inkW / 2));
  const int y = static_cast<int>(std::lround(cy + ink.oy * fontSize - inkH / 2));

  HFONT oldFont = static_cast<HFONT>(::SelectObject(dc, font));
  const int oldMode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF oldColor = ::SetTextColor(dc, inkColor);
  const UINT oldAlign = ::SetTextAlign(dc, TA_LEFT | TA_BASELINE);

  ::TextOutW(dc, x, y, &glyph, 1);

  ::SetTextAlign(dc, oldAlign);
  ::SetTextColor(dc, oldColor);
  ::SetBkMode(dc, oldMode);
  ::SelectObject(dc, oldFont);
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
// 只为了把行撑到 44px，图标本身在自绘时单独画。
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
