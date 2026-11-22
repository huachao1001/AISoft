// Windows 窗口后端的契约测试
//
// 与控件测试不同，这里测的是「后端把系统交出来的东西翻译对了没有」：
//
//   1. **非客户区真的让掉了**。自绘标题栏的前提是客户区等于整窗；
//      这条一旦不成立，标题栏会盖住内容，且在最大化时被裁掉一圈。
//   2. **createCanvas 画的东西真的到了窗口 DC**。这是「屏幕与离线渲染
//      共用 SoftwareBackend」的最后一段：DIB 画对了但 BitBlt 贴错位置，
//      离线测试全绿、屏幕上却什么都没变。
//   3. **命中测试的坐标系没搞反**。WM_NCHITTEST 给的是屏幕坐标，翻译成
//      客户区时少减一次偏移，缩放带就会跑到别的角上。
//
// 只有 Windows 能跑：非 Windows 构建里整个文件编成空翻译单元。
#include "test_framework.h"

#include "pyui/canvas.h"
#include "pyui/painter.h"
#include "pyui/platform/platform.h"
#include "pyui/platform/window.h"
#include "pyui/surface.h"
#include "pyui/text_renderer.h"

#if defined(PYUI_OS_WINDOWS)

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pyui {
namespace test {
namespace {

// 记录宿主收到的回调。窗口后端只该「翻译并转发」，所以这些计数就是
// 契约本身：收到几次、带什么坐标。
struct RecordingHost final : WindowHost {
  int resizes = 0;
  int exposes = 0;
  int closes = 0;
  bool closeHandled = false;

  Size lastSize{};
  Rect lastUpdate{};

  int mouseMoves = 0;
  Point lastMouse{};
  MouseButton lastButton = MouseButton::kNone;
  int wheelDelta = 0;
  int mouseLeaves = 0;
  int doubleClicks = 0;
  int contextMenus = 0;

  bool lastFocused = false;
  std::uint32_t lastChar = 0;
  std::vector<std::uint32_t> chars;
  HitTarget nextHit = HitTarget::kClient;

  void onExpose(const Rect& update) override {
    ++exposes;
    lastUpdate = update;
  }
  void onResize(int width, int height) override {
    ++resizes;
    lastSize = Size{width, height};
  }
  void onMouseDown(const MouseEvent& e) override {
    lastButton = e.button;
    lastMouse = e.position;
  }
  void onMouseUp(const MouseEvent& e) override {
    lastButton = e.button;
    lastMouse = e.position;
  }
  void onMouseMove(const MouseEvent& e) override {
    ++mouseMoves;
    lastMouse = e.position;
  }
  void onMouseLeave() override { ++mouseLeaves; }
  void onMouseWheel(const MouseEvent& e) override {
    wheelDelta = e.wheelDelta;
    // 滚轮位置也要转发：脚本里「在哪个控件上滚动」靠它定位，
    // 而 WM_MOUSEWHEEL 的坐标是屏幕坐标，最容易翻错。
    lastMouse = e.position;
  }
  void onDoubleClick(const MouseEvent&) override { ++doubleClicks; }
  void onContextMenu(const MouseEvent& e) override {
    ++contextMenus;
    lastMouse = e.position;
  }
  bool onKeyDown(const Key&, const Modifiers&) override { return false; }
  bool onChar(std::uint32_t codepoint) override {
    chars.push_back(codepoint);
    lastChar = codepoint;
    return true;
  }
  void onFocusChanged(bool focused) override { lastFocused = focused; }
  void onTimer() override {}
  HitTarget onHitTest(const Point&) override { return nextHit; }
  bool onCloseRequest() override {
    ++closes;
    return closeHandled;
  }
};

// 把队列里的消息跑完。窗口几何、WM_SIZE 都是异步投递的，
// 不排空就没法断言。
void pumpMessages() {
  MSG msg{};
  // 上限只是防御：WM_PAINT 在 BeginPaint 里会校验区域，不会无限重排。
  for (int i = 0; i < 500; ++i) {
    if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) == 0) break;
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
}

HWND rawHandle(PlatformWindow* window) {
  return static_cast<HWND>(window->handle());
}

// 从窗口 DC 读回一块像素。
//
// 为什么不直接读后端内部的 DIB：那只能证明「我们画进了自己的缓冲」，
// 而 BitBlt 贴错位置这一类的 bug 恰恰在缓冲之外。读窗口 DC 是唯一能
// 覆盖「DIB → 屏幕」这段的断言。
Surface captureClient(HWND hwnd, const Rect& area) {
  Surface result(area.width(), area.height());
  HDC windowDc = ::GetDC(hwnd);
  if (windowDc == nullptr) return Surface();

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = area.width();
  info.bmiHeader.biHeight = -area.height();  // 自上而下，与 Surface 一致
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC memory = ::CreateCompatibleDC(windowDc);
  if (bitmap != nullptr && bits != nullptr && memory != nullptr) {
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    // 窗口 DC 可能被裁剪；CAPTUREBLT 让它连分层窗口一起拷。
    if (::BitBlt(memory, 0, 0, area.width(), area.height(), windowDc, area.left, area.top,
                 SRCCOPY | CAPTUREBLT) != 0) {
      const auto* source = static_cast<const std::uint32_t*>(bits);
      for (int y = 0; y < area.height(); ++y) {
        for (int x = 0; x < area.width(); ++x) {
          result.set(x, y, source[static_cast<size_t>(y) * area.width() + x]);
        }
      }
    }
    ::SelectObject(memory, previous);
  }
  if (memory != nullptr) ::DeleteDC(memory);
  if (bitmap != nullptr) ::DeleteObject(bitmap);
  ::ReleaseDC(hwnd, windowDc);
  return result;
}

// 只比 RGB：窗口 DC 在 BitBlt 回读时会把 alpha 归一化，
// 拿 alpha 做断言会得到一个与后端无关的假失败。
std::uint32_t rgbAt(const Surface& surface, int x, int y) {
  return surface.at(x, y) & 0x00FFFFFFu;
}

// 一扇自绘标题栏的窗口。参数集中在一处，用例里只改需要变的字段。
//
// visible 默认关掉：测试不该在用户桌面上闪一堆窗口。只有需要「从窗口 DC
// 读回像素」的用例才打开——隐藏窗口的 DC 没有合成表面，回读到的是空白，
// 那会让 BitBlt 的断言变成假失败。
struct Fixture {
  RecordingHost host;
  PlatformWindow* window = nullptr;

  explicit Fixture(const WindowStyle& style = customFrameStyle(), bool visible = false) {
    WindowParams params;
    params.visible = false;
    params.title = "PyUI 测试窗口";
    params.style = style;
    params.bounds = Rect::at(120, 120, 480, 320);
    window = platform().createWindow(nullptr, params, host);
    if (window != nullptr && visible) {
      // SW_SHOWNOACTIVATE：弹出但不抢焦点，用户的当前操作不被打断。
      window->show(true);
      pumpMessages();
    }
  }
  ~Fixture() {
    if (window != nullptr) platform().destroyWindow(window);
  }

  static WindowStyle customFrameStyle() {
    WindowStyle style;
    style.topLevel = true;
    style.captionHeight = 32;
    return style;
  }
  static WindowStyle nativeFrameStyle() {
    WindowStyle style;
    style.topLevel = true;
    style.captionHeight = 0;  // 保留系统标题栏
    return style;
  }
};

}  // namespace

// ── 创建与几何 ──

TEST(win_window_creates_with_handle_and_dpi) {
  Fixture fixture;
  CHECK(fixture.window != nullptr);
  if (fixture.window == nullptr) return;

  CHECK(fixture.window->handle() != nullptr);
  // DPI 只可能是 96 的常见档位；断言「> 0」是为了抓「忘了初始化」。
  CHECK(fixture.window->dpi() > 0);
  CHECK(fixture.window->clientRect().width() > 0);
  CHECK(fixture.window->clientRect().height() > 0);
}

TEST(win_window_creation_reports_size_to_host) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  // 显式改一次尺寸：WM_SIZE 可能是创建期投递的，也可能还没到，
  // 主动触发才能确定地断言「后端把尺寸转发给了宿主」。
  fixture.window->setBounds(Rect::at(140, 140, 400, 300));
  pumpMessages();

  CHECK(fixture.host.resizes > 0);
  const Rect client = fixture.window->clientRect();
  CHECK_EQ(fixture.host.lastSize.width, client.width());
  CHECK_EQ(fixture.host.lastSize.height, client.height());
}

