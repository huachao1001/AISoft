// Win32 窗口后端
//
// 四个关键决定：
//
// 1. **客户区画布是 DIB + 共享光栅器**，不是直接往 HDC 上画。
//    直接用 GDI 画图元会让屏幕与离线渲染走两条实现，像素断言就只能对着
//    「另一条路径」验收。用 CreateDIBSection 拿一块 32 位内存位图，
//    把 SoftwareBackend 画进去，再 BitBlt 上屏——屏幕与内存是同一段代码，
//    DESIGN.md 第 2 节那条「两条渲染路径」才真正成立。
//
// 2. **非客户区靠 WM_NCCALCSIZE / WM_NCHITTEST 让出来**，不是靠
//    「去掉标题栏再自己画」。前者保住了系统的贴边吸附、双击最大化、
//    Snap Layouts、以及窗口投影与圆角；后者会把这些全部弄丢，还得自己
//    实现一套多显示器 DPI 切换。
//
// 3. **一个静态 WndProc + GWLP_USERDATA**，不用 SetWindowSubclass。
//    窗口类在进程内共享，带 this 的 WndProc 会把第一个窗口的对象指针
//    泄漏给所有窗口；子类化能解决但要额外链 comctl32，而且 WM_NCCREATE
//    时机还得单独处理。GWLP_USERDATA 在 WM_NCCREATE 里写一次就够。
//
// 4. **脏区用 InvalidateRect 攒批**，在 WM_PAINT 里一次画完。
//    悬停、光标闪烁都是小块区域，逐条重绘会让 60fps 变成不可能。
#include "pyui/platform/window.h"

#include "pyui/canvas.h"
#include "pyui/platform/platform.h"
#include "pyui/software_backend.h"
#include "pyui/surface.h"
#include "pyui/text_renderer.h"

#include "win_common.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace pyui {
namespace {

constexpr const wchar_t* kWindowClassName = L"PyUIWindow";

// 光标闪烁是整棵树共享的周期定时器；「稍后做一次」是单次定时器。
// 两者共用一个窗口，必须各用独立的 id，否则 KillTimer 会互相打掉。
constexpr UINT_PTR kBlinkTimerId = 1;
constexpr UINT_PTR kDelayTimerId = 2;
// 悬停监视：只在最大化按钮处于悬停态时开，指针一离开就停。
constexpr UINT_PTR kHoverWatchTimerId = 3;
constexpr UINT kHoverWatchIntervalMs = 60;

Rect rectFromWin32(const RECT& rc) {
  return Rect{rc.left, rc.top, rc.right, rc.bottom};
}

// Win32 虚拟键 → 中性 Key。字母与数字靠 vk 码连续区间换算，
// 免得写 26+10 条 case；其余逐个映射。
Key keyFromVirtualKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::kA) + (vk - 'A'));
  if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::k0) + (vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12) {
    return static_cast<Key>(static_cast<int>(Key::kF1) + (vk - VK_F1));
  }

  switch (vk) {
    case VK_BACK:
      return Key::kBackspace;
    case VK_TAB:
      return Key::kTab;
    case VK_RETURN:
      return Key::kEnter;
    case VK_ESCAPE:
      return Key::kEscape;
    case VK_SPACE:
      return Key::kSpace;
    case VK_INSERT:
      return Key::kInsert;
    case VK_DELETE:
      return Key::kDelete;
    case VK_HOME:
      return Key::kHome;
    case VK_END:
      return Key::kEnd;
    case VK_PRIOR:
      return Key::kPageUp;
    case VK_NEXT:
      return Key::kPageDown;
    case VK_LEFT:
      return Key::kLeft;
    case VK_UP:
      return Key::kUp;
    case VK_RIGHT:
      return Key::kRight;
    case VK_DOWN:
      return Key::kDown;
    default:
      return Key::kNone;
  }
}

Modifiers modifiersFromWin32() {
  Modifiers mods;
  // GetKeyState 取「当前状态」，比跟踪 WM_KEYDOWN/UP 可靠：按住 Shift
  // 点鼠标时我们可能先收到移动消息，此时按键消息可能被别的窗口吃掉了。
  mods.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  mods.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  mods.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
  mods.meta = (::GetKeyState(VK_LWIN) & 0x8000) != 0 ||
              (::GetKeyState(VK_RWIN) & 0x8000) != 0;
  return mods;
}

MouseButton buttonFromMessage(UINT message, WPARAM wparam) {
  switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
      return MouseButton::kLeft;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
      return MouseButton::kRight;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
      return MouseButton::kMiddle;
    default:
      break;
  }
  // WM_XBUTTON* 把按键号放在 wParam 高位；侧键只区分前两个，
  // 多出来的按键在多数界面里没有语义，按未按下处理。
  if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) {
    return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MouseButton::kLeft : MouseButton::kRight;
  }
  return MouseButton::kNone;
}HCURSOR cursorForShape(CursorShape shape) {
  // 一律用系统光标：自己画的在别的窗口上方会显得格格不入，
  // 而且系统光标已经处理了高对比度与不同 DPI。
  LPWSTR id = IDC_ARROW;
  switch (shape) {
    case CursorShape::kArrow:
      id = IDC_ARROW;
      break;
    case CursorShape::kHand:
      id = IDC_HAND;
      break;
    case CursorShape::kSizeHorizontal:
      id = IDC_SIZEWE;
      break;
    case CursorShape::kSizeVertical:
      id = IDC_SIZENS;
      break;
    case CursorShape::kIBeam:
      id = IDC_IBEAM;
      break;
    case CursorShape::kGrab:
    case CursorShape::kGrabbing:
      // 系统没有「张开的手」，用四向箭头表达「可拖动」。
      // 手掌光标若自己画，在 DPI 切换时还要重做一套位图，不值得。
      id = IDC_SIZEALL;
      break;
  }
  return ::LoadCursorW(nullptr, id);
}

// HitTarget → 非客户区命中码。
// 只用于缩放边与标题栏；kClient / kMaxButton 单独处理，
// 因为它们的语义不是「给系统一个 NC 码」那么简单。
bool hitTargetToNcCode(HitTarget target, WPARAM& code) {
  switch (target) {
    case HitTarget::kCaption:
      code = HTCAPTION;
      return true;
    case HitTarget::kLeft:
      code = HTLEFT;
      return true;
    case HitTarget::kRight:
      code = HTRIGHT;
      return true;
    case HitTarget::kTop:
      code = HTTOP;
      return true;
    case HitTarget::kBottom:
      code = HTBOTTOM;
      return true;
    case HitTarget::kTopLeft:
      code = HTTOPLEFT;
      return true;
    case HitTarget::kTopRight:
      code = HTTOPRIGHT;
      return true;
    case HitTarget::kBottomLeft:
      code = HTBOTTOMLEFT;
      return true;
    case HitTarget::kBottomRight:
      code = HTBOTTOMRIGHT;
      return true;
    default:
      return false;
  }
}

// 一块可复用的 32 位 DIB。
//
// 为什么不每帧新建：1080p 一张就是 8 MB，60fps 下每秒分配释放 480 MB，
// 分配器撑得住但毫无意义。DIB 只会「变大不会变小」：窗口缩小时留着旧的
// 大缓冲，最多浪费一次窗口历史最大尺寸的内存。
struct DibCache {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  void* bits = nullptr;
  int width = 0;
  int height = 0;
  // 每行字节数。32 位 DIB 按 4 字节对齐，等于 width * 4；
  // 单独存是因为 Surface 需要它，将来换 24 位时这里改一处就够。
  int stride = 0;

