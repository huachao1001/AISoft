// Windows 窗口后端：窗口类注册、消息翻译、离屏绘制
#include "backend.h"

#include "AiUI/platform.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <map>
#include <memory>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {
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

// 命中目标 → 系统边缘码，用于开始系统缩放
int hitCode(HitTarget target) {
  switch (target) {
    case HitTarget::kLeft: return HTLEFT;
    case HitTarget::kRight: return HTRIGHT;
    case HitTarget::kTop: return HTTOP;
    case HitTarget::kBottom: return HTBOTTOM;
    case HitTarget::kTopLeft: return HTTOPLEFT;
    case HitTarget::kTopRight: return HTTOPRIGHT;
    case HitTarget::kBottomLeft: return HTBOTTOMLEFT;
    case HitTarget::kBottomRight: return HTBOTTOMRIGHT;
    case HitTarget::kCaption: return HTCAPTION;
    case HitTarget::kMaxButton: return HTMAXBUTTON;
    default: return HTCLIENT;
  }
}

constexpr UINT kBlinkTimerId = 1;
// 一次性延时定时器（滚动条自动隐藏等）
constexpr UINT kDelayTimerId = 2;
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

class WindowsWindow : public PlatformWindow {
 public:
  explicit WindowsWindow(WindowHost& host) : host_(host) {}

  ~WindowsWindow() override {
    if (blinkTimer_) ::KillTimer(hwnd_, kBlinkTimerId);
    if (eraseBrush_) ::DeleteObject(eraseBrush_);
    if (hwnd_) {
      ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      ::DestroyWindow(hwnd_);
    }
    // 图标已随窗口销毁而解除引用，这时才能释放
    if (bigIcon_) ::DestroyIcon(bigIcon_);
    if (smallIcon_) ::DestroyIcon(smallIcon_);
  }

