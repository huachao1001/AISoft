// Win32 窗口后端：窗口类注册、消息翻译、离屏绘制
#include "backend.h"

#include "AiUI/platform.h"

#include <windowsx.h>

#include <map>
#include <memory>

namespace AiUI {
namespace win32 {
namespace {

// 测量专用 Graphics：绑定 1x1 位图，不碰屏幕也不占 DC。
// 延迟创建：GDI+ 必须先启动，不能放在静态初始化期。
Gdiplus::Graphics* measureGraphics() {
  if (!ensureGdiplus()) return nullptr;
  static Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(1, 1, PixelFormat32bppPARGB);
  static Gdiplus::Graphics* graphics = Gdiplus::Graphics::FromImage(bitmap);
  return graphics;
}

// 键位映射：只覆盖界面用到的部分
Key mapKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::kA) + (vk - 'A'));
  if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::k0) + (vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12) {
    return static_cast<Key>(static_cast<int>(Key::kF1) + (vk - VK_F1));
  }
  switch (vk) {
    case VK_BACK: return Key::kBackspace;
    case VK_TAB: return Key::kTab;
    case VK_RETURN: return Key::kEnter;
    case VK_ESCAPE: return Key::kEscape;
    case VK_SPACE: return Key::kSpace;
    case VK_INSERT: return Key::kInsert;
    case VK_DELETE: return Key::kDelete;
    case VK_HOME: return Key::kHome;
    case VK_END: return Key::kEnd;
    case VK_PRIOR: return Key::kPageUp;
    case VK_NEXT: return Key::kPageDown;
    case VK_LEFT: return Key::kLeft;
    case VK_UP: return Key::kUp;
    case VK_RIGHT: return Key::kRight;
    case VK_DOWN: return Key::kDown;
    default: return Key::kNone;
  }
}

LPCTSTR cursorId(CursorShape shape) {
  switch (shape) {
    case CursorShape::kHand: return IDC_HAND;
    case CursorShape::kSizeHorizontal: return IDC_SIZEWE;
    case CursorShape::kIBeam: return IDC_IBEAM;
    default: return IDC_ARROW;
  }
}

constexpr UINT kBlinkTimerId = 1;
constexpr UINT kBlinkIntervalMs = 530;

// 画布守卫：提交完必须释放 DC
class DcCanvas : public Canvas {
 public:
  DcCanvas(std::unique_ptr<Canvas> inner, HWND hwnd, HDC dc)
      : inner_(std::move(inner)), hwnd_(hwnd), dc_(dc) {}
  ~DcCanvas() override {
    inner_.reset();
    ::ReleaseDC(hwnd_, dc_);
  }
  Painter& painter() override { return inner_->painter(); }
  void commit() override { inner_->commit(); }

 private:
  std::unique_ptr<Canvas> inner_;
  HWND hwnd_ = nullptr;
  HDC dc_ = nullptr;
};

}  // namespace

class Win32Window : public PlatformWindow {
 public:
  explicit Win32Window(WindowHost& host) : host_(host) {}

  ~Win32Window() override {
    if (blinkTimer_) ::KillTimer(hwnd_, kBlinkTimerId);
    if (hwnd_) {
      ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      ::DestroyWindow(hwnd_);
    }
  }

  bool create(HWND parent, const WindowParams& params, HINSTANCE instance) {
    registerClass(params.className, instance);

    DWORD style = WS_CHILD;
    if (params.visible) style |= WS_VISIBLE;
    if (params.tabStop) style |= WS_TABSTOP;

    // 单一窗口，不再需要 id；HMENU 位置传 0 即可
    hwnd_ = ::CreateWindowExW(0, params.className.c_str(), L"", style, 0, 0, 0, 0, parent, nullptr,
                              instance, this);
    if (!hwnd_) return false;
    dpi_ = ::GetDpiForWindow(hwnd_);
    return true;
  }

  // ── PlatformWindow ──
  NativeHandle handle() const override { return hwnd_; }

  Rect clientRect() const override {
    RECT rc{};
    if (hwnd_) ::GetClientRect(hwnd_, &rc);
    return Rect{rc.left, rc.top, rc.right, rc.bottom};
  }

  int dpi() const override { return dpi_; }

  std::unique_ptr<Canvas> createCanvas(const Rect& area) override {
    if (!hwnd_ || area.isEmpty()) return nullptr;
    HDC dc = ::GetDC(hwnd_);
    if (!dc) return nullptr;
    auto inner = makeCanvas(dc, area);
    if (!inner) {
      ::ReleaseDC(hwnd_, dc);
      return nullptr;
    }
    return std::make_unique<DcCanvas>(std::move(inner), hwnd_, dc);
  }