  ~DibCache() { release(); }

  // 保证至少能放下 w×h。失败时缓存保持原样（可能是空）。
  bool ensure(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (dc != nullptr && width >= w && height >= h) return true;
    release();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    // 负高度 = 自上而下的 DIB，与 Surface 的行序一致，省掉一次翻转拷贝。
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    // 参考 DC 传 nullptr：DIB 与设备无关，CreateCompatibleDC(nullptr)
    // 得到的就是一个可以选入 DIB 的内存 DC。
    HDC screen = ::GetDC(nullptr);
    bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (screen != nullptr) ::ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || pixels == nullptr) {
      release();
      return false;
    }

    dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
      release();
      return false;
    }
    ::SelectObject(dc, bitmap);

    bits = pixels;
    width = w;
    height = h;
    stride = w * static_cast<int>(sizeof(std::uint32_t));
    return true;
  }

  void release() {
    if (dc != nullptr) {
      ::DeleteDC(dc);
      dc = nullptr;
    }
    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
      bitmap = nullptr;
    }
    bits = nullptr;
    width = 0;
    height = 0;
    stride = 0;
  }
};

// 一次绘制的目标：DIB + 共享光栅器，析构时 BitBlt 上屏。
//
// 为什么用 DIB 而不是 GDI 直接画：屏幕与离线渲染必须走同一个
// SoftwareBackend，否则「同一棵树的像素在窗口里和在内存里一样」这条
// 就无从验证。DIB 给了一块可以直接当 Surface 用的 32 位内存。
class WindowCanvas final : public Canvas {
 public:
  WindowCanvas(DibCache& cache, HDC target, bool ownsTarget, const Rect& area, Color background,
               TextRenderer* renderer)
      : target_(target), ownsTarget_(ownsTarget), area_(area) {
    const int w = std::max(0, area.width());
    const int h = std::max(0, area.height());
    if (w == 0 || h == 0 || target_ == nullptr) return;
    if (!cache.ensure(w, h)) return;

    // DIB 里那一块内存直接当 Surface：不拷贝、每帧零分配。
    // stride 用缓存的真实行宽而不是 w*4——缓存可能比 area 大，
    // 此时第 y 行必须从 base + y*stride 取。
    surface_ = std::make_unique<Surface>(
        Surface::wrapExternal(static_cast<std::uint32_t*>(cache.bits), w, h, cache.stride));
    if (!surface_->valid()) {
      surface_.reset();
      return;
    }

    // 先铺不透明底色。DIB 的初值是垃圾（首次使用）或上一帧的内容（复用），
    // 而 BitBlt 是 SRCCOPY、不看 alpha，透明像素会变成黑色。
    // 强制 alpha=255：这一层要当作「已经画好的成品」贴到窗口上。
    surface_->clear(background | 0xFF000000u);

    width_ = w;
    height_ = h;
    memoryDc_ = cache.dc;

    backend_ = std::make_unique<SoftwareBackend>(*surface_);
    backend_->setTextRenderer(renderer);
    painter_ = std::make_unique<Painter>(*backend_);
    // Painter 的原点对齐到 area 左上角：调用方（ViewRoot）用的是客户区坐标，
    // 这里把客户区原点搬到画布原点。
    painter_->translate(-area_.left, -area_.top);
  }

  ~WindowCanvas() override {
    present();
    if (ownsTarget_ && target_ != nullptr) {
      ::ReleaseDC(nullptr, target_);
    }
  }

  Painter& painter() override { return *painter_; }
  Rect area() const override { return area_; }

  void present() override {
    if (presented_ || memoryDc_ == nullptr || target_ == nullptr) return;
    presented_ = true;
    // 只贴 area 那一块：画布可能只覆盖一个小脏区，整块 BitBlt 会把
    // 没重画的区域一起推翻，悬停时就会看到闪烁。
    ::BitBlt(target_, area_.left, area_.top, width_, height_, memoryDc_, 0, 0, SRCCOPY);
  }

  bool valid() const { return painter_ != nullptr; }

 private:
  HDC target_ = nullptr;
  bool ownsTarget_ = false;
  Rect area_{};
  int width_ = 0;
  int height_ = 0;
  HDC memoryDc_ = nullptr;
  std::unique_ptr<Surface> surface_;
  std::unique_ptr<SoftwareBackend> backend_;
  std::unique_ptr<Painter> painter_;
  bool presented_ = false;
};

// DWM 渐进式入口：Win7 起都有，但为了保持 exe 的单文件性不做静态链接。
// 用 GetProcAddress 探测，取不到就跳过——没有投影只是观感差一点，
// 而静态链接 dwmapi.lib 会让最小可执行文件多一个导入项。
using DwmExtendFrameIntoClientAreaFn = HRESULT(WINAPI*)(HWND, const MARGINS*);
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

// DWMWA_USE_IMMERSIVE_DARK_MODE 在 SDK 头文件里直到 10.0.19041 才有，
// 老 SDK 编不过是常态，所以用字面值（20）而不是宏——这个值自 1809 起没变过。
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

void applyDwmAppearance(HWND hwnd) {
  static const auto extendFrame = reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(
      ::GetProcAddress(::GetModuleHandleW(L"dwmapi.dll"), "DwmExtendFrameIntoClientArea"));
  static const auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
      ::GetProcAddress(::GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));

  if (extendFrame != nullptr) {
    // 留 1px 不可见的非客户区，DWM 才会给窗口画投影与 Win11 的圆角。
    // 完全不要非客户区的窗口是「无影」的，看起来像一张贴纸。
    const MARGINS margins{1, 1, 1, 1};
    extendFrame(hwnd, &margins);
  }

  if (setAttribute != nullptr) {
    // 深色模式下让系统画的 1px 边框也跟着变深。不改的话亮色细边框
    // 会围着一圈暗底，在深色桌面上很扎眼。
    const BOOL dark = win32::isSystemDarkMode() ? TRUE : FALSE;
    setAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
  }
}

// 平台窗口实现。
class Win32Window final : public PlatformWindow {
 public:
  Win32Window(NativeHandle parent, const WindowParams& params, WindowHost& host);
  ~Win32Window() override;

  // ── PlatformWindow ──
  NativeHandle handle() const override { return hwnd_; }
  Rect clientRect() const override;
  int dpi() const override { return dpi_; }
  std::unique_ptr<Canvas> createCanvas(const Rect& area) override;
  void setBounds(const Rect& bounds) override;
  Rect windowRect() const override;
  Point clientOrigin() const override;
  void show(bool visible) override;
  void invalidate() override;
  void invalidateRect(const Rect& rc) override;
  void setBackgroundColor(Color color) override;
  void setFocus() override;
  void setCursorShape(CursorShape shape) override;
  void captureMouse(bool capture) override;
  void setBlinkTimer(bool on) override;
  void setDelayTimer(int ms) override;
  void setImeCaret(int x, int y) override;
  void setTitle(const std::string& title) override;
  void minimize() override;
  void toggleMaximize() override;
  bool isMaximized() const override;
  void setFullscreen(bool fullscreen) override;
  bool isFullscreen() const override { return fullscreen_; }
  Rect normalBounds() const override;
  void close() override;
  void startDragMove() override;
  void startDragResize(HitTarget edge) override;
  void setFileDropHandler(std::function<void(const std::vector<std::string>&)> fn) override;

