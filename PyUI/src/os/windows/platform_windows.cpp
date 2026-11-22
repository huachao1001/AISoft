// Windows 平台门面：把各子系统收成一个 Platform
//
// 这里只做「聚合」与「进程级单例」，不加任何业务判断。窗口、菜单、关联、
// 托盘各自在独立文件里实现，这里只持有它们并转发。
//
// 为什么 platform() 要返回引用而不是指针：调用方几乎从不判空
// （textRenderer()、screenDpi() 无处不在），返回指针会让每个调用点都长成
// `if (auto* p = platform()) ...`。未实现的平台由默认实现整体降级，
// 那个对象永远存在，所以引用是安全的。
#include "pyui/platform/platform.h"

#include "pyui/platform/context_menu.h"
#include "pyui/platform/file_association.h"
#include "pyui/platform/tray.h"

#include "gdi_text_renderer.h"
#include "win_common.h"
#include "win_platform_hosts.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pyui {
namespace {

// 事件循环的退出码。全局而不是 Platform 成员：quitEventLoop 可能在窗口
// 回调、定时器、甚至另一个模块里被调用，而进程只该有一个退出码。
int& exitCode() {
  static int code = 0;
  return code;
}
bool& quitRequested() {
  static bool quit = false;
  return quit;
}

// 记录本进程的主消息循环线程。
// quitEventLoop 可能从别的线程（工作线程里发现要退出）调用，而
// PostThreadMessage 必须投给创建窗口队列的那个线程，否则消息没人收，
// GetMessage 会永远阻塞。
DWORD& loopThreadId() {
  static DWORD id = 0;
  return id;
}

// 屏幕尺寸与工作区。统一用**主显示器**：窗口未创建时的居中、以及
// 「一棵树只有一份 DPI」的模型都要求一个确定的答案。多显示器混合 DPI
// 时逐窗口取 DPI（PlatformWindow::dpi）才是对的，这里只服务全局查询。
Rect primaryWorkArea() {
  RECT rc{};
  if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0) != 0) {
    return Rect{rc.left, rc.top, rc.right, rc.bottom};
  }
  return Rect::fromSize(::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
}

int primaryDpi() {
  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) return 96;
  const int dpi = ::GetDeviceCaps(screen, LOGPIXELSX);
  ::ReleaseDC(nullptr, screen);
  return dpi > 0 ? dpi : 96;
}

// 剪贴板：只处理 CF_UNICODETEXT。
//
// 为什么不接 CF_TEXT：ANSI 版本会丢非当前代码页的字符。我们要么给正确的
// UTF-8，要么明确失败——静默把「确定」变成「??」比不支持更糟。
bool openClipboardRetry(HWND owner) {
  // 别的程序可能正拿着剪贴板（几百毫秒的正常行为）。重试几次再放弃，
  // 一次性失败会让「复制」偶尔失灵，用户只会觉得程序有 bug。
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (::OpenClipboard(owner) != 0) return true;
    ::Sleep(10);
  }
  return false;
}

class WindowsPlatform final : public Platform {
 public:
  WindowsPlatform() : contextMenu_(), fileAssociations_(), tray_() {}

  // ── 屏幕 ──
  int screenDpi() override { return primaryDpi(); }
  Rect workArea() override { return primaryWorkArea(); }