  bool create(HWND parent, const WindowParams& params, HINSTANCE instance) {
    registerClass(params.className, instance);
    style_ = params.style;

    DWORD style = WS_CHILD;
    DWORD exStyle = 0;
    HWND owner = parent;
    owned_ = parent != nullptr;
    if (params.style.topLevel) {
      // 必须保留 WS_CAPTION 这一组样式位，DWM 才会给窗口投影、圆角与贴边吸附；
      // 少了它窗口会退回经典外观。系统标题栏外观靠 WM_NCCALCSIZE 让位 +
      // SetWindowTheme 关掉标题栏主题来隐去，而不是删样式。
      style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
      if (params.style.resizable) style |= WS_THICKFRAME;
      if (params.style.minimizable) style |= WS_MINIMIZEBOX;
      if (params.style.maximizable) style |= WS_MAXIMIZEBOX;
      if (params.visible) style |= WS_VISIBLE;
      exStyle = WS_EX_APPWINDOW;
    } else {
      if (params.visible) style |= WS_VISIBLE;
      if (params.tabStop) style |= WS_TABSTOP;
    }

    // 单一窗口，不再需要 id；HMENU 位置传 0 即可
    hwnd_ =
        ::CreateWindowExW(exStyle, params.className.c_str(), params.title.c_str(), style, 0, 0, 0, 0,
                          owner, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = ::GetDpiForWindow(hwnd_);
    minWidth_ = params.minWidth;
    minHeight_ = params.minHeight;
    // 首帧之前窗口就是这块中性底色，宿主随后会用 setBackgroundColor 覆盖
    setBackgroundColor(makeColor(0xf3, 0xf4, 0xf6));
    if (params.style.topLevel) applyWindowIcons();
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
    auto inner = canvasCache_.create(dc, area);
    if (!inner) {
      ::ReleaseDC(hwnd_, dc);
      return nullptr;
    }
    return std::make_unique<DcCanvas>(std::move(inner), hwnd_, dc);
  }

  void setBounds(const Rect& bounds) override {
    if (!hwnd_) return;
    if (style_.topLevel) {
      // 顶层窗口的 bounds 指客户区，Reverse 后换算成整窗尺寸
      RECT rc{bounds.left, bounds.top, bounds.right, bounds.bottom};
      ::AdjustWindowRectExForDpi(&rc, ::GetWindowLongW(hwnd_, GWL_STYLE), FALSE,
                                 ::GetWindowLongW(hwnd_, GWL_EXSTYLE), dpi_);
      ::SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      return;
    }
    ::MoveWindow(hwnd_, bounds.left, bounds.top, bounds.width(), bounds.height(), TRUE);
  }

  Rect windowRect() const override {
    RECT rc{};
    if (hwnd_) ::GetWindowRect(hwnd_, &rc);
    return Rect{rc.left, rc.top, rc.right, rc.bottom};
  }

  Point clientOrigin() const override {
    POINT pt{0, 0};
    if (hwnd_) ::ClientToScreen(hwnd_, &pt);
    return Point{pt.x, pt.y};
  }

  void show(bool visible) override {
    if (hwnd_) ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
  }

  void invalidate() override {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void invalidateRect(const Rect& rc) override {
    if (!hwnd_ || rc.isEmpty()) return;
    // 右/下边界是开区间，交给 Windows 时就是原值
    RECT r{rc.left, rc.top, rc.right, rc.bottom};
    ::InvalidateRect(hwnd_, &r, FALSE);
  }

  void setBackgroundColor(Color color) override {
    if (eraseBrush_) {
      ::DeleteObject(eraseBrush_);
      eraseBrush_ = nullptr;
    }
    eraseBrush_ = ::CreateSolidBrush(RGB(colorRed(color), colorGreen(color), colorBlue(color)));
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

  void setDelayTimer(int ms) override {
    if (!hwnd_) return;
    // 重新设一个 id 之前先撤掉旧的，否则滚动这类高频触发会攒一堆定时器
    if (delayTimer_) {
      ::KillTimer(hwnd_, kDelayTimerId);
      delayTimer_ = 0;
    }
    if (ms > 0) delayTimer_ = ::SetTimer(hwnd_, kDelayTimerId, static_cast<UINT>(ms), nullptr);
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

  void setTitle(const std::wstring& title) override {
    if (hwnd_) ::SetWindowTextW(hwnd_, title.c_str());
  }

  void minimize() override {
    if (hwnd_) ::ShowWindow(hwnd_, SW_MINIMIZE);
  }

  void toggleMaximize() override {
    if (!hwnd_) return;
    ::ShowWindow(hwnd_, isMaximized() ? SW_RESTORE : SW_MAXIMIZE);
  }

  bool isMaximized() const override { return hwnd_ && ::IsZoomed(hwnd_); }

  // 还原态外框：最大化时 GetWindowPlacement 仍保留还原尺寸，持久化窗口用
  Rect normalBounds() const override {
    if (!hwnd_) return windowRect();
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(hwnd_, &wp)) return windowRect();
    const RECT& rc = wp.rcNormalPosition;
    return Rect{rc.left, rc.top, rc.right, rc.bottom};
  }

  void close() override {
    if (hwnd_) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }

  void startDragMove() override {
    if (!hwnd_) return;
    // 归还捕获后再交给系统，否则拖动由本窗口吃掉
    ::ReleaseCapture();
    ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
  }

  void startDragResize(HitTarget edge) override {
    if (!hwnd_) return;
    ::ReleaseCapture();
    ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, hitCode(edge), 0);
  }

  void setFileDropHandler(
      std::function<void(const std::vector<std::wstring>& paths)> fn) override {
    if (!hwnd_) return;
    fileDropHandler_ = std::move(fn);
    // 只在真的要用时才向系统注册：注册过的窗口会被资源管理器当作投放目标，
    // 光标形态与普通窗口不同
    ::DragAcceptFiles(hwnd_, fileDropHandler_ != nullptr);
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
      auto* self =
          static_cast<WindowsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      if (self) {
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }
      return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<WindowsWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handleMessage(msg, wParam, lParam);
  }

  // 标准做法：先让系统按 WS_CAPTION 算出默认客户区（四边已扣掉边框），
  // 再把顶部整条（边框 + 标题栏）收回给自绘。左右与底部边框仍由系统绘制，
  // 所以投影、圆角、左右下三条边的缩放都不变。
  void adjustClient(RECT& rc, const RECT& windowRect) const {
    if (!style_.topLevel) return;

    // 最大化时窗口被外扩到屏幕外，客户区对齐工作区，免得标题栏被裁
    if (::IsZoomed(hwnd_)) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info)) {
        rc = info.rcWork;
      }
      return;
    }