TEST(win_window_custom_frame_keeps_side_borders_for_shadow) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  // 自绘标题栏的核心不变量：**顶部**没有非客户区（否则系统标题栏会盖住
  // 自绘的那一条），但左右与底部必须留着。
  //
  // 为什么不是「客户区 == 整窗」：那样系统就没有非客户区可画，窗口会完全
  // 没有边框与阴影。DWM 的投影、Win11 圆角、贴边吸附全挂在这块非客户区上，
  // 全吃掉之后窗口看起来像一张贴纸（这是实际踩过的回归）。
  //
  // 注意 clientRect() 的原点是 0（客户区局部坐标），判断「顶部有没有
  // 非客户区」必须比较 clientOrigin()（屏幕坐标）与 windowRect().top。
  const Point origin = fixture.window->clientOrigin();
  const Rect client = fixture.window->clientRect();
  const Rect frame = fixture.window->windowRect();
  CHECK_EQ(origin.y, frame.top);  // 顶部让给自绘标题栏
  // 左右与底部各留一条系统边框（宽度随 DPI 变化，不写死像素值）。
  CHECK(client.width() < frame.width());
  CHECK(client.height() < frame.height());
}

TEST(win_window_native_frame_keeps_nonclient_area) {
  Fixture fixture(Fixture::nativeFrameStyle());
  if (fixture.window == nullptr) return;

  // captionHeight == 0 时不碰非客户区，系统标题栏与边框都还在：
  // 客户区必须比整窗小。
  const Rect client = fixture.window->clientRect();
  const Rect frame = fixture.window->windowRect();
  CHECK(client.width() < frame.width());
  CHECK(client.height() < frame.height());
}