  // ── 事件循环 ──
  int runEventLoop() override {
    // 刻意不清 quitRequested()：调用方可能在进入循环**之前**就请求退出
    // （测试里、"启动即退出"的参数校验失败时）。清了就等于吞掉那条请求，
    // 循环会一直挂着等消息。
    loopThreadId() = ::GetCurrentThreadId();
    MSG msg{};
    // GetMessage 返回 0 是 WM_QUIT，-1 是错误。两者都要退出循环，
    // 但错误要记下来——否则「窗口一闪就没」会查不出原因。
    while (!quitRequested()) {
      const BOOL result = ::GetMessageW(&msg, nullptr, 0, 0);
      if (result == 0) break;
      if (result == -1) {
        exitCode() = 1;
        break;
      }
      // TranslateMessage 生成 WM_CHAR（输入框靠它收字符）；
      // DispatchMessage 才能让窗口过程拿到消息。两个都不能省。
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    loopThreadId() = 0;
    // 一次循环结束就消费掉退出请求：下一次 runEventLoop 应当能重新跑起来。
    quitRequested() = false;
    return exitCode();
  }

  void quitEventLoop(int code) override {
    exitCode() = code;
    quitRequested() = true;
    // 循环里可能正阻塞在 GetMessage（没有任何消息）。投一个空消息把它叫醒。
    // 投给循环线程而不是当前线程：两者相同时是这个调用的常见情形，
    // 不同时（工作线程请求退出）投错了就永远叫不醒。
    const DWORD thread = loopThreadId();
    ::PostThreadMessageW(thread != 0 ? thread : ::GetCurrentThreadId(), WM_NULL, 0, 0);
  }

  // ── 文本 ──
  TextRenderer* textRenderer() override { return &textRenderer_; }
  const char* defaultFontFamily() override { return textRenderer_.defaultFamily(); }

  // ── 剪贴板 ──
  bool setClipboardText(const std::string& text) override {
    const std::wstring wide = win32::toWide(text);
    // owner 传 nullptr：这是「后台写剪贴板」，不绑定某个窗口。
    // 绑了窗口的话，窗口一销毁系统就会清空剪贴板。
    if (!openClipboardRetry(nullptr)) return false;

    ::EmptyClipboard();
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
      ::CloseClipboard();
      return false;
    }
    void* target = ::GlobalLock(handle);
    if (target == nullptr) {
      ::GlobalFree(handle);
      ::CloseClipboard();
      return false;
    }
    std::memcpy(target, wide.c_str(), bytes);
    ::GlobalUnlock(handle);

    // SetClipboardData 成功后所有权归系统，失败时我们自己释放。
    if (::SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
      ::GlobalFree(handle);
      ::CloseClipboard();
      return false;
    }
    ::CloseClipboard();
    return true;
  }

  std::string clipboardText() override {
    if (!openClipboardRetry(nullptr)) return std::string();
    std::string out;
    HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
    if (handle != nullptr) {
      const auto* text = static_cast<const wchar_t*>(::GlobalLock(handle));
      if (text != nullptr) {
        out = win32::toUtf8(text);
        ::GlobalUnlock(handle);
      }
    }
    ::CloseClipboard();
    return out;
  }

  // ── 路径 ──
  bool isPathPrefix(const std::string& prefix, const std::string& path) override {
    if (prefix.empty()) return true;
    std::wstring a = win32::toWidePath(prefix);
    std::wstring b = win32::toWidePath(path);
    // 统一成小写再比：Windows 的盘符与文件名都不区分大小写。
    // 不转成小写会让 C:\Data 与 c:\data 被当成两个目录。
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return ::towlower(c); });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return ::towlower(c); });

    // 去掉尾部分隔符，避免 C:\Data\ 与 C:\Data 的结果不一致
    while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
    if (b.size() < a.size()) return false;
    if (b.compare(0, a.size(), a) != 0) return false;
    // 必须落在分隔符边界上：C:\Data 不是 C:\Database 的前缀
    return b.size() == a.size() || b[a.size()] == L'\\' || b[a.size()] == L'/';
  }

  char pathSeparator() const override { return '\\'; }

  std::string executableDirectory() override {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
      const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
      if (length == 0) return std::string();
      if (length < buffer.size()) {
        buffer.resize(length);
        break;
      }
      // 截断了：路径比缓冲长，翻倍重取。不重取会返回一个被截断的目录，
      // 资源加载就会「看起来正常但永远找不到文件」。
      buffer.resize(buffer.size() * 2);
    }

    // 去掉文件名，只留目录，再去掉尾分隔符
    const size_t slash = buffer.find_last_of(L"\\/");
    if (slash != std::wstring::npos) buffer.resize(slash);
    return win32::toUtf8(buffer);
  }

  std::string homeDirectory() override {
    // USERPROFILE 是 Windows 上「我的文档」的父目录；没有它时退回
    // HOMEDRIVE+HOMEPATH（域账户下偶尔只设后两者）。两者都没有就返回空串，
    // 让调用方自己决定降级行为，而不是塞一个 "." 进去假装成功。
    wchar_t buffer[MAX_PATH];
    const DWORD length = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return win32::toUtf8(std::wstring(buffer, length));

    wchar_t drive[MAX_PATH];
    wchar_t homePath[MAX_PATH];
    const DWORD driveLength = ::GetEnvironmentVariableW(L"HOMEDRIVE", drive, MAX_PATH);
    const DWORD homeLength = ::GetEnvironmentVariableW(L"HOMEPATH", homePath, MAX_PATH);
    if (driveLength > 0 && driveLength < MAX_PATH && homeLength > 0 && homeLength < MAX_PATH) {
      return win32::toUtf8(std::wstring(drive, driveLength) +
                           std::wstring(homePath, homeLength));
    }
    return std::string();
  }

  // ── 其它能力 ──
  ContextMenuHost* contextMenu() override { return &contextMenu_; }
  FileAssociationHost* fileAssociations() override { return &fileAssociations_; }
  TrayHost* tray() override { return &tray_; }

  Image applicationIcon(int pixelSize) override {
    // 按尺寸缓存：标题栏每次重排都会问一次，而 PrivateExtractIconsW 要读
    // exe 资源，不能每次重做。
    const int size = pixelSize > 0 ? pixelSize : 16;
    auto found = appIcons_.find(size);
    if (found != appIcons_.end()) return found->second;

    // loadApplicationIcon 的契约是「返回值由调用方 DestroyIcon」——它内部
    // 已经为共享图标做过 CopyIcon，所以这里无条件销毁是对的。
    HICON icon = win32::loadApplicationIcon(size);
    Image result = win32::iconToImage(icon, size);
    if (icon != nullptr) ::DestroyIcon(icon);

    appIcons_[size] = result;
    return result;
  }

  std::vector<std::string> commandLineArguments() override {
    int count = 0;
    wchar_t** wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (wide == nullptr) return {};

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) arguments.push_back(win32::toUtf8(wide[i]));
    ::LocalFree(wide);
    return arguments;
  }

  void enableHighDpi() override {
    // Per-Monitor-V2。必须比 SetProcessDPIAware（V1）更明确：
    // 只有 V2 才会让 WM_DPICHANGED 在窗口被拖到另一块屏时真的发过来，
    // V1 下系统会把整个窗口位图缩放，字会糊。
    //
    // 用 GetProcAddress 而不是静态链接：user32 在 Windows 7 上没有这个
    // 导出，虽然我们只支持 Win10+，但让老系统直接启动失败没有意义——
    // 取不到就退回 V1，界面仍然可用。
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
      const auto setContext = reinterpret_cast<SetContextFn>(
          ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
      if (setContext != nullptr && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
      }
    }
    // 已经声明过（例如清单里写了）会返回失败，这是正常的，不需要报错
    ::SetProcessDPIAware();
  }

 private:
  GdiTextRenderer textRenderer_;
  // 三个子系统都是「进程内一份」：右键菜单同时只该有一个，托盘图标
  // 只该有一个，关联登记是纯函数。按值持有，省掉生命周期管理。
  WindowsContextMenu contextMenu_;
  WindowsFileAssociations fileAssociations_;
  WindowsTray tray_;
  // 应用图标按设备像素边长缓存：Image 里的 Surface* 由这里持有。
  std::map<int, Image> appIcons_;
};