    // 客户区顶直接对齐窗口顶。用系统算出的矩形相减，可拉伸与固定尺寸两种
    // 边框厚度的差异都不用自己算度量。
    rc.top = windowRect.top;
  }

  // 顶部缩放带：标题栏在顶上，缩放带因此落在内侧，比其它三边宽
  int topResizeWidth() const {
    if (!style_.topLevel || !style_.resizable) return 0;
    const int w = style_.topResizeWidth > 0 ? style_.topResizeWidth : 8;
    return ::MulDiv(w, dpi_, 96);
  }

  LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      // 标准做法：先让系统按 WS_CAPTION 算出默认客户区，再把顶部那条收回自绘。
      // 左右与底部边框仍是非客户区，系统照常绘制边框、阴影并处理缩放。
      case WM_NCCALCSIZE:
        if (style_.topLevel) {
          if (wParam) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const RECT windowRect = params->rgrc[0];
            // 先取系统算好的客户区
            ::DefWindowProcW(hwnd_, msg, wParam, lParam);
            adjustClient(params->rgrc[0], windowRect);
          } else {
            auto* rc = reinterpret_cast<RECT*>(lParam);
            if (rc) {
              const RECT windowRect = *rc;
              ::DefWindowProcW(hwnd_, msg, wParam, lParam);
              adjustClient(*rc, windowRect);
            }
          }
          return 0;
        }
        break;

      // 客户区外的边框与四角归系统（缩放、光标、阴影都不变）；
      // 客户区内交给视图树。顶部边框已收进客户区，所以顶部缩放带要自己补。
      case WM_NCHITTEST:
        if (style_.topLevel) {
          POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
          ::ScreenToClient(hwnd_, &pt);
          RECT client{};
          ::GetClientRect(hwnd_, &client);

          if (!::PtInRect(&client, pt)) {
            clearMaxButtonHover();
            return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
          }

          // 顶部那一条已是客户区，缩放带落在内侧；宽度比其它三边大才好抓
          const int tw = topResizeWidth();
          if (tw > 0 && pt.y >= 0 && pt.y < tw) {
            clearMaxButtonHover();
            const int bw = ::MulDiv(
                style_.borderResizeWidth > 0 ? style_.borderResizeWidth : 4, dpi_, 96);
            if (pt.x < bw) return HTTOPLEFT;
            if (pt.x >= client.right - bw) return HTTOPRIGHT;
            return HTTOP;
          }

          const HitTarget target = host_.onHitTest(Point{pt.x, pt.y});
          if (target == HitTarget::kMaxButton) {
            updateMaxButtonHover(Point{pt.x, pt.y});
            return HTMAXBUTTON;
          }
          clearMaxButtonHover();
          return hitCode(target);
        }
        break;

      // HTMAXBUTTON 只为了让系统在悬停时弹出贴靠浮层。点击不能交给
      // DefWindowProc：它会画一个系统按下态按钮（看起来像多出一个丑按钮），
      // 而且不会真的最大化。这里自己接住，松开时切换最大化。
      case WM_NCLBUTTONDOWN:
        if (style_.topLevel && wParam == HTMAXBUTTON) {
          maxButtonDown_ = true;
          return 0;
        }
        break;

      case WM_NCLBUTTONUP:
        if (style_.topLevel && wParam == HTMAXBUTTON) {
          const bool clicked = maxButtonDown_;
          maxButtonDown_ = false;
          if (clicked) toggleMaximize();
          return 0;
        }
        break;

      // 失活/激活时系统会把标题栏重画一遍（经典蓝框就是这么冒出来的）。
      // 边框本身仍要系统画，所以只重绘非客户区、不重画标题文字。
      case WM_NCACTIVATE:
        if (style_.topLevel) {
          if (lParam != -1) {
            // 让系统绘制边框，但标题栏已归客户区，重画整窗确保自绘内容跟着刷新
            ::RedrawWindow(hwnd_, nullptr, nullptr,
                           RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
          }
          return TRUE;
        }
        break;

      case WM_GETMINMAXINFO:
        if (style_.topLevel && minWidth_ > 0 && minHeight_ > 0) {
          auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
          mmi->ptMinTrackSize.x = ::MulDiv(minWidth_, dpi_, 96);
          mmi->ptMinTrackSize.y = ::MulDiv(minHeight_, dpi_, 96);
          return 0;
        }
        break;

      // 命中码在 lParam 低字。客户区用视图自己的光标，非客户区（系统边框）
      // 交回系统——它会设好缩放光标，我们覆盖只会把缩放箭头变回普通箭头。
      case WM_SETCURSOR:
        if (style_.topLevel && LOWORD(lParam) == HTCLIENT) {
          ::SetCursor(::LoadCursorW(nullptr, cursorId(cursor_)));
          return TRUE;
        }
        break;

      // 非客户区（最大化按钮）离开：只有光标真的出了窗口才清悬停
      case WM_NCMOUSELEAVE:
        if (style_.topLevel) {
          POINT cursor{};
          RECT window{};
          if (::GetCursorPos(&cursor) && ::GetWindowRect(hwnd_, &window) &&
              ::PtInRect(&window, cursor)) {
            return 0;
          }
          clearMaxButtonHover();
          maxButtonDown_ = false;
          return 0;
        }
        break;

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

      case WM_ERASEBKGND: {
        // 窗口刚变大时新增的那圈还没人画过；这里先用背景色填上，
        // 否则 WM_PAINT 之前会露出一帧黑底。
        if (eraseBrush_) {
          RECT rc{};
          ::GetClientRect(hwnd_, &rc);
          ::FillRect(reinterpret_cast<HDC>(wParam), &rc, eraseBrush_);
        }
        return 1;  // 其余仍由自绘负责
      }

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

      case WM_MOUSELEAVE: {
        tracking_ = false;
        // 移到非客户区（最大化按钮、边框）也会收到这条消息。只有光标真的
        // 离开窗口时才整体清悬停，否则刚推上去的高亮会被立刻撤掉。
        POINT cursor{};
        RECT window{};
        bool outside = true;
        if (::GetCursorPos(&cursor) && ::GetWindowRect(hwnd_, &window)) {
          outside = !::PtInRect(&window, cursor);
        }
        if (outside) {
          hoverMaxButton_ = false;
          host_.onMouseLeave();
        }
        return 0;
      }

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
        if (wParam == kDelayTimerId) {
          // 单次：触发后立刻撤掉，避免重复进入
          ::KillTimer(hwnd_, kDelayTimerId);
          delayTimer_ = 0;
          host_.onDelayTimer();
          return 0;
        }
        break;

      case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

      case WM_DROPFILES: {
        if (fileDropHandler_) {
          HDROP drop = reinterpret_cast<HDROP>(wParam);
          const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
          std::vector<std::wstring> paths;
          paths.reserve(count);
          for (UINT i = 0; i < count; ++i) {
            const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len, L'\0');
            ::DragQueryFileW(drop, i, path.data(), len + 1);
            paths.push_back(std::move(path));
          }
          ::DragFinish(drop);
          if (!paths.empty()) fileDropHandler_(paths);
        } else {
          ::DragFinish(reinterpret_cast<HDROP>(wParam));
        }
        return 0;
      }

      case WM_CLOSE:
        // 交给宿主决定：自绘窗口可以拦截（如未保存确认）
        if (host_.onCloseRequest()) return 0;
        break;

      case WM_DESTROY:
        if (blinkTimer_) {
          ::KillTimer(hwnd_, kBlinkTimerId);
          blinkTimer_ = 0;
        }
        if (delayTimer_) {
          ::KillTimer(hwnd_, kDelayTimerId);
          delayTimer_ = 0;
        }
        // 主窗口销毁即整个程序结束；对话框有 owner，不能顺带退出消息循环
        if (style_.topLevel && !owned_) ::PostQuitMessage(0);
        return 0;

      case WM_NCDESTROY:
        // 窗口已销毁：句柄置空，后续调用变成安全的空操作
        hwnd_ = nullptr;
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

  // 最大化按钮交给系统后收不到客户区鼠标消息，悬停状态只能在这里推。
  // WM_NCHITTEST 是鼠标移动时唯一必定经过的钩子。
  void updateMaxButtonHover(const Point& position) {
    hoverMaxButton_ = true;
    if (position.x == hoverPos_.x && position.y == hoverPos_.y) return;
    hoverPos_ = position;
    MouseEvent e;
    e.position = position;
    host_.onMouseMove(e);
  }

  void clearMaxButtonHover() {
    if (!hoverMaxButton_) return;
    hoverMaxButton_ = false;
    host_.onMouseLeave();
  }

  // 顶层窗口的原生图标：任务栏、Alt+Tab 与悬停预览都向系统要它，
  // 不设的话这些地方是空白方块。大图标给预览，小图标给标题栏与任务栏。
  void applyWindowIcons() {
    if (!hwnd_) return;
    const int smallSize = ::GetSystemMetrics(SM_CXSMICON);
    const int bigSize = ::GetSystemMetrics(SM_CXICON);
    const int captureSize = ::GetSystemMetrics(SM_CXSMICON) * 4;

    // SM_CXICON 只是 32；高分屏下预览要更大的图，取一档更大的再让系统缩。
    bigIcon_ = os::windows::loadAppIcon(bigSize > captureSize ? bigSize : captureSize);
    smallIcon_ = os::windows::loadAppIcon(smallSize);

    // 先小后大：ICON_SMALL2 的设置会连带覆盖 ICON_BIG，大图标必须最后写
    if (smallIcon_) {
      ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));
      // Alt+Tab 与任务栏悬停预览优先读 SMALL2；没单设时系统回落到 SMALL
      ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(smallIcon_));
    }
    if (bigIcon_) {
      ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon_));
    }
    // 类图标兜底：某些旧式读取路径只看窗口类
    if (bigIcon_) ::SetClassLongPtrW(hwnd_, GCLP_HICON, reinterpret_cast<LONG_PTR>(bigIcon_));
    if (smallIcon_) ::SetClassLongPtrW(hwnd_, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(smallIcon_));
  }

  static void registerClass(const std::wstring& name, HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &WindowsWindow::proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name.c_str();
    ::RegisterClassExW(&wc);
  }

  WindowHost& host_;
  HWND hwnd_ = nullptr;
  int dpi_ = 96;
  bool tracking_ = false;
  // 最大化按钮命中 HTMAXBUTTON 后归系统管，这里自己记住它的悬停状态
  bool hoverMaxButton_ = false;
  // 按下中途离开再回来才算点击，和普通按钮一致
  bool maxButtonDown_ = false;
  Point hoverPos_{-1, -1};
  UINT_PTR blinkTimer_ = 0;
  UINT_PTR delayTimer_ = 0;
  CursorShape cursor_ = CursorShape::kArrow;
  WindowStyle style_;
  // 有 owner 的顶层窗口是对话框，销毁时不该结束消息循环
  bool owned_ = false;
  // 离屏位图跨帧复用，避免每次重绘都重建几十 MB 的图
  CanvasCache canvasCache_;
  // 原生窗口图标由系统持有引用，销毁窗口前不能释放
  HICON bigIcon_ = nullptr;
  HICON smallIcon_ = nullptr;
  // 擦背景用的刷子；窗口变大时先铺底色，避免露出黑底一帧
  HBRUSH eraseBrush_ = nullptr;
  // 拖入文件的回调；非空时才向系统注册接受拖放
  std::function<void(const std::vector<std::wstring>& paths)> fileDropHandler_;
  // 最小尺寸（96 DPI 设计值），0 表示不限制
  int minWidth_ = 0;
  int minHeight_ = 0;
};

