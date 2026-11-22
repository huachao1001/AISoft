// 平台门面：应用与通用代码只看到这一个入口
//
// 刻意不做成一个大而全的接口：窗口、标题栏、右键菜单、文件关联、托盘各自
// 独立成头文件，缺哪个平台就只缺那一个实现，其余照常工作。
// 这里只聚合「跨所有平台都得有」的几项基础设施。
//
// 实现位置：src/os/<platform>/platform_<platform>.cpp，按 PYUI_OS_* 选择。
// 通用实现（src/ 顶层、src/widgets/）不允许 #include <windows.h>，
// 也不允许出现 #ifdef _WIN32 —— 平台差异只在这里之下。
#pragma once

#include "pyui/platform/context_menu.h"
#include "pyui/platform/file_association.h"
#include "pyui/platform/title_bar.h"
#include "pyui/platform/tray.h"
#include "pyui/platform/window.h"
#include "pyui/text_renderer.h"
#include "pyui/types.h"

#include <memory>
#include <string>
#include <vector>

namespace pyui {

class Platform {
 public:
  virtual ~Platform() = default;

  // ── 窗口 ──
  PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                               WindowHost& host) {
    return createPlatformWindow(parent, params, host);
  }
  void destroyWindow(PlatformWindow* window) { destroyPlatformWindow(window); }

  // ── 屏幕 ──
  virtual int screenDpi() = 0;
  // 屏幕可用区域（不含任务栏/菜单栏）；窗口居中与尺寸收敛用
  virtual Rect workArea() = 0;

  // ── 事件循环 ──
  // 进入主消息循环，直到 quitEventLoop 被调用。返回退出码。
  virtual int runEventLoop() = 0;
  virtual void quitEventLoop(int exitCode) = 0;

  // ── 文本 ──
  // 平台的文本渲染器。若返回 nullptr，则只能用 SoftwareBackend 的占位字形
  // （离线渲染/测试），正式界面必须有实现。
  virtual TextRenderer* textRenderer() = 0;
  // 系统默认界面字体族（UTF-8）。各平台自带的中文/西文界面字体不同。
  virtual const char* defaultFontFamily() = 0;

  // ── 剪贴板 ──
  virtual bool setClipboardText(const std::string& text) = 0;
  // 读纯文本；没有文本内容时返回空串
  virtual std::string clipboardText() = 0;

  // ── 路径 ──
  // 是否同一条路径前缀（忽略大小写，按分隔符边界判断）
  virtual bool isPathPrefix(const std::string& prefix, const std::string& path) = 0;
  // 路径比较：忽略大小写且按系统排序规则。Windows 的盘符与扩展名都不区分
  // 大小写，不应直接拿 std::string 比较。
  virtual bool equalsPath(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
      const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
      if (ca != cb) return false;
    }
    return true;
  }
  virtual char pathSeparator() const = 0;
  // 可执行文件所在目录（UTF-8，无尾分隔符）。资源定位以它为基准，
  // 不能用当前工作目录——双击启动时工作目录是别处。
  virtual std::string executableDirectory() = 0;
  // 用户主目录（UTF-8，无尾分隔符）。示例程序往里写用户数据，
  // 这样不必为了一个「另存到桌面」去实现完整的系统对话框。
  // 没有概念的平台返回空串。
  virtual std::string homeDirectory() { return std::string(); }

  // ── 其它能力 ──
  virtual ContextMenuHost* contextMenu() = 0;
  virtual FileAssociationHost* fileAssociations() = 0;
  virtual TrayHost* tray() = 0;

  // ── 应用图标 ──
  // 本程序自己的那个图标（任务栏 / ALT+Tab / 自绘标题栏左上角）。
  //
  // pixelSize 是**设备像素**边长，不是设计值：调用方自己乘 DPI。返回的
  // Image 里是一支平台分配的位图，所有权归平台，调用方只借用（与
  // TrayHost 的图标语义一致）。拿不到时返回无效 Image。
  //
  // 为什么放在 Platform 而不是让 TitleBar 自己去取：核心层不认识 exe 资源，
  // 也不该认识 HICON。放这里之后，macOS 返回 NSImage 对应的位图、Linux
  // 返回 .desktop 里的 icon，通用层一行都不用改。
  virtual Image applicationIcon(int pixelSize) {
    (void)pixelSize;
    return Image();
  }

  // ── 命令行 ──
  // 进程启动参数（UTF-8，含 argv[0]）。
  //
  // 为什么不让生成入口自己接 argv：Windows 的图形入口是 wWinMain，拿到的是
  // UTF-16 命令行；在入口里拼一遍 CommandLineToArgvW 等于把平台细节写进
  // 每个应用的生成代码。放到平台层之后，生成的入口只剩一句
  // `py::run(*script)`，三平台完全一样。
  virtual std::vector<std::string> commandLineArguments() { return {}; }

  // ── 进程级 DPI 感知 ──
  // 必须在建窗口**之前**调用。Windows 不声明感知时系统按 96 DPI 虚拟化整个
  // 进程，界面会被位图放大成糊的——自绘库尤其明显，因为每个字都是我们
  // 自己栅格化的，位图放大等于把抗锯齿白白丢掉。
  //
  // macOS 天生就是每屏独立缩放，Linux 由 Xft/工具链处理，两者的实现是空操作；
  // 默认实现留空也正是这个原因。
  virtual void enableHighDpi() {}
};

// 进程内唯一的平台实例。未实现平台返回一个空实现（所有能力降级为
// 「不支持」而不是崩溃），这样单元测试可以在无窗口环境跑。
Platform& platform();

// 注入自定义实现，主要用于测试与离线渲染。传 nullptr 恢复默认。
void setPlatform(Platform* instance);

// 便捷入口：多数调用方只关心其中一两项
inline TextRenderer* textRenderer() { return platform().textRenderer(); }
inline int screenDpi() { return platform().screenDpi(); }
inline Rect workArea() { return platform().workArea(); }
// 进程启动时调用一次，必须在建窗口之前
inline void enableHighDpi() { platform().enableHighDpi(); }

}  // namespace pyui