 private:
  // 窗口过程。静态是因为窗口类共享；this 从 GWLP_USERDATA 取。
  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT processMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  // 96 DPI 设计值 → 屏幕像素
  int px(int designValue) const;

  void refreshDpi();
  void paint();
  MouseEvent makeMouseEvent(LPARAM lparam) const;
  MouseEvent makeWheelEvent(WPARAM wparam, LPARAM lparam) const;
  // 客户区坐标 → 非客户区命中（标题栏/边框）
  WPARAM hitTest(const Point& clientPoint) const;
  // 自绘标题栏模式：客户区覆盖整个窗口
  bool usesCustomFrame() const { return style_.topLevel && style_.captionHeight > 0; }
  void trackMouseLeave();
  void beginDrag(WPARAM ncCode);
  // 最大化按钮在非客户区，系统只负责悬停预览，点击要自己接管。
  // 参考 AIUI：NCHITTEST 命中时转发一次 MouseMove 让按钮进入悬停态，
  // 离开窗口时清掉，否则悬停底色会一直留着。
  void updateMaxButtonHover(const Point& position);
  void clearMaxButtonHover();
  // 悬停期间的低频复核：离开通知不能作为唯一依据（一次性、可能被排队消息
  // 重新点亮）。实时光标不再指着按钮就清掉。
  void checkMaxButtonHover();
  void startHoverWatch();
  void stopHoverWatch();
  // 实时光标 → 客户区坐标；光标不在客户区内返回 false。
  bool realCursorClient(Point* out) const;

  HWND hwnd_ = nullptr;
  HWND parent_ = nullptr;
  WindowHost& host_;
  WindowStyle style_;
  int dpi_ = 96;
  int minWidth_ = 0;
  int minHeight_ = 0;
  bool fullscreen_ = false;
  bool trackingLeave_ = false;
  // 进入全屏前的样式与位置，退出时恢复
  WINDOWPLACEMENT savedPlacement_{};
  LONG_PTR savedStyle_ = 0;
  HCURSOR cursor_ = nullptr;
  Color backgroundColor_ = makeColor(0xff, 0xff, 0xff);
  TextRenderer* textRenderer_ = nullptr;
  std::function<void(const std::vector<std::string>&)> dropHandler_;
  DibCache dib_;
  // WM_PAINT 期间由 createCanvas 复用的 DC。用 BeginPaint 给的 DC 而不是
  // 再 GetDC 一次：后者在 WM_PAINT 里画会让更新区校验出现闪屏。
  HDC activePaintDc_ = nullptr;
  // 本次 onExpose 里宿主是否真的取过画布。没取就由后端补一层底色，
  // 否则更新区会留着上一帧或未初始化内容。
  bool paintedDuringExpose_ = false;
  // WM_PAINT 重入保护：onExpose 里再触发一次绘制会让脏区永远清不掉。
  bool painting_ = false;
  // 高/低代理缓存：BMP 之外的字符在 UTF-16 里是两段 WM_CHAR
  wchar_t highSurrogate_ = 0;
  // 输入法候选框的位置（客户区坐标）
  Point imeCaret_{};

  // ── 最大化按钮的 NC 状态 ──
  //
  // 最大化按钮返回 HTMAXBUTTON 是为了让系统弹贴靠布局（Snap Layouts），
  // 代价是 WM_LBUTTONDOWN 收不到——系统把点击当非客户区事件处理。AIUI
  // 的做法是自己接管按下与抬起：系统只负责悬停时的预览弹窗，点击结果由
  // 我们决定。少了这两条，窗口最大化按钮点了没反应。
  bool maxButtonDown_ = false;
  bool hoverMaxButton_ = false;
  // 上一次转发给宿主的非客户区悬停位置：同样的坐标不重复发，
  // 否则每次 WM_NCHITTEST 都会推一次 MouseMove，悬停态反复重算。
  Point hoverMaxPos_{-1, -1};
};

// ─────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────

Win32Window::Win32Window(NativeHandle parent, const WindowParams& params, WindowHost& host)
    : parent_(static_cast<HWND>(parent)), host_(host), style_(params.style) {
  minWidth_ = std::max(0, params.minWidth);
  minHeight_ = std::max(0, params.minHeight);
  cursor_ = ::LoadCursorW(nullptr, IDC_ARROW);

  const HINSTANCE instance = win32::moduleHandle();
  HICON icon = params.iconResourceId != 0
                   ? ::LoadIconW(instance, MAKEINTRESOURCEW(params.iconResourceId))
                   : ::LoadIconW(nullptr, IDI_APPLICATION);

  // 窗口类用 CS_DBLCLKS：双击由系统判定（连击间隔、移动容差都按系统设置），
  // 自己数两次点击的时间差会在慢速鼠标上误判。
  const std::wstring className =
      params.className.empty() ? std::wstring(kWindowClassName) : win32::toWide(params.className);
  if (win32::registerWindowClass(className.c_str(), &Win32Window::windowProc, icon, cursor_,
                                 CS_DBLCLKS) == 0) {
    return;
  }

  DWORD exStyle = WS_EX_CONTROLPARENT;
  DWORD windowStyle = WS_CLIPCHILDREN;

  if (parent_ != nullptr) {
    // 子窗口：铺满父客户区，不加边框，也不参与非客户区自绘。
    windowStyle |= WS_CHILD;
  } else if (style_.topLevel) {
    // WS_CAPTION + WS_THICKFRAME + WS_SYSMENU：系统边框、投影、Win11 圆角、
    // 贴边吸附全都靠这一组样式。标题栏本身由 WM_NCCALCSIZE 让出来自绘，
    // 所以「有 WS_CAPTION」不等于「会看到系统标题栏」。
    //
    // 这一点踩过：早先用 WS_POPUP + WM_NCCALCSIZE 返回 0，客户区盖满整窗，
    // 系统没有非客户区可画，于是窗口没有边框、没有阴影，看起来像一张贴纸。
    // AiUI 的做法是保留 WS_CAPTION，只把客户区的顶边抬到窗口顶端。
    if (style_.resizable) windowStyle |= WS_THICKFRAME;
    if (style_.minimizable) windowStyle |= WS_MINIMIZEBOX;
    if (style_.maximizable) windowStyle |= WS_MAXIMIZEBOX;
    // WS_SYSMENU 是 Alt+Space 系统菜单与 Alt+F4 的前提，不是装饰。
    windowStyle |= WS_SYSMENU | WS_CAPTION;
    if (!usesCustomFrame()) {
      // captionHeight = 0：不划走任何非客户区，交给系统画标题栏。
      // 这是一个合法的「原生窗口」模式，用于对话框与工具窗。
      windowStyle |= WS_VISIBLE;
    }
    // WS_EX_APPWINDOW：任务栏显示。自绘标题栏的窗口仍需出现在任务栏里。
    exStyle |= WS_EX_APPWINDOW;
  } else {
    windowStyle |= WS_CHILD;
  }
  if (params.visible) windowStyle |= WS_VISIBLE;

  // bounds 一律按「整窗矩形」解释（与 setBounds 一致）。自绘标题栏时
  // 客户区就等于整窗，两者没有差别；原生标题栏模式请用 clientRect() 校正。
  const bool hasBounds = params.bounds.width() > 0 && params.bounds.height() > 0;
  int x = params.bounds.left;
  int y = params.bounds.top;
  int width = 800;
  int height = 600;
  if (hasBounds) {
    width = params.bounds.width();
    height = params.bounds.height();
  } else {
    x = CW_USEDEFAULT;
    y = CW_USEDEFAULT;
  }
  if (parent_ == nullptr && !style_.topLevel) {
    // 没有父也没有顶层标记：当作铺满屏幕的子窗口处理，位置由调用方再设。
    x = 0;
    y = 0;
  }

  // 文本渲染器从平台门面取：字形缓存是进程内一份，不能每个窗口复制一套。
  // 取不到时（离线/测试环境）留空，SoftwareBackend 会画占位块而不是崩。
  textRenderer_ = platform().textRenderer();
  // FontCache 也必须绑上：控件创建字体走的是 FontCache::get，它没有绑定
  // 渲染器时会返回一个无效 Font，于是所有文字被静默跳过——窗口里只剩
  // 背景与图标，看起来像「文字没画」，实际是「字体没拿到」。
  FontCache::instance().setTextRenderer(textRenderer_);

  const std::wstring title = win32::toWide(params.title);
  hwnd_ = ::CreateWindowExW(exStyle, className.c_str(), title.c_str(), windowStyle, x, y, width,
                            height, parent_, nullptr, instance, this);
  if (hwnd_ == nullptr) return;

  if (usesCustomFrame()) {
    // 让 DWM 保留投影与圆角，并把系统边框调成与深浅色一致。
    // 必须在窗口建好之后调用：它下发的是非客户区消息，没有窗口就没有非客户区。
    applyDwmAppearance(hwnd_);
  }

  // 窗口已建好：后续所有消息都能通过 GWLP_USERDATA 找回 this。
  refreshDpi();
  if (dropHandler_) ::DragAcceptFiles(hwnd_, TRUE);
  // 这里刻意不发 onResize：宿主拿到 createWindow 的返回值之后才
  // setContentView，提前回调会让它对着半成品的树做布局。
  // 创建紧随其后的 WM_SIZE 才是第一次真正的「有尺寸了」。
  invalidate();
}