namespace {

class WindowsPlatform : public Platform {
 public:
  PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                               WindowHost& host) override {
    auto window = std::make_unique<WindowsWindow>(host);
    if (!window->create(static_cast<HWND>(parent), params, instance_)) return nullptr;
    WindowsWindow* raw = window.get();
    windows_[raw] = std::move(window);
    return raw;
  }

  void destroyWindow(PlatformWindow* window) override {
    windows_.erase(static_cast<WindowsWindow*>(window));
  }

  int screenDpi() override {
    HDC dc = ::GetDC(nullptr);
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
  }

  const wchar_t* defaultFontFamily() override { return L"Microsoft YaHei UI"; }

  Image appIcon() override {
    // 小图标：与系统标题栏一致，取进程可执行文件的图标。
    // 位图交给 loadedImages 托管，随 releaseIconCaches 一起释放。
    if (appIconLoaded_) return appIcon_;
    appIconLoaded_ = true;

    wchar_t path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return appIcon_;

    // 注意：small 是 Windows 头里的宏，不能当变量名
    HICON icon = nullptr;
    if (::ExtractIconExW(path, 0, nullptr, &icon, 1) > 0 && icon) {
      appIcon_ = os::windows::imageFromIcon(icon);
      ::DestroyIcon(icon);
    }
    return appIcon_;
  }