// 默认平台：不支持任何能力，但绝不崩。
//
// 为什么需要它：单元测试、离线渲染、以及「在 Linux 上跑 Windows 的布局
// 测试」都需要 platform() 能返回一个对象。每个方法给一个安全的降级值，
// 而不是断言 —— 测试里断言只会让「本来就不该有窗口」的用例全部失败。
class NullPlatform final : public Platform {
 public:
  int screenDpi() override { return 96; }
  Rect workArea() override { return Rect::fromSize(1920, 1080); }
  int runEventLoop() override { return 0; }
  void quitEventLoop(int code) override { (void)code; }
  TextRenderer* textRenderer() override { return nullptr; }
  const char* defaultFontFamily() override { return ""; }
  bool setClipboardText(const std::string& text) override {
    (void)text;
    return false;
  }
  std::string clipboardText() override { return std::string(); }
  bool isPathPrefix(const std::string& prefix, const std::string& path) override {
    if (prefix.empty()) return true;
    if (path.size() < prefix.size()) return false;
    return path.compare(0, prefix.size(), prefix) == 0;
  }
  char pathSeparator() const override { return '/'; }
  std::string executableDirectory() override { return "."; }
  ContextMenuHost* contextMenu() override { return nullptr; }
  FileAssociationHost* fileAssociations() override { return nullptr; }
  TrayHost* tray() override { return nullptr; }
};

Platform*& installed() {
  static Platform* instance = nullptr;
  return instance;
}

}  // namespace

Platform& platform() {
  Platform*& slot = installed();
  if (slot == nullptr) {
    // 延迟构造：静态初始化顺序在跨模块时不确定，
    // 而 GdiTextRenderer 的构造函数要调 SystemParametersInfoW。
    static WindowsPlatform defaultPlatform;
    slot = &defaultPlatform;
  }
  return *slot;
}

void setPlatform(Platform* instance) {
  // 传 nullptr 不清空而是恢复默认：置空会让之后每个 platform() 调用都
  // 需要判空，而它返回的是引用。
  installed() = instance;
}

// 供后端内部与测试复用：显式拿一个「什么都不会」的平台。
Platform* nullPlatform() {
  static NullPlatform instance;
  return &instance;
}

}  // namespace pyui