Win32Window::~Win32Window() {
  // 不在这里清 GWLP_USERDATA：DestroyWindow 会同步发 WM_DESTROY /
  // WM_NCDESTROY，我们的处理要能拿到 this 才有机会 KillTimer。
  // 反查由 WM_NCDESTROY 自己断开（那时 hwnd_ 已经归零，之后不会再回调）。
  dib_.release();
  if (hwnd_ != nullptr) {
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

LRESULT CALLBACK Win32Window::windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  Win32Window* self = nullptr;
  if (message == WM_NCCREATE) {
    // WM_NCCREATE 是唯一能拿到 lpCreateParams 的时机，必须在这里建立反查。
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    self = static_cast<Win32Window*>(create->lpCreateParams);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  // 创建早期（类注册到 WM_NCCREATE 之间）还没有 this，交给默认过程。
  if (self == nullptr) return ::DefWindowProcW(hwnd, message, wparam, lparam);
  return self->processMessage(hwnd, message, wparam, lparam);
}

// ─────────────────────────────────────────────────────────────
// 消息分发
// ─────────────────────────────────────────────────────────────

LRESULT Win32Window::processMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  // 构造期间 hwnd_ 还是空的，补上——否则 paint() 之类拿不到句柄。
  if (hwnd_ == nullptr) hwnd_ = hwnd;

  switch (message) {
    case WM_NCCALCSIZE: {
      // 只有自绘标题栏时才动非客户区。原生标题栏模式必须原样交给系统，
      // 否则系统的标题栏就没了。
      if (!usesCustomFrame()) break;

      // 做法与 AiUI 一致：**先让系统算出标准客户区**，再把顶边抬到窗口顶端。
      //
      // 为什么不能直接 return 0：那等于声明「客户区 = 整窗」，系统就没有
      // 非客户区可画了。后果不是只有标题栏消失，而是**窗口完全没有边框与
      // 阴影**——Win10/11 的 DWM 边框、投影、圆角、贴边吸附全挂在这块
      // 非客户区上。窗口会看起来像一张贴纸直接糊在桌面上。
      //
      // 保留左右与底部边框的代价是客户区比整窗小，这由 clientRect() 统一
      // 处理（它问系统要），绘制与命中都以客户区为坐标原点，所以不用另算。
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
      const RECT windowRect = params->rgrc[0];
      ::DefWindowProcW(hwnd, message, wparam, lparam);

      if (wparam == TRUE) {
        if (!fullscreen_ && ::IsZoomed(hwnd)) {
          // 最大化时窗口被系统放大到超出工作区（让边框藏在屏幕外）。
          // 直接取工作区，比按边框厚度缩回来更准——多显示器与任务栏
          // 停靠在侧边时，边框厚度算不出正确结果。
          MONITORINFO info{};
          info.cbSize = sizeof(info);
          if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
            params->rgrc[0] = info.rcWork;
          } else {
            const int frameX =
                ::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
            const int frameY =
                ::GetSystemMetrics(SM_CYSIZEFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
            params->rgrc[0].left += frameX;
            params->rgrc[0].right -= frameX;
            params->rgrc[0].top += frameY;
            params->rgrc[0].bottom -= frameY;
          }
        } else {
          // 常规窗口：只吃掉顶部那条标题栏，左右与底部边框留给系统。
          params->rgrc[0].top = windowRect.top;
        }
      } else {
        auto* rc = reinterpret_cast<RECT*>(lparam);
        if (rc != nullptr) rc->top = windowRect.top;
      }
      return 0;
    }

    case WM_NCACTIVATE:
      // 自绘标题栏：告诉系统「非客户区不需要重画」。返回 TRUE 而不是
      // 走默认处理，否则系统会试图画一条它以为存在的标题栏，闪一下黑条。
      if (usesCustomFrame()) return TRUE;
      break;

    case WM_NCHITTEST: {
      if (!usesCustomFrame() || fullscreen_) break;
      POINT screenPoint{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &screenPoint);
      const Point point{screenPoint.x, screenPoint.y};

      // 客户区之外（左右/底部边框）交给系统：那里是系统自己的缩放热区。
      const Rect client = clientRect();
      if (!client.contains(point)) {
        clearMaxButtonHover();
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
      }

      const WPARAM code = hitTest(point);
      if (code == HTMAXBUTTON) {
        // 系统接管 HTMAXBUTTON 后不会再发 WM_MOUSEMOVE，悬停态只能在这里推。
        // 只推「进入/移动」，离开由 WM_NCMOUSELEAVE 与其它分支清理。
        updateMaxButtonHover(point);
        return HTMAXBUTTON;
      }
      clearMaxButtonHover();
      return code;
    }

    // HTMAXBUTTON 的默认处理是「按下立刻最大化」，与普通按钮手感不同
    // （拖开也不取消）。这里自己接管按下与抬起：系统仍负责悬停时的
    // 贴靠布局预览，点击结果由我们决定，三个按钮的手感才一致。
    case WM_NCLBUTTONDOWN:
      if (usesCustomFrame() && wparam == HTMAXBUTTON) {
        maxButtonDown_ = true;
        return 0;
      }
      break;

    case WM_NCLBUTTONUP:
      if (usesCustomFrame() && wparam == HTMAXBUTTON) {
        const bool clicked = maxButtonDown_;
        maxButtonDown_ = false;
        if (clicked) toggleMaximize();
        return 0;
      }
      break;

    case WM_GETMINMAXINFO: {
      if (minWidth_ <= 0 && minHeight_ <= 0) break;
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      int w = minWidth_ > 0 ? px(minWidth_) : 0;
      int h = minHeight_ > 0 ? px(minHeight_) : 0;
      if (!usesCustomFrame()) {
        // 原生标题栏：最小尺寸指的是客户区，得加上边框与标题栏。
        RECT rc{0, 0, w, h};
        ::AdjustWindowRectEx(&rc, static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
      }
      if (w > 0) info->ptMinTrackSize.x = w;
      if (h > 0) info->ptMinTrackSize.y = h;
      return 0;
    }

    case WM_DPICHANGED: {
      dpi_ = static_cast<int>(HIWORD(wparam));
      if (dpi_ <= 0) dpi_ = 96;
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      const Rect client = clientRect();
      host_.onResize(client.width(), client.height());
      invalidate();
      return 0;
    }

    case WM_SIZE: {
      if (wparam == SIZE_MINIMIZED) return 0;
      const Rect client = clientRect();
      host_.onResize(client.width(), client.height());
      invalidate();
      return 0;
    }

    case WM_ERASEBKGND:
      // 背景由画布自己铺。让系统再刷一遍会闪，而且刷的是类背景色（空）。
      return 1;

    case WM_PAINT:
      paint();
      return 0;

    case WM_MOUSEMOVE: {
      if (!trackingLeave_) trackMouseLeave();
      // 用非客户区命中码区分「在标题栏上移动」。自绘标题栏模式下，
      // 鼠标在按钮以外的标题栏区域会被判成 HTCAPTION，此时系统发的是
      // WM_NCMOUSEMOVE，正常的 WM_MOUSEMOVE 收不到——那条在下面补。
      host_.onMouseMove(makeMouseEvent(lparam));
      return 0;
    }

    case WM_NCMOUSEMOVE: {
      // 标题栏上的移动也要让控件知道：最大/最小/关闭按钮虽然画在客户区里，
      // 但它们上方那一段会被判成 HTCAPTION，系统发的是 WM_NCMOUSEMOVE。
      // 不转发的话，鼠标从按钮区滑进标题栏时按钮的悬停态会一直留着。
      if (!usesCustomFrame()) break;
      // 队列里的消息可能带着「移出窗口之前」的坐标。照单转发会把已经离开的
      // 指针重新送回按钮上，悬停态就再也清不掉——转发前先核对实时光标。
      Point cursor{-1, -1};
      if (!realCursorClient(&cursor)) {
        clearMaxButtonHover();
        maxButtonDown_ = false;
        host_.onMouseLeave();
        break;
      }
      host_.onMouseMove(makeMouseEvent(MAKELPARAM(cursor.x, cursor.y)));
      // 交给系统：非客户区鼠标离开的跟踪由 DefWindowProc 维护。
      // 直接 return 0 会把那一步跳过，系统就不再关心指针何时离开。
      break;
    }

    case WM_MOUSELEAVE: {
      trackingLeave_ = false;
      // 光标可能只是从客户区滑进了最大化按钮：HTMAXBUTTON 的几何位置在客户区、
      // 命中码却属于非客户区，系统因此判定「已离开客户区」。按钮其实还被指着，
      // 不能清悬停。
      POINT cursor{};
      if (::GetCursorPos(&cursor) && ::ScreenToClient(hwnd, &cursor)) {
        if (host_.onHitTest(Point{cursor.x, cursor.y}) == HitTarget::kMaxButton) return 0;
      }
      clearMaxButtonHover();
      maxButtonDown_ = false;
      host_.onMouseLeave();
      return 0;
    }

    case WM_NCMOUSELEAVE:
      if (usesCustomFrame()) {
        // 系统告诉指针已离开非客户区。只清「指针已经不指着最大化按钮」这一种
        // 情况：从客户区滑进按钮时也会收到这条（按钮几何在客户区、命中码在
        // 非客户区），那时清掉会让悬停闪一下。
        //
        // 判断用「现在指着谁」，不用「是否还在窗口矩形内」：后者在指针刚越过
        // 边界那一刻为真，会把清除吞掉，而这条通知是一次性的。
        POINT cursor{};
        if (::GetCursorPos(&cursor) && ::ScreenToClient(hwnd, &cursor) &&
            host_.onHitTest(Point{cursor.x, cursor.y}) == HitTarget::kMaxButton) {
          return 0;
        }
        clearMaxButtonHover();
        maxButtonDown_ = false;
        trackingLeave_ = false;
        host_.onMouseLeave();
      }
      return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
      // 点击捕获焦点：Windows 的惯例是点哪个窗口哪个窗口拿到键盘焦点。
      if (::GetFocus() != hwnd) ::SetFocus(hwnd);
      MouseEvent event = makeMouseEvent(lparam);
      event.button = buttonFromMessage(message, wparam);
      host_.onMouseDown(event);
      // 侧键必须回 XBUTTON1/2，否则系统会把它当成「没处理」再送给默认过程。
      if (message == WM_XBUTTONDOWN) return TRUE;
      return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      MouseEvent event = makeMouseEvent(lparam);
      event.button = buttonFromMessage(message, wparam);
      host_.onMouseUp(event);
      if (message == WM_XBUTTONUP) return TRUE;
      return 0;
    }

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: {
      MouseEvent event = makeMouseEvent(lparam);
      event.button = buttonFromMessage(message, wparam);
      host_.onDoubleClick(event);
      return 0;
    }

    case WM_MOUSEWHEEL:
      host_.onMouseWheel(makeWheelEvent(wparam, lparam));
      return 0;

    case WM_CONTEXTMENU: {
      // lParam 是屏幕坐标；键盘触发（Shift+F10）时是 -1，
      // 此时按「当前鼠标位置」弹，与系统其它程序一致。
      Point screenPoint{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (screenPoint.x == -1 && screenPoint.y == -1) {
        POINT cursor{};
        if (::GetCursorPos(&cursor)) screenPoint = Point{cursor.x, cursor.y};
      }
      POINT clientPoint{screenPoint.x, screenPoint.y};
      ::ScreenToClient(hwnd, &clientPoint);
      MouseEvent event;
      event.position = Point{clientPoint.x, clientPoint.y};
      event.button = MouseButton::kRight;
      event.modifiers = modifiersFromWin32();
      host_.onContextMenu(event);
      // 即使宿主没弹菜单也吃掉：让系统再弹一次「系统菜单」只会让人困惑。
      return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const Key key = keyFromVirtualKey(wparam);
      if (key != Key::kNone) {
        if (host_.onKeyDown(key, modifiersFromWin32())) return 0;
      }
      break;
    }

    case WM_CHAR: {
      const auto unit = static_cast<std::uint32_t>(wparam);
      // 控制字符（退格、回车、Tab、Ctrl+A 产生的 0x01）不当正文。
      // 按键语义已经在 WM_KEYDOWN 里处理过了。
      if (unit < 0x20 || unit == 0x7F) return 0;

      if (unit >= 0xD800 && unit <= 0xDBFF) {
        // 高代理：等下一个 WM_CHAR 的低代理，合成一个码点。
        highSurrogate_ = static_cast<wchar_t>(unit);
        return 0;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (highSurrogate_ != 0) {
          const std::uint32_t combined =
              0x10000 + ((static_cast<std::uint32_t>(highSurrogate_) - 0xD800) << 10) +
              (unit - 0xDC00);
          highSurrogate_ = 0;
          host_.onChar(combined);
        }
        return 0;
      }
      highSurrogate_ = 0;
      host_.onChar(unit);
      return 0;
    }

    case WM_IME_STARTCOMPOSITION: {
      // 候选框贴到光标处。不做这一步，输入法窗口会跑到屏幕左上角，
      // 中文输入时用户根本找不到自己在打什么。
      HIMC context = ::ImmGetContext(hwnd);
      if (context != nullptr) {
        COMPOSITIONFORM form{};
        form.dwStyle = CFS_POINT;
        form.ptCurrentPos.x = imeCaret_.x;
        form.ptCurrentPos.y = imeCaret_.y;
        ::ImmSetCompositionWindow(context, &form);
        ::ImmReleaseContext(hwnd, context);
      }
      break;
    }

    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        ::SetCursor(cursor_);
        return TRUE;
      }
      break;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      // 输入法的组合串在失焦时必须结束，否则焦点回到窗口时还是旧内容。
      if (message == WM_KILLFOCUS) {
        HIMC context = ::ImmGetContext(hwnd);
        if (context != nullptr) {
          ::ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
          ::ImmReleaseContext(hwnd, context);
        }
        highSurrogate_ = 0;
      }
      break;
    }

    case WM_ACTIVATE: {
      // 标题栏的「激活/未激活」配色、输入框光标的显示都看它。
      // 用 WM_ACTIVATE 而不是 WM_SETFOCUS：点另一个程序时输入框就该停止闪烁。
      const bool active = LOWORD(wparam) != WA_INACTIVE;
      host_.onFocusChanged(active);
      if (usesCustomFrame()) invalidate();
      return 0;
    }

    case WM_TIMER: {
      if (wparam == kBlinkTimerId) {
        host_.onTimer();
        return 0;
      }
      if (wparam == kHoverWatchTimerId) {
        checkMaxButtonHover();
        return 0;
      }
      if (wparam == kDelayTimerId) {
        // 单次定时器：先杀掉，否则每 100ms 触发一次就变成周期定时器了。
        // 回调里可能又调 setDelayTimer，所以要等 KillTimer 之后再回调。
        ::KillTimer(hwnd, kDelayTimerId);
        host_.onTimer();
        return 0;
      }
      break;
    }

    case WM_SYSCOMMAND: {
      // Alt / F10 会触发系统菜单栏激活（SC_KEYMENU）。自绘标题栏的窗口
      // 没有菜单栏，这个激活只会让窗口卡在一个看不见的模态状态里。
      if ((wparam & 0xFFF0) == SC_KEYMENU && usesCustomFrame()) return 0;
      break;
    }

    case WM_CLOSE:
      // onCloseRequest 返回 true 表示宿主自己处理（比如「有未保存的文档」）。
      if (host_.onCloseRequest()) return 0;
      // 走正常关闭。用 DestroyWindow 而不是 PostQuitMessage：多窗口程序
      // 关一个窗口不该退出整个进程。
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      ::KillTimer(hwnd, kBlinkTimerId);
      ::KillTimer(hwnd, kDelayTimerId);
      ::KillTimer(hwnd, kHoverWatchTimerId);
      return 0;

    case WM_NCDESTROY:
      // 窗口已经死了，之后任何消息都可能带着这个句柄。
      ::KillTimer(hwnd, kHoverWatchTimerId);
      dib_.release();
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      return ::DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_DROPFILES: {
      const auto drop = reinterpret_cast<HDROP>(wparam);
      std::vector<std::string> paths;
      if (drop != nullptr) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i) {
          const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
          if (length == 0) continue;
          std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
          const UINT copied =
              ::DragQueryFileW(drop, i, buffer.data(), length + 1);
          buffer.resize(copied);
          paths.push_back(win32::toUtf8(buffer));
        }
        ::DragFinish(drop);
      }
      if (dropHandler_) dropHandler_(paths);
      return 0;
    }

    default:
      break;
  }

  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// ─────────────────────────────────────────────────────────────