  bool setClipboardText(const std::wstring& text) override {
    if (!::OpenClipboard(nullptr)) return false;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool ok = false;
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
      if (void* dst = ::GlobalLock(memory)) {
        memcpy(dst, text.c_str(), bytes);
        ::GlobalUnlock(memory);
        // 成功后所有权归剪贴板，不能再 GlobalFree
        ok = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        if (!ok) ::GlobalFree(memory);
      } else {
        ::GlobalFree(memory);
      }
    }
    ::CloseClipboard();
    return ok;
  }

  std::wstring clipboardText() override {
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return L"";
    if (!::OpenClipboard(nullptr)) return L"";
    std::wstring data;
    if (HANDLE handle = ::GetClipboardData(CF_UNICODETEXT)) {
      if (const auto* text = static_cast<const wchar_t*>(::GlobalLock(handle))) {
        data = text;
        ::GlobalUnlock(handle);
      }
    }
    ::CloseClipboard();
    return data;
  }

  bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) override {
    // 按分隔符边界判断，避免 "a2" 误配 "a"
    if (prefix.empty()) return true;
    if (path.size() < prefix.size()) return false;
    if (::CompareStringOrdinal(path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                               static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
      return false;
    }
    if (path.size() == prefix.size()) return true;
    if (prefix.back() == L'\\' || prefix.back() == L'/') return true;
    return path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
  }

  wchar_t pathSeparator() override { return L'\\'; }
  int compareText(const std::wstring& a, const std::wstring& b) override {
    // 用系统的忽略大小写比较：与资源管理器的排序观感一致
    const int r = ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                         static_cast<int>(b.size()), TRUE);
    if (r == CSTR_LESS_THAN) return -1;
    if (r == CSTR_GREATER_THAN) return 1;
    return 0;
  }
  bool containsTextIgnoreCase(const std::wstring& text, const std::wstring& needle) override {
    if (needle.empty()) return true;
    return ::StrStrIW(text.c_str(), needle.c_str()) != nullptr;
  }
  bool equalsPath(const std::wstring& a, const std::wstring& b) override {
    // Windows 盘符与扩展名不区分大小写，用系统排序规则比较
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
  }

  Font createFont(const wchar_t* family, float pointSize, bool bold, int dpi) override {
    return os::windows::makeFont(family, pointSize, bold, dpi);
  }

  void destroyFont(void* handle) override { os::windows::releaseFont(handle); }

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

  int advanceText(const std::wstring& text, const Font& font) override {
    if (text.empty() || !font.isValid()) return 0;
    auto* g = measureGraphics();
    auto* f = static_cast<Gdiplus::Font*>(font.handle());
    if (!g || !f) return 0;
    const INT n = static_cast<INT>(text.size());

    // 绘制走默认格式，GDI+ 会在左右各补 em/6 的排版留白，墨迹并不从布局左缘起笔。
    // 插入点必须落在同一坐标系里：少算留白会压住末字，多算又离得太远。
    Gdiplus::StringFormat drawFormat;
    drawFormat.SetAlignment(Gdiplus::StringAlignmentNear);
    drawFormat.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                              Gdiplus::StringFormatFlagsNoClip |
                              Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF box(0, 0, static_cast<Gdiplus::REAL>(n + 1) * 4096.0f, 4096.0f);
    Gdiplus::RectF loose;
    g->MeasureString(text.c_str(), n, f, box, &drawFormat, &loose);

    // 排版格式无留白，宽度即纯字形推进
    Gdiplus::StringFormat tight(Gdiplus::StringFormat::GenericTypographic());
    Gdiplus::RectF narrow;
    g->MeasureString(text.c_str(), n, f, box, &tight, &narrow);

    // 两种格式只差两侧留白，取差值一半就是左侧那一份
    const float inset = (loose.Width > narrow.Width) ? (loose.Width - narrow.Width) * 0.5f : 0.0f;
    return static_cast<int>(inset + narrow.Width + 0.5f);
  }

  // 整串一次排版，向 GDI+ 要每字符的布局范围。光标与绘制必须共用这一套
  // 坐标：逐个前缀去量（advanceText）会把每段各自的排版留白累加进来，
  // 十几个字符就能偏出几像素，文本越长插入点离末字越远。
  std::vector<int> glyphBounds(const std::wstring& text, const Font& font) override {
    const int n = static_cast<int>(text.size());
    if (n <= 0) return {};
    auto* g = measureGraphics();
    auto* f = static_cast<Gdiplus::Font*>(font.handle());
    if (!g || !f) return {};

    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    // GDI+ 一次最多接受 32 个可测范围，超出的整串会退化成一个大矩形。
    // 分段测，段起点用前一段的排布宽度推进，误差只落在段边界上。
    constexpr int kMaxRanges = 32;
    std::vector<int> out(static_cast<size_t>(n) * 2, 0);
    Gdiplus::REAL originX = 0.0f;
    std::vector<Gdiplus::CharacterRange> ranges(kMaxRanges);
    std::vector<Gdiplus::Region> regions(kMaxRanges);

    for (int start = 0; start < n; start += kMaxRanges) {
      const int count = (std::min)(kMaxRanges, n - start);
      for (int i = 0; i < count; ++i) ranges[static_cast<size_t>(i)] = Gdiplus::CharacterRange(i, 1);
      fmt.SetMeasurableCharacterRanges(count, ranges.data());

      // 矩形给足宽高，免得排版折行或裁掉后面的字符
      const Gdiplus::RectF box(0, 0, static_cast<Gdiplus::REAL>(count + 1) * 4096.0f, 4096.0f);
      if (g->MeasureCharacterRanges(text.c_str() + start, count, f, box, &fmt, count,
                                    regions.data()) != Gdiplus::Ok) {
        return {};
      }

      for (int i = 0; i < count; ++i) {
        Gdiplus::RectF bounds;
        // GetBounds 要显式传目标矩形，单参数版本不存在
        if (regions[static_cast<size_t>(i)].GetBounds(&bounds, g) != Gdiplus::Ok) return {};
        const size_t at = static_cast<size_t>(start + i) * 2;
        out[at] = static_cast<int>(std::lround(originX + bounds.GetLeft()));
        out[at + 1] = static_cast<int>(std::lround(originX + bounds.GetRight()));
      }

      // 段的推进量就是本段末字符的布局右缘；最后一段之后不再需要
      if (start + count < n) {
        Gdiplus::RectF tail;
        if (regions[static_cast<size_t>(count) - 1].GetBounds(&tail, g) != Gdiplus::Ok) return {};
        originX += tail.GetRight();
      }
    }
    return out;
  }

  Image loadImage(const std::wstring& path) override { return loadImageFile(path); }

  Size imageSize(const Image& image) override { return os::windows::imagePixelSize(image); }

  void releaseImage(Image image) override { os::windows::releaseLoadedImage(image); }

  void startupDrawing() override { os::windows::ensureGdiplus(); }

  void shutdownDrawing() override {
    os::windows::releaseIconCaches();
    os::windows::releaseGdiplus();
  }

  bool isDrawingReady() override { return os::windows::ensureGdiplus(); }

  int64_t monotonicMillis() override {
    // 与窗口消息队列同源的计时器，不受系统时间调整影响
    return static_cast<int64_t>(::GetTickCount64());
  }

  // ── 平台能力：转给 platform_windows.cpp ──
  Rect workArea() override {
    RECT work{};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      work = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    }
    return Rect{work.left, work.top, work.right, work.bottom};
  }

  int runEventLoop() override {
    // 快捷键由 ViewRoot 的按键回调接管，这里只管派发。
    // 不用 IsDialogMessage：它会把回车吃掉，自绘控件收不到。
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
  }

  void quitEventLoop(int exitCode) override { ::PostQuitMessage(exitCode); }

  std::wstring appDataDir(const std::wstring& appName) override {
    return os::windows::appDataDirImpl(appName);
  }
  std::vector<DirEntry> listDirectory(const std::wstring& dir, bool skipHidden) override {
    return os::windows::listDirectoryImpl(dir, skipHidden);
  }
  bool pathExists(const std::wstring& path) override {
    return os::windows::pathExistsImpl(path);
  }
  bool isDirectory(const std::wstring& path) override {
    return os::windows::isDirectoryImpl(path);
  }
  bool createDirectory(const std::wstring& path) override {
    return os::windows::createDirectoryImpl(path);
  }
  uint64_t fileSize(const std::wstring& path) override {
    return os::windows::fileSizeImpl(path);
  }
  int64_t modifiedTime(const std::wstring& path) override {
    return os::windows::modifiedTimeImpl(path);
  }
  bool setModifiedTime(const std::wstring& path, int64_t millis) override {
    return os::windows::setModifiedTimeImpl(path, millis);
  }
  std::string readFile(const std::wstring& path, size_t maxBytes) override {
    return os::windows::readFileImpl(path, maxBytes);
  }
  bool writeFile(const std::wstring& path, const std::string& data) override {
    return os::windows::writeFileImpl(path, data);
  }
  bool deleteFile(const std::wstring& path) override {
    return os::windows::deleteFileImpl(path);
  }
  std::unique_ptr<FileStream> openFileStream(const std::wstring& path, bool openWrite) override {
    return os::windows::openFileStreamImpl(path, openWrite);
  }
  bool moveFile(const std::wstring& from, const std::wstring& to) override {
    return os::windows::moveFileImpl(from, to);
  }
  std::wstring absolutePath(const std::wstring& path) override {
    return os::windows::absolutePathImpl(path);
  }
  std::wstring executablePath() override { return os::windows::executablePathImpl(); }
  std::vector<std::wstring> commandLineArgs() override {
    return os::windows::commandLineArgsImpl();
  }
  std::wstring tempDir() override { return os::windows::tempDirImpl(); }
  uint32_t processId() override { return os::windows::processIdImpl(); }
  bool moveToTrash(const std::wstring& path) override {
    return os::windows::moveToTrashImpl(path);
  }
  std::wstring specialFolder(SpecialFolder folder) override {
    return os::windows::specialFolderImpl(folder);
  }
  std::vector<DriveInfo> listDrives() override { return os::windows::listDrivesImpl(); }

  MessageBoxResult showMessageBox(NativeHandle owner, const MessageBoxParams& params) override {
    return os::windows::showMessageBoxImpl(owner, params);
  }
  bool canOwnModal(NativeHandle window) override {
    return os::windows::canOwnModalImpl(window);
  }
  int showPopupMenu(NativeHandle owner, const Point& position,
                    const std::vector<MenuItem>& items) override {
    return os::windows::showPopupMenuImpl(owner, position, items);
  }
  Point cursorPosition() override { return os::windows::cursorPositionImpl(); }
  void bringToFront(NativeHandle window) override { os::windows::bringToFrontImpl(window); }

  std::wstring registerContextMenu(const std::vector<ContextMenuEntry>& entries) override {
    return os::windows::registerContextMenuImpl(entries);
  }
  std::wstring unregisterContextMenu(const std::vector<ContextMenuEntry>& entries) override {
    return os::windows::unregisterContextMenuImpl(entries);
  }
  bool isContextMenuRegistered(const std::vector<ContextMenuEntry>& entries) override {
    return os::windows::isContextMenuRegisteredImpl(entries);
  }

  bool isUiThread() override { return os::windows::isUiThreadImpl(); }
  void sleepMillis(int ms) override { os::windows::sleepMillisImpl(ms); }
  float scaleFactor() override { return os::windows::scaleFactorImpl(); }

  std::wstring pickFolder(NativeHandle owner, const std::wstring& title,
                          const std::wstring& initialDir) override {
    return os::windows::pickFolderImpl(owner, title, initialDir);
  }
  std::wstring pickSaveFile(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir, const std::wstring& defaultName,
                            const std::wstring& defaultExt,
                            const std::vector<std::wstring>& filters) override {
    return os::windows::pickSaveFileImpl(owner, title, initialDir, defaultName, defaultExt,
                                         filters);
  }
  std::wstring pickOpenFile(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir,
                            const std::vector<std::wstring>& filters) override {
    return os::windows::pickOpenFileImpl(owner, title, initialDir, filters);
  }

  bool openWithSystem(const std::wstring& path) override {
    return os::windows::openWithSystemImpl(path);
  }
  bool showProperties(NativeHandle owner, const std::wstring& path) override {
    return os::windows::showPropertiesImpl(owner, path);
  }

  void postToUiThread(std::function<void()> fn) override {
    os::windows::postToUiThread(std::move(fn));
  }

  void invokeOnUiThread(std::function<void()> fn) override {
    os::windows::invokeOnUiThread(std::move(fn));
  }

  bool claimSingleInstance(const std::wstring& appId) override {
    return os::windows::claimSingleInstanceImpl(appId);
  }
  void setLaunchPayloadHandler(std::function<void(const std::wstring& payload)> fn) override {
    os::windows::setLaunchPayloadHandlerImpl(std::move(fn));
  }
  bool forwardToRunningInstance(const std::wstring& appId,
                                const std::wstring& payload) override {
    return os::windows::forwardToRunningInstanceImpl(appId, payload);
  }

 private:
  HINSTANCE instance_ = ::GetModuleHandleW(nullptr);
  std::map<WindowsWindow*, std::unique_ptr<WindowsWindow>> windows_;
  // 本进程图标：只取一次
  bool appIconLoaded_ = false;
  Image appIcon_;
};