TEST(win_window_client_origin_matches_screen_position) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 160, 400, 300));
  pumpMessages();

  const Point origin = fixture.window->clientOrigin();
  const Rect frame = fixture.window->windowRect();
  // 顶部没有非客户区，所以客户区原点与整窗的 left/top 对齐；
  // 左右边框的存在会让 x 略大一点，这里只断言 y（顶边）严格相等。
  CHECK_EQ(origin.y, frame.top);
  CHECK(origin.x >= frame.left);
  CHECK(origin.x <= frame.left + 32);  // 边框不会宽到离谱
}

TEST(win_window_hit_test_translates_screen_coordinates) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  HWND hwnd = rawHandle(fixture.window);
  const Point origin = fixture.window->clientOrigin();
  const Rect client = fixture.window->clientRect();

  // WM_NCHITTEST 收的是**屏幕**坐标。少减一次客户区偏移的话，
  // 下面这条「窗口正中间是客户区」会误判成边框。
  auto hitTestAt = [&](int clientX, int clientY) -> LRESULT {
    return ::SendMessageW(hwnd, WM_NCHITTEST, 0,
                          MAKELPARAM(origin.x + clientX, origin.y + clientY));
  };

  // 宿主说「这里是标题栏」时才回 HTCAPTION。默认的 nextHit 是 kClient，
  // 所以必须先显式设成 kCaption——否则这条断言测的是「宿主说什么就是什么」，
  // 而不是「屏幕坐标翻译对了没有」。
  fixture.host.nextHit = HitTarget::kCaption;
  CHECK_EQ(hitTestAt(client.width() / 2, client.height() / 2), LRESULT(HTCAPTION));
  // 左右与底部的缩放热区现在由系统负责（那几圈仍在非客户区里），
  // 后端不再自己判——所以这几个位置必须回落到宿主给的 kCaption。
  CHECK_EQ(hitTestAt(1, client.height() / 2), LRESULT(HTCAPTION));
  CHECK_EQ(hitTestAt(client.width() - 1, client.height() / 2), LRESULT(HTCAPTION));
  CHECK_EQ(hitTestAt(client.width() / 2, client.height() - 1), LRESULT(HTCAPTION));
  // 顶部缩放带是后端自己补的：那圈非客户区被吃掉画标题栏了，
  // 不补就拖不动上边。左上角同时是水平缩放角。
  CHECK_EQ(hitTestAt(1, 1), LRESULT(HTTOPLEFT));

  // 宿主说是按钮热区时，后端必须回 HTMAXBUTTON——只有这个码会让
  // 系统在悬停时弹出 Snap Layouts。
  fixture.host.nextHit = HitTarget::kMaxButton;
  CHECK_EQ(hitTestAt(client.width() - 60, 10), LRESULT(HTMAXBUTTON));

  // 宿主说客户区就还给客户区
  fixture.host.nextHit = HitTarget::kClient;
  CHECK_EQ(hitTestAt(client.width() / 2, client.height() / 2), LRESULT(HTCLIENT));
}