// 绘制
// ─────────────────────────────────────────────────────────────

void Win32Window::paint() {
  if (hwnd_ == nullptr || painting_) {
    // 重入：只校验更新区，不画。不校验的话 WM_PAINT 会一直排队，
    // 消息循环就被它占满了。
    if (hwnd_ != nullptr) ::ValidateRect(hwnd_, nullptr);
    return;
  }

  PAINTSTRUCT ps{};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (dc == nullptr) return;

  const Rect update = rectFromWin32(ps.rcPaint);
  if (!update.isEmpty()) {
    painting_ = true;
    paintedDuringExpose_ = false;
    activePaintDc_ = dc;
    host_.onExpose(update);
    activePaintDc_ = nullptr;
    painting_ = false;

    if (!paintedDuringExpose_) {
      // 宿主没画（比如整棵树是空的）：补一层底色，别让更新区留着垃圾。
      WindowCanvas canvas(dib_, dc, /*ownsTarget=*/false, update, backgroundColor_, textRenderer_);
    }
  }

  ::EndPaint(hwnd_, &ps);
}

std::unique_ptr<Canvas> Win32Window::createCanvas(const Rect& area) {
  if (hwnd_ == nullptr) return nullptr;

  HDC target = activePaintDc_;
  bool ownsTarget = false;
  if (target == nullptr) {
    // WM_PAINT 之外（比如宿主想立刻画一帧）才自己取 DC。
    target = ::GetDC(hwnd_);
    if (target == nullptr) return nullptr;
    ownsTarget = true;
  }

  const Rect clamped = area.intersect(clientRect());
  auto canvas = std::make_unique<WindowCanvas>(dib_, target, ownsTarget, clamped,
                                               backgroundColor_, textRenderer_);
  if (!canvas->valid()) {
    // 不给调用方一个画不出东西的画布。DC 由 WindowCanvas 析构释放
    // （ownsTarget_ 已经记在它身上），这里再释放就是重复释放。
    return nullptr;
  }
  // 只有真的拿到了画布才算「本次 onExpose 宿主画过了」。
  // 提前置位会让 paint() 的兜底底色在失败时也不执行，更新区留垃圾。
  if (!ownsTarget) paintedDuringExpose_ = true;
  return canvas;
}