WindowsPlatform g_platform;
bool g_started = false;
// 测试替换的平台后端；非空时所有 platform() 调用都走它
Platform* g_testPlatform = nullptr;

}  // namespace

Platform* backendPlatform() { return &g_platform; }

Platform* testPlatformOverride() { return g_testPlatform ? g_testPlatform : &g_platform; }

void markTestOverride(Platform* platform) { g_testPlatform = platform; }

void markStarted(bool started) { g_started = started; }
bool hasStarted() { return g_started; }

}  // namespace windows
}  // namespace os

Platform* platform() { return os::windows::testPlatformOverride(); }

// 测试替换：非空时优先于内置后端；空则回到底层 Windows
void setPlatformForTesting(Platform* platform) {
  os::windows::markTestOverride(platform);
}

void startupPlatform() {
  if (os::windows::hasStarted()) return;
  os::windows::ensureGdiplus();
  os::windows::markStarted(true);
}

void shutdownPlatform() {
  if (!os::windows::hasStarted()) return;
  os::windows::releaseIconCaches();
  os::windows::releaseGdiplus();
  os::windows::markStarted(false);
}

bool isPlatformReady() { return os::windows::hasStarted(); }

void initHostProcess() {
  // 逐显示器 DPI 感知：必须在创建任何窗口前设置，否则系统按 96 DPI 缩放
  using SetCtxFn = BOOL(WINAPI*)(void*);
  static auto setCtx = reinterpret_cast<SetCtxFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (!setCtx || !setCtx(reinterpret_cast<void*>(-4))) ::SetProcessDPIAware();

  // 公共控件：原生对话框里的进度条、列表等依赖它
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                               ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                               ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  ::InitCommonControlsEx(&icc);

  // 工作线程回传 UI 用的收件窗口
  os::windows::prepareUiThreadDispatch();
}

void reportFatalError(const wchar_t* message) {
  ::MessageBoxW(nullptr, message, L"错误", MB_OK | MB_ICONERROR);
}

void runModalLoop(NativeHandle modal, NativeHandle owner) {
  HWND dialog = static_cast<HWND>(modal);
  HWND ownerWnd = static_cast<HWND>(owner);
  // 只有本进程的窗口才能当模态 owner，否则异常退出会锁死外部界面
  if (ownerWnd) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(ownerWnd, &pid);
    if (!::IsWindow(ownerWnd) || pid != ::GetCurrentProcessId()) ownerWnd = nullptr;
  }
  if (ownerWnd) ::EnableWindow(ownerWnd, FALSE);

  MSG msg;
  while (::IsWindow(dialog) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  if (ownerWnd) {
    ::EnableWindow(ownerWnd, TRUE);
    ::SetActiveWindow(ownerWnd);
  }
}

}  // namespace AiUI