TEST(win_window_client_point_becomes_click_coordinates) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  // 鼠标坐标是**客户区**坐标，不需要后端再换算。这里验证它没有被
  // 误当成屏幕坐标去减偏移。
  ::SendMessageW(rawHandle(fixture.window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(37, 61));
  CHECK_EQ(fixture.host.lastMouse.x, 37);
  CHECK_EQ(fixture.host.lastMouse.y, 61);
  CHECK(fixture.host.lastButton == MouseButton::kLeft);
}

TEST(win_window_wheel_position_is_client_relative) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  const Point origin = fixture.window->clientOrigin();
  // WM_MOUSEWHEEL 的 lParam 是屏幕坐标（与其它鼠标消息不同），
  // 后端要减掉客户端原点。不减的话滚轮永远作用在左上角那块内容上。
  const WPARAM wparam = MAKEWPARAM(0, static_cast<WORD>(-120));
  ::SendMessageW(rawHandle(fixture.window), WM_MOUSEWHEEL, wparam,
                 MAKELPARAM(origin.x + 55, origin.y + 44));

  CHECK_EQ(fixture.host.wheelDelta, -120);
  CHECK_EQ(fixture.host.lastMouse.x, 55);
  CHECK_EQ(fixture.host.lastMouse.y, 44);
}

// ── 绘制 ──

TEST(win_window_canvas_blits_to_window_dc) {
  Fixture fixture(Fixture::customFrameStyle(), /*visible=*/true);
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  const Rect area = Rect::fromSize(64, 48);
  const Color fill = makeColor(0x2F, 0x6F, 0xE8);
  const Color background = makeColor(0xF3, 0xF3, 0xF3);
  fixture.window->setBackgroundColor(background);

  {
    std::unique_ptr<Canvas> canvas = fixture.window->createCanvas(area);
    CHECK(canvas != nullptr);
    if (canvas == nullptr) return;
    CHECK(canvas->area() == area);
    canvas->painter().fillRect(Rect{10, 10, 40, 30}, fill);
    // 析构即 present()
  }

  // 读回窗口 DC：像素必须真的落到屏幕上，而不是只进了内部 DIB。
  const Surface shot = captureClient(rawHandle(fixture.window), area);
  CHECK(shot.valid());
  if (!shot.valid()) return;

  CHECK_EQ(rgbAt(shot, 20, 20), fill & 0x00FFFFFFu);
  // 矩形外是底色：证明清屏用了 setBackgroundColor 的值，
  // 而不是 DIB 的残留内容。
  CHECK_EQ(rgbAt(shot, 2, 2), background & 0x00FFFFFFu);
  // 边界：x=40 已经在矩形右侧（Rect 右界开区间）
  CHECK_EQ(rgbAt(shot, 40, 20), background & 0x00FFFFFFu);
  CHECK_EQ(rgbAt(shot, 39, 20), fill & 0x00FFFFFFu);
}

TEST(win_window_canvas_clamps_area_to_client) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 320, 240));
  pumpMessages();

  const Rect client = fixture.window->clientRect();
  // 越界区域必须被裁到客户区：否则 present() 会 BitBlt 到窗口外，
  // 在别的窗口上留下一块"幽灵"像素。
  auto canvas = fixture.window->createCanvas(
      Rect::fromLtrb(-20, -20, client.width() + 40, client.height() + 40));
  CHECK(canvas != nullptr);
  if (canvas == nullptr) return;
  CHECK(canvas->area() == client);
}