// ─────────────────────────────────────────────────────────────
// 几何与状态
// ─────────────────────────────────────────────────────────────

int Win32Window::px(int designValue) const {
  // 与 View::px 同一套取整：正负对称、0 保持 0。
  if (designValue == 0) return 0;
  const long long scaled = static_cast<long long>(designValue) * dpi_;
  if (scaled >= 0) return static_cast<int>((scaled + 48) / 96);
  return -static_cast<int>((-scaled + 48) / 96);
}

Rect Win32Window::clientRect() const {
  if (hwnd_ == nullptr) return Rect{};
  RECT rc{};
  if (!::GetClientRect(hwnd_, &rc)) return Rect{};
  return Rect::fromSize(rc.right - rc.left, rc.bottom - rc.top);
}

Rect Win32Window::windowRect() const {
  if (hwnd_ == nullptr) return Rect{};
  RECT rc{};
  if (!::GetWindowRect(hwnd_, &rc)) return Rect{};
  return rectFromWin32(rc);
}

Point Win32Window::clientOrigin() const {
  if (hwnd_ == nullptr) return Point{};
  POINT origin{0, 0};
  if (!::ClientToScreen(hwnd_, &origin)) return Point{};
  return Point{origin.x, origin.y};
}

void Win32Window::setBounds(const Rect& bounds) {
  if (hwnd_ == nullptr) return;
  ::SetWindowPos(hwnd_, nullptr, bounds.left, bounds.top, bounds.width(), bounds.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32Window::refreshDpi() {
  if (hwnd_ == nullptr) return;
  const int dpi = win32::dpiForWindow(hwnd_);
  dpi_ = dpi > 0 ? dpi : 96;
}

void Win32Window::show(bool visible) {
  if (hwnd_ == nullptr) return;
  ::ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Win32Window::invalidate() {
  if (hwnd_ == nullptr) return;
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void Win32Window::invalidateRect(const Rect& rc) {
  if (hwnd_ == nullptr) return;
  const Rect clamped = rc.intersect(clientRect());
  if (clamped.isEmpty()) return;
  RECT r{clamped.left, clamped.top, clamped.right, clamped.bottom};
  ::InvalidateRect(hwnd_, &r, FALSE);
}

void Win32Window::setBackgroundColor(Color color) {
  backgroundColor_ = color;
  // 不改窗口类的 hbrBackground：WM_ERASEBKGND 已经被我们吃掉（返回 1），
  // 系统画不到它；而 SetClassLongPtr 每次调用都要新建一个画刷，
  // 换主题时就会漏刷子。背景统一由画布自己铺，只有一处真相。
  invalidate();
}

void Win32Window::setFocus() {
  if (hwnd_ == nullptr) return;
  ::SetFocus(hwnd_);
}

void Win32Window::setCursorShape(CursorShape shape) {
  cursor_ = cursorForShape(shape);
  if (cursor_ == nullptr) cursor_ = ::LoadCursorW(nullptr, IDC_ARROW);
}

void Win32Window::captureMouse(bool capture) {
  if (hwnd_ == nullptr) return;
  if (capture) {
    ::SetCapture(hwnd_);
  } else if (::GetCapture() == hwnd_) {
    ::ReleaseCapture();
  }
}

void Win32Window::setBlinkTimer(bool on) {
  if (hwnd_ == nullptr) return;
  if (on) {
    // 闪烁周期用系统的光标闪烁速率：用户改过速度时这里自动跟着变。
    UINT interval = ::GetCaretBlinkTime();
    if (interval == 0 || interval == static_cast<UINT>(-1)) interval = 530;
    ::SetTimer(hwnd_, kBlinkTimerId, interval, nullptr);
  } else {
    ::KillTimer(hwnd_, kBlinkTimerId);
  }
}

void Win32Window::setDelayTimer(int ms) {
  if (hwnd_ == nullptr) return;
  // 同一个 id 再 SetTimer 会自动重新计时，不需要先 Kill。
  if (ms <= 0) {
    ::KillTimer(hwnd_, kDelayTimerId);
    return;
  }
  ::SetTimer(hwnd_, kDelayTimerId, static_cast<UINT>(ms), nullptr);
}

void Win32Window::setImeCaret(int x, int y) {
  imeCaret_ = Point{x, y};
  if (hwnd_ == nullptr) return;
  HIMC context = ::ImmGetContext(hwnd_);
  if (context == nullptr) return;
  COMPOSITIONFORM form{};
  form.dwStyle = CFS_POINT;
  form.ptCurrentPos.x = x;
  form.ptCurrentPos.y = y;
  ::ImmSetCompositionWindow(context, &form);
  ::ImmReleaseContext(hwnd_, context);
}

void Win32Window::setTitle(const std::string& title) {
  if (hwnd_ == nullptr) return;
  ::SetWindowTextW(hwnd_, win32::toWide(title).c_str());
}

void Win32Window::minimize() {
  if (hwnd_ == nullptr) return;
  ::ShowWindow(hwnd_, SW_MINIMIZE);
}

bool Win32Window::isMaximized() const {
  if (hwnd_ == nullptr) return false;
  return ::IsZoomed(hwnd_) != 0;
}

void Win32Window::toggleMaximize() {
  if (hwnd_ == nullptr || !style_.maximizable) return;
  ::ShowWindow(hwnd_, isMaximized() ? SW_RESTORE : SW_MAXIMIZE);
}

Rect Win32Window::normalBounds() const {
  if (hwnd_ == nullptr) return Rect{};
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!::GetWindowPlacement(hwnd_, &placement)) return windowRect();
  // rcNormalPosition 是工作区坐标；对顶层窗口就是屏幕坐标。
  return rectFromWin32(placement.rcNormalPosition);
}

void Win32Window::close() {
  if (hwnd_ == nullptr) return;
  // 异步：宿主可能在 close() 返回后继续用自己的对象。
  ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Win32Window::beginDrag(WPARAM ncCode) {
  if (hwnd_ == nullptr) return;
  // 系统拖动循环要求鼠标已经释放：否则我们自己的 mouse up 会在循环
  // 结束后才到，状态就错位了。
  ::ReleaseCapture();
  POINT cursor{};
  ::GetCursorPos(&cursor);
  ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, ncCode, MAKELPARAM(cursor.x, cursor.y));
}

void Win32Window::startDragMove() { beginDrag(HTCAPTION); }

void Win32Window::startDragResize(HitTarget edge) {
  WPARAM code = HTBORDER;
  if (!hitTargetToNcCode(edge, code) || code == HTCAPTION) return;
  beginDrag(code);
}

void Win32Window::setFullscreen(bool fullscreen) {
  if (hwnd_ == nullptr || fullscreen == fullscreen_) return;

  if (fullscreen) {
    savedPlacement_.length = sizeof(savedPlacement_);
    ::GetWindowPlacement(hwnd_, &savedPlacement_);
    savedStyle_ = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
      return;
    }

    // 去掉边框与标题栏，铺满整个显示器（含任务栏区域）。
    // 保留 WS_VISIBLE，否则窗口会消失。
    const LONG_PTR style = (savedStyle_ & ~(WS_THICKFRAME | WS_CAPTION | WS_SYSMENU)) | WS_POPUP;
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    ::SetWindowPos(hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                   monitor.rcMonitor.right - monitor.rcMonitor.left,
                   monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                   SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    fullscreen_ = true;
  } else {
    fullscreen_ = false;
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
    ::SetWindowPlacement(hwnd_, &savedPlacement_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }

  const Rect client = clientRect();
  host_.onResize(client.width(), client.height());
  invalidate();
}

void Win32Window::setFileDropHandler(std::function<void(const std::vector<std::string>&)> fn) {
  dropHandler_ = std::move(fn);
  if (hwnd_ != nullptr) {
    // 不设处理器就别接收：接收了又要悄悄丢掉会让拖放源显示错误的光标。
    ::DragAcceptFiles(hwnd_, dropHandler_ != nullptr);
  }
}

// ─────────────────────────────────────────────────────────────
// 输入翻译
// ─────────────────────────────────────────────────────────────

MouseEvent Win32Window::makeMouseEvent(LPARAM lparam) const {
  MouseEvent event;
  event.position = Point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  event.button = MouseButton::kNone;
  event.modifiers = modifiersFromWin32();
  return event;
}

MouseEvent Win32Window::makeWheelEvent(WPARAM wparam, LPARAM lparam) const {
  MouseEvent event;
  // 滚轮消息里的坐标是**屏幕**坐标，与其它鼠标消息不同。
  POINT screenPoint{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (hwnd_ != nullptr) ::ScreenToClient(hwnd_, &screenPoint);
  event.position = Point{screenPoint.x, screenPoint.y};
  event.wheelDelta = GET_WHEEL_DELTA_WPARAM(wparam);
  event.modifiers = modifiersFromWin32();
  return event;
}

void Win32Window::trackMouseLeave() {
  if (hwnd_ == nullptr) return;
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  if (::TrackMouseEvent(&track)) trackingLeave_ = true;
}

// ── 最大化按钮的 NC 状态 ──
//
// 为什么需要这几条：最大化按钮返回 HTMAXBUTTON，点击因此变成非客户区事件，
// WM_LBUTTONDOWN 收不到。系统对 HTMAXBUTTON 的默认处理是「按下即最大化」，
// 与普通按钮的手感不同（拖开就取消），也让「悬停变底色」失效——因为悬停
// 判定在系统的 NC 逻辑里，不在我们的视图树里。
//
// AIUI 的做法就是这里的做法：系统只保留贴靠布局的悬停预览，按下/抬起与
// 悬停态全部自己接管。这样最大化按钮与相邻两个按钮的手感完全一致。
void Win32Window::updateMaxButtonHover(const Point& position) {
  (void)position;
  // 判定一律以**实时光标**为准，不用消息里带的位置。
  //
  // WM_NCHITTEST / WM_NCMOUSEMOVE 带的是排队那一刻的坐标：指针快速移出窗口时，
  // 队列里还压着几条「当时确实在按钮上」的消息，逐条照收就会把刚清掉的悬停
  // 重新点亮；而离开通知是一次性的，点亮之后不会再来第二条，灰底便永久残留。
  // 这就是「第一次能消失、之后再也不消失」。
  Point cursor{-1, -1};
  if (!realCursorClient(&cursor) || host_.onHitTest(cursor) != HitTarget::kMaxButton) {
    clearMaxButtonHover();
    return;
  }

  const bool entered = !hoverMaxButton_;
  hoverMaxButton_ = true;
  // 进入悬停时开兜底复核：离开通知不能作为唯一依据。
  if (entered) startHoverWatch();
  if (cursor.x == hoverMaxPos_.x && cursor.y == hoverMaxPos_.y) return;
  hoverMaxPos_ = cursor;

  MouseEvent event;
  event.position = cursor;
  event.modifiers = modifiersFromWin32();
  host_.onMouseMove(event);
}

void Win32Window::clearMaxButtonHover() {
  if (!hoverMaxButton_) return;
  hoverMaxButton_ = false;
  hoverMaxPos_ = Point{-1, -1};
  stopHoverWatch();
  host_.onMouseLeave();
}

// 实时光标 → 客户区坐标。窗口已销毁，或光标已不在客户区内时返回 false。
bool Win32Window::realCursorClient(Point* out) const {
  if (hwnd_ == nullptr) return false;
  POINT cursor{};
  if (!::GetCursorPos(&cursor)) return false;
  if (!::ScreenToClient(hwnd_, &cursor)) return false;
  if (!clientRect().contains(Point{cursor.x, cursor.y})) return false;
  if (out != nullptr) *out = Point{cursor.x, cursor.y};
  return true;
}

// 悬停期间的低频复核。
//
// 为什么必须有它：指针移出窗口时系统不一定会再发离开通知（HTMAXBUTTON 的
// 非客户区跟踪是一次性的，而队列里排着的命中测试还会把悬停重新点亮）。
// 悬停唯一的合法条件就是「指针指着按钮」，那就按这个条件定期复核，把离开
// 通知降级成加速手段。只在悬停期间运行，静态界面不会因此有任何周期唤醒。
void Win32Window::startHoverWatch() {
  if (hwnd_ == nullptr) return;
  ::SetTimer(hwnd_, kHoverWatchTimerId, kHoverWatchIntervalMs, nullptr);
}

void Win32Window::stopHoverWatch() {
  if (hwnd_ == nullptr) return;
  ::KillTimer(hwnd_, kHoverWatchTimerId);
}

// 悬停复核：指针已经不在最大化按钮上就清掉。
void Win32Window::checkMaxButtonHover() {
  if (!hoverMaxButton_) return;
  Point cursor{-1, -1};
  if (!realCursorClient(&cursor) || host_.onHitTest(cursor) != HitTarget::kMaxButton) {
    clearMaxButtonHover();
  }
}

WPARAM Win32Window::hitTest(const Point& clientPoint) const {
  const Rect client = clientRect();

  // 先问视图树，再判缩放带——顺序与 AiUI 一致，不能反过来。
  //
  // 反过来会踩一个坑：鼠标从最大化按钮往上移时先落进顶部缩放带，命中被改成
  // HTTOP，系统立刻转入「调整大小」模式并接管鼠标；此后窗口收不到鼠标消息，
  // 按钮的悬停底色就再也等不到清除它的那一步。先问视图树能让按钮在缩放带
  // 之上拿到 HTMAXBUTTON，缩放带的判定只作用于 kClient/kCaption。
  const HitTarget target = host_.onHitTest(clientPoint);
  if (target == HitTarget::kMaxButton) {
    // 交给系统：悬停会弹出贴靠布局（Snap Layouts）。
    return HTMAXBUTTON;
  }

  // 顶部缩放带：只有顶部是自己补的。
  //
  // 左右与底部边框现在由系统负责——WM_NCCALCSIZE 把它们留在了非客户区里，
  // 系统自带的缩放热区还在，不需要也不应该在这里重复判断（重复判断会让
  // 客户区边缘 4~8px 内的控件点不到，因为命中被改成 HTLEFT 之类的缩放码）。
  //
  // 顶部则必须自己补：那一圈非客户区被我们吃掉用来画标题栏，系统的顶部
  // 缩放热区也就没了，不补就拖不动上边。
  if (style_.resizable && !::IsZoomed(hwnd_) &&
      (target == HitTarget::kClient || target == HitTarget::kCaption)) {
    const int topBand = std::max(1, px(style_.topResizeWidth));
    if (clientPoint.y < client.top + topBand) {
      // 左右上角仍按角处理，否则顶边只能上下拉、拉不了宽度。
      const int corner = std::max(1, px(style_.borderResizeWidth));
      if (clientPoint.x < client.left + corner) return HTTOPLEFT;
      if (clientPoint.x >= client.right - corner) return HTTOPRIGHT;
      return HTTOP;
    }
  }

  WPARAM code = HTCLIENT;
  if (hitTargetToNcCode(target, code)) return code;
  return HTCLIENT;
}
}  // namespace

// ─────────────────────────────────────────────────────────────
// 工厂
// ─────────────────────────────────────────────────────────────

// 文本渲染器从平台门面取：窗口只是绘制目标，字体服务的所有权在 Platform。
// 取不到时（离线/测试环境）退回占位字形，界面仍然能画出来，不会崩。
PlatformWindow* createPlatformWindow(NativeHandle parent, const WindowParams& params,
                                     WindowHost& host) {
  auto* window = new Win32Window(parent, params, host);
  if (window->handle() == nullptr) {
    delete window;
    return nullptr;
  }
  return window;
}

void destroyPlatformWindow(PlatformWindow* window) { delete window; }

}  // namespace pyui