  void setBounds(const Rect& bounds) override {
    if (!hwnd_) return;
    ::MoveWindow(hwnd_, bounds.left, bounds.top, bounds.width(), bounds.height(), TRUE);
  }

  void show(bool visible) override {
    if (hwnd_) ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
  }

  void invalidate() override {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void invalidateRect(const Rect& rc) override {
    if (!hwnd_ || rc.isEmpty()) return;
    // 右/下边界是开区间，交给 Win32 时就是原值
    RECT r{rc.left, rc.top, rc.right, rc.bottom};
    ::InvalidateRect(hwnd_, &r, FALSE);
  }

  void setFocus() override {
    if (hwnd_) ::SetFocus(hwnd_);
  }

  void setCursorShape(CursorShape shape) override {
    cursor_ = shape;
    if (hwnd_) ::SetCursor(::LoadCursorW(nullptr, cursorId(shape)));
  }

  void captureMouse(bool capture) override {
    if (!hwnd_) return;
    if (capture) {
      ::SetCapture(hwnd_);
    } else {
      ::ReleaseCapture();
    }
  }

  void setBlinkTimer(bool on) override {
    if (!hwnd_) return;
    if (on && !blinkTimer_) {
      blinkTimer_ = ::SetTimer(hwnd_, kBlinkTimerId, kBlinkIntervalMs, nullptr);
    } else if (!on && blinkTimer_) {
      ::KillTimer(hwnd_, kBlinkTimerId);
      blinkTimer_ = 0;
    }
  }

  void setImeCaret(int x, int y) override {
    if (!hwnd_) return;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = POINT{x, y};
    if (HIMC imc = ::ImmGetContext(hwnd_)) {
      ::ImmSetCompositionWindow(imc, &cf);
      ::ImmReleaseContext(hwnd_, imc);
    }
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
      auto* self =
          static_cast<Win32Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      if (self) {
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }
      return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handleMessage(msg, wParam, lParam);
  }

  LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd_, &ps);
        // 更新区就是本次要重画的部分；为空时交给 ViewRoot 按整窗处理
        const RECT& rc = ps.rcPaint;
        Rect update{rc.left, rc.top, rc.right, rc.bottom};
        if (update.isEmpty()) update = clientRect();
        host_.onExpose(update);
        ::EndPaint(hwnd_, &ps);
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;  // 全部自绘

      case WM_SIZE:
        dpi_ = ::GetDpiForWindow(hwnd_);
        host_.onResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

      case WM_MOUSEMOVE:
        if (!tracking_) {
          tracking_ = true;
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
          ::TrackMouseEvent(&tme);
        }
        host_.onMouseMove(mouseEvent(lParam));
        return 0;

      case WM_MOUSELEAVE:
        tracking_ = false;
        host_.onMouseLeave();
        return 0;

      case WM_LBUTTONDOWN:
        ::SetFocus(hwnd_);
        host_.onMouseDown(mouseEvent(lParam, MouseButton::kLeft));
        return 0;

      case WM_LBUTTONUP:
        host_.onMouseUp(mouseEvent(lParam, MouseButton::kLeft));
        return 0;

      case WM_LBUTTONDBLCLK:
        host_.onDoubleClick(mouseEvent(lParam, MouseButton::kLeft));
        return 0;

      case WM_RBUTTONUP:
        host_.onContextMenu(mouseEvent(lParam, MouseButton::kRight));
        return 0;