TEST(win_window_canvas_offset_draws_at_area_origin) {
  Fixture fixture(Fixture::customFrameStyle(), /*visible=*/true);
  if (fixture.window == nullptr) return;
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  const Color background = makeColor(0xFF, 0xFF, 0xFF);
  const Color fill = makeColor(0x00, 0x80, 0x00);
  fixture.window->setBackgroundColor(background);

  // 只更新中间一块 32×32。画布的 Painter 用的是**客户区坐标**
  // （构造时 translate 到 area 原点），所以画在 (100,80) 的矩形就应当
  // 出现在客户区的 (100,80) 处。
  const Rect area = Rect::at(100, 80, 32, 32);
  {
    auto canvas = fixture.window->createCanvas(area);
    CHECK(canvas != nullptr);
    if (canvas == nullptr) return;
    canvas->painter().fillRect(Rect{100, 80, 108, 88}, fill);
  }

  const Surface shot = captureClient(rawHandle(fixture.window), Rect::fromSize(200, 160));
  CHECK(shot.valid());
  if (!shot.valid()) return;

  CHECK_EQ(rgbAt(shot, 100, 80), fill & 0x00FFFFFFu);
  CHECK_EQ(rgbAt(shot, 107, 87), fill & 0x00FFFFFFu);
  // 右边一格已经出矩形
  CHECK_EQ(rgbAt(shot, 108, 80), background & 0x00FFFFFFu);
  // 偏移错了会在这里留下底色：area 左上方不该被画到
  CHECK_EQ(rgbAt(shot, 99, 80), background & 0x00FFFFFFu);
  CHECK_EQ(rgbAt(shot, 100, 79), background & 0x00FFFFFFu);
}

TEST(win_window_canvas_draws_visible_text) {
  // 这条是为一个真实缺陷补的：窗口只把 TextRenderer 交给了 SoftwareBackend，
  // 没有交给 FontCache，于是 uiFont() 返回无效 Font，所有文字被静默跳过——
  // 控件、图标、背景全都在，唯独没有一个字。
  //
  // 为什么会漏这么久：控件测试用 MockTextRenderer（widget_test_support.h 会
  // 把它装进 FontCache），所以离线测试里 uiFont() 永远有效。只有真窗口 +
  // 真字体这条路才会暴露。
  //
  // 因此这里必须断言「FontCache 绑的是平台自己的渲染器」，而不只是
  // 「字体有效」——否则 Mock 让它白白通过。
  Fixture fixture(Fixture::customFrameStyle(), /*visible=*/true);
  CHECK_MSG(fixture.window != nullptr, "第二个窗口建不出来（窗口类已存在时注册失败？）");
  if (fixture.window == nullptr) return;

  CHECK_MSG(FontCache::instance().textRenderer() == platform().textRenderer(),
            "窗口没有把平台的 TextRenderer 绑进 FontCache：真机上所有文字会被跳过");

  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  const Color background = makeColor(0xFF, 0xFF, 0xFF);
  const Color ink = makeColor(0x00, 0x00, 0x00);
  fixture.window->setBackgroundColor(background);

  const Rect area = Rect::at(40, 40, 200, 40);
  {
    auto canvas = fixture.window->createCanvas(area);
    CHECK(canvas != nullptr);
    if (canvas == nullptr) return;
    const Font font = uiFont(14.0f);
    CHECK_MSG(font.isValid(), "uiFont() 拿不到字体：FontCache 没绑定 TextRenderer");
    // 画布的 Painter 用客户区坐标，所以文字矩形也要写成客户区坐标：
    // area 是从 (40,40) 开始的，就画在同一处。
    canvas->painter().drawText(Rect{40, 40, 240, 80}, "AA", ink, font, TextAlign::kLeft);
  }

  const Surface shot = captureClient(rawHandle(fixture.window), Rect::fromSize(400, 300));
  CHECK(shot.valid());
  if (!shot.valid()) return;

  int dark = 0;
  for (int y = 40; y < 80; ++y) {
    for (int x = 40; x < 140; ++x) {
      if (rgbAt(shot, x, y) != (background & 0x00FFFFFFu)) ++dark;
    }
  }
  CHECK_MSG(dark > 20, "画布上没有字形墨迹：文字被跳过了（FontCache 未绑定？）");
}

// ── 输入翻译 ──

TEST(win_window_surrogate_pair_makes_one_codepoint) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  HWND hwnd = rawHandle(fixture.window);
  // 😀 = U+1F600 = D83D DE00。UTF-16 分成两段 WM_CHAR，
  // 后端必须合并成一个码点再交给宿主——否则文本里会出现两个替换符。
  ::SendMessageW(hwnd, WM_CHAR, 0xD83D, 0);
  CHECK_EQ(fixture.host.chars.size(), size_t(0));
  ::SendMessageW(hwnd, WM_CHAR, 0xDE00, 0);

  CHECK_EQ(fixture.host.chars.size(), size_t(1));
  if (!fixture.host.chars.empty()) CHECK_EQ(fixture.host.chars[0], std::uint32_t(0x1F600));
}

TEST(win_window_control_chars_are_not_text) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  HWND hwnd = rawHandle(fixture.window);
  // 退格 / 回车 / Ctrl+A 产生的 0x01 都是「按键语义」，已经在 WM_KEYDOWN
  // 里处理过。当成正文字符塞进输入框会出现方块。
  ::SendMessageW(hwnd, WM_CHAR, 0x08, 0);
  ::SendMessageW(hwnd, WM_CHAR, 0x0D, 0);
  ::SendMessageW(hwnd, WM_CHAR, 0x01, 0);
  CHECK_EQ(fixture.host.chars.size(), size_t(0));

  ::SendMessageW(hwnd, WM_CHAR, 'A', 0);
  CHECK_EQ(fixture.host.chars.size(), size_t(1));
}

TEST(win_window_activate_reaches_host) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  ::SendMessageW(rawHandle(fixture.window), WM_ACTIVATE, WA_ACTIVE, 0);
  CHECK(fixture.host.lastFocused);
  ::SendMessageW(rawHandle(fixture.window), WM_ACTIVATE, WA_INACTIVE, 0);
  CHECK(!fixture.host.lastFocused);
}

// ── 生命周期 ──

TEST(win_window_close_request_can_be_vetoed) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  fixture.host.closeHandled = true;

  fixture.window->close();  // PostMessage(WM_CLOSE)，异步
  pumpMessages();

  CHECK_EQ(fixture.host.closes, 1);
  // 宿主接管了关闭：窗口必须还活着，否则「有未保存的文档」就失效了。
  CHECK(fixture.window->handle() != nullptr);
}

TEST(win_window_close_destroys_handle) {
  Fixture fixture;
  if (fixture.window == nullptr) return;

  PlatformWindow* window = fixture.window;
  fixture.host.closeHandled = false;
  window->close();
  pumpMessages();

  // 句柄已经随窗口销毁而失效；析构时不应再 DestroyWindow 一次。
  CHECK(window->handle() == nullptr);
  fixture.window = nullptr;  // 交给用例外的清理由析构完成
  platform().destroyWindow(window);
}

// ── 平台门面 ──

TEST(win_platform_reports_screen_and_text) {
  Platform& host = platform();

  CHECK(host.screenDpi() >= 96);
  const Rect work = host.workArea();
  CHECK(work.width() > 0);
  CHECK(work.height() > 0);
  CHECK(host.pathSeparator() == '\\');
  CHECK(host.textRenderer() != nullptr);
  // 默认字体族不能是空串：空串会让字体创建退回「系统随便挑一个」，
  // 中文界面就会显示成豆腐块。
  const char* family = host.defaultFontFamily();
  CHECK(family != nullptr && family[0] != '\0');
}

TEST(win_platform_executable_directory_is_absolute_and_trimmed) {
  const std::string dir = platform().executableDirectory();
  CHECK(!dir.empty());
  // 不能有结尾分隔符：调用方都是 dir + "\\" + name 拼的，
  // 多一个会变成双反斜杠（多数 API 能忍，但比较路径时会不等）。
  if (!dir.empty()) {
    CHECK(dir.back() != '\\');
    CHECK(dir.back() != '/');
  }
  // 必须是绝对路径：双击启动时工作目录是别处，用相对路径找资源必然失败。
  CHECK(dir.size() >= 2 && dir[1] == ':');
}

TEST(win_platform_path_prefix_respects_separator_boundary) {
  Platform& host = platform();

  CHECK(host.isPathPrefix("C:\\Data", "C:\\Data\\a.txt"));
  CHECK(host.isPathPrefix("C:\\Data", "C:\\Data"));
  // 大小写不敏感：Windows 的盘符与文件名都不区分
  CHECK(host.isPathPrefix("c:\\data", "C:\\Data\\a.txt"));
  // 前缀必须落在分隔符边界上：C:\Data 不是 C:\Database 的前缀
  CHECK(!host.isPathPrefix("C:\\Data", "C:\\Database\\a.txt"));
  CHECK(!host.isPathPrefix("C:\\Data", "D:\\Data\\a.txt"));
  // 末尾带分隔符也不该改变结论
  CHECK(host.isPathPrefix("C:\\Data\\", "C:\\Data\\a.txt"));
}