      case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ::ScreenToClient(hwnd_, &pt);
        MouseEvent e;
        e.position = Point{pt.x, pt.y};
        e.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        e.modifiers = modifiers();
        host_.onMouseWheel(e);
        return 0;
      }

      case WM_KEYDOWN:
        if (host_.onKeyDown(mapKey(wParam), modifiers())) return 0;
        break;

      case WM_CHAR:
        if (host_.onChar(static_cast<wchar_t>(wParam))) return 0;
        break;

      case WM_SETFOCUS:
        host_.onFocusChanged(true);
        return 0;

      case WM_KILLFOCUS:
        host_.onFocusChanged(false);
        return 0;

      case WM_TIMER:
        if (wParam == kBlinkTimerId) {
          host_.onTimer();
          return 0;
        }
        break;

      case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
          ::SetCursor(::LoadCursorW(nullptr, cursorId(cursor_)));
          return TRUE;
        }
        break;

      case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

      case WM_DESTROY:
        if (blinkTimer_) {
          ::KillTimer(hwnd_, kBlinkTimerId);
          blinkTimer_ = 0;
        }
        return 0;

      default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
  }

 private:
  Modifiers modifiers() const {
    Modifiers m;
    m.ctrl = (::GetKeyState(VK_CONTROL) < 0);
    m.shift = (::GetKeyState(VK_SHIFT) < 0);
    m.alt = (::GetKeyState(VK_MENU) < 0);
    return m;
  }

  MouseEvent mouseEvent(LPARAM lParam, MouseButton button = MouseButton::kNone) const {
    MouseEvent e;
    e.position = Point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    e.button = button;
    e.modifiers = modifiers();
    return e;
  }

  static void registerClass(const std::wstring& name, HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &Win32Window::proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name.c_str();
    ::RegisterClassExW(&wc);
  }

  WindowHost& host_;
  HWND hwnd_ = nullptr;
  int dpi_ = 96;
  bool tracking_ = false;
  UINT_PTR blinkTimer_ = 0;
  CursorShape cursor_ = CursorShape::kArrow;
};

namespace {

class Win32Platform : public Platform {
 public:
  PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                               WindowHost& host) override {
    auto window = std::make_unique<Win32Window>(host);
    if (!window->create(static_cast<HWND>(parent), params, instance_)) return nullptr;
    Win32Window* raw = window.get();
    windows_[raw] = std::move(window);
    return raw;
  }

  void destroyWindow(PlatformWindow* window) override {
    windows_.erase(static_cast<Win32Window*>(window));
  }

  int screenDpi() override {
    HDC dc = ::GetDC(nullptr);
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
  }

  Font createFont(const wchar_t* family, float pointSize, bool bold, int dpi) override {
    return win32::makeFont(family, pointSize, bold, dpi);
  }

  void destroyFont(void* handle) override { win32::releaseFont(handle); }

  int measureText(const std::wstring& text, const Font& font) override {
    if (text.empty() || !font.isValid()) return 0;
    auto* g = measureGraphics();
    auto* f = static_cast<Gdiplus::Font*>(font.handle());
    if (!g || !f) return 0;
    Gdiplus::RectF bounds;
    g->MeasureString(text.c_str(), static_cast<INT>(text.size()), f, Gdiplus::PointF(0, 0),
                     &bounds);
    // 与 painter 后端保持一致：MeasureString 偏小，留 4px 余量防末字被裁
    return static_cast<int>(bounds.Width + 0.5f) + 4;
  }

  Image loadImage(const std::wstring& path) override { return loadImageFile(path); }

  Size imageSize(const Image& image) override { return win32::imagePixelSize(image); }

  void releaseImage(Image image) override { win32::releaseLoadedImage(image); }

  void startupDrawing() override { win32::ensureGdiplus(); }

  void shutdownDrawing() override {
    win32::releaseIconCaches();
    win32::releaseGdiplus();
  }

  bool isDrawingReady() override { return win32::ensureGdiplus(); }

 private:
  HINSTANCE instance_ = ::GetModuleHandleW(nullptr);
  std::map<Win32Window*, std::unique_ptr<Win32Window>> windows_;
};

Win32Platform g_platform;
bool g_started = false;
// 测试替换的平台后端；非空时所有 platform() 调用都走它
Platform* g_testPlatform = nullptr;

}  // namespace

Platform* backendPlatform() { return &g_platform; }

Platform* testPlatformOverride() { return g_testPlatform ? g_testPlatform : &g_platform; }

void markTestOverride(Platform* platform) { g_testPlatform = platform; }

void markStarted(bool started) { g_started = started; }
bool hasStarted() { return g_started; }

}  // namespace win32

Platform* platform() { return win32::testPlatformOverride(); }

// 测试替换：非空时优先于内置后端；空则回到 Win32
void setPlatformForTesting(Platform* platform) {
  win32::markTestOverride(platform);
}

void startupPlatform() {
  if (win32::hasStarted()) return;
  win32::ensureGdiplus();
  win32::markStarted(true);
}

void shutdownPlatform() {
  if (!win32::hasStarted()) return;
  win32::releaseIconCaches();
  win32::releaseGdiplus();
  win32::markStarted(false);
}

bool isPlatformReady() { return win32::hasStarted(); }

}  // namespace AiUI