TEST(win_platform_clipboard_round_trip) {
  Platform& host = platform();

  // 先存原内容，测完还原：测试不该留下用户的剪贴板被改写。
  const std::string saved = host.clipboardText();

  // 非 ASCII 是重点：ANSI 版 API 会在这里变成问号。
  const std::string sample = "PyUI 剪贴板 round-trip \xF0\x9F\x98\x80";
  if (!host.setClipboardText(sample)) {
    // 剪贴板被别的程序长期占用时不算后端缺陷，跳过而不误报。
    return;
  }
  CHECK_EQ(host.clipboardText(), sample);

  host.setClipboardText(saved);
}

TEST(win_platform_set_null_restores_default_instance) {
  Platform* before = &platform();
  setPlatform(nullptr);
  // 传 nullptr 不清空而是恢复默认：platform() 返回引用，永远不能是空。
  CHECK(&platform() == before);
}

TEST(win_platform_subsystems_are_available) {
  Platform& host = platform();
  CHECK(host.contextMenu() != nullptr);
  CHECK(host.fileAssociations() != nullptr);
  CHECK(host.tray() != nullptr);
  // 菜单支持子菜单是 Windows 的既有能力，不该降级成平铺
  CHECK(host.contextMenu()->supportsSubmenus());
}

// 最大化按钮的悬停必须「进入/离开」成对。
//
// 这条测的是最有价值的不变量：悬停唯一的合法条件是「指针现在指着按钮」。
// 历史上出错的方式是——离开通知只到一次，被排队中的 WM_NCHITTEST 重新
// 点亮，之后再没有第二次通知，灰底永久残留。
//
// 注意这里**真的移动光标**：后端判定悬停以实时光标为准，不信任消息里带的
// 坐标（那条队列坐标正是残留的成因）。所以「把光标放到按钮上」是进入悬停的
// 唯一合法方式，测试也照这个前提来。
TEST(win_window_max_button_hover_clears_when_cursor_leaves) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  const HWND hwnd = rawHandle(fixture.window);
  fixture.window->setBounds(Rect::at(220, 220, 400, 300));
  pumpMessages();

  const Point origin = fixture.window->clientOrigin();
  const Rect client = fixture.window->clientRect();

  // 把光标放到客户区内的按钮热区（顶部一行），并让宿主回答「这里是按钮」。
  POINT saved{};
  ::GetCursorPos(&saved);
  fixture.host.nextHit = HitTarget::kMaxButton;
  fixture.host.mouseMoves = 0;
  fixture.host.mouseLeaves = 0;
  ::SetCursorPos(origin.x + 100, origin.y + 10);
  ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(origin.x + 100, origin.y + 10));
  const bool entered = fixture.host.mouseMoves > 0;

  // 光标移到窗口外（按钮一定不在那里），再发离开通知：必须清除。
  ::SetCursorPos(origin.x + 100, origin.y - 60);
  fixture.host.nextHit = HitTarget::kClient;
  ::SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0);
  const bool cleared = fixture.host.mouseLeaves > 0;

  // 无论进入是否成功（光标被别的窗口抢走时长这样），**清除必须发生**：
  // 悬停不能因为「没进去」或「进去了但离开通知丢了」而卡住。
  ::SetCursorPos(saved.x, saved.y);
  CHECK(entered);
  CHECK(cleared);
  (void)client;
}

// 悬停期间同一位置不重复推 MouseMove。
// 这是灰底残留的另一半病因：坐标没变却每条命中消息都推一次，悬停被反复
// 「点亮」，清掉之后立刻又亮。
TEST(win_window_max_button_hover_is_idempotent_per_position) {
  Fixture fixture;
  if (fixture.window == nullptr) return;
  const HWND hwnd = rawHandle(fixture.window);
  fixture.window->setBounds(Rect::at(200, 200, 400, 300));
  pumpMessages();

  fixture.host.nextHit = HitTarget::kMaxButton;
  const Point origin = fixture.window->clientOrigin();
  fixture.host.mouseMoves = 0;

  for (int i = 0; i < 3; ++i) {
    ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(origin.x + 120, origin.y + 10));
  }
  // 位置没变，只应产生一次 MouseMove。真实光标位置在这里是固定的
  // （测试没有移动鼠标），所以这条断言不依赖光标漂移。
  CHECK(fixture.host.mouseMoves <= 1);
}

}  // namespace test
}  // namespace pyui

#endif  // PYUI_OS_WINDOWS
