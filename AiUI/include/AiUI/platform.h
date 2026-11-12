// 平台后端接口
//
// 视图只通过这里与操作系统打交道。所有平台相关实现集中在 src/os/<platform>/，
// 换平台时新增一份实现即可，控件代码一行不用改。
//
// 关键约束：整棵视图树共用一个窗口。ViewRoot 是唯一持有 PlatformWindow 的
// 对象，子视图不创建任何原生句柄，因此这里不再提供“每个控件一个窗口”的入口。
#pragma once

#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AiUI {

class Canvas;

// 非客户区命中结果：平台据此决定交给系统（拖动、缩放）还是客户区控件。
// 自绘标题栏靠它既能保留系统拖动/贴边吸附，又能让按钮收到鼠标事件。
enum class HitTarget {
  kClient,
  kCaption,
  kLeft,
  kRight,
  kTop,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
  // 最大化按钮：交给系统后，悬停会弹出贴靠布局（Snap Layouts）
  kMaxButton,
};

// ── 文件系统 ──

// 目录条目：只带界面需要的元数据，不暴露平台的文件句柄
struct DirEntry {
  std::wstring name;
  std::wstring fullPath;
  bool isDir = false;
  uint64_t size = 0;
  // 文件类型描述（如「文本文档」「文件夹」）；平台给不出时留空
  std::wstring typeName;
  // 最后修改时间，平台纪元毫秒；0 表示未知
  int64_t modifiedMillis = 0;
  // 平台属性位。语义随平台不同，只用于原样透传（如压缩时写回包里），
  // 不参与任何跨平台判断。
  uint32_t attributes = 0;
};

// 常见系统位置
enum class SpecialFolder {
  kDesktop,
  kHome,
  kDocuments,
  kDownloads,
  kTemp,
};

// 磁盘卷
struct DriveInfo {
  std::wstring label;  // 显示名，如「本地磁盘 (C:)」
  std::wstring path;   // 根路径，如 "C:\\"
  bool removable = false;
};

// ── 文件流 ──
// 压缩内核这类代码要随机读写大文件，不能一次性读进内存。
// 上层只拿这个接口，原生句柄封在各平台后端里。
enum class SeekOrigin { kBegin, kCurrent, kEnd };

class FileStream {
 public:
  virtual ~FileStream() = default;

  // 返回实际读/写的字节数；出错返回 -1
  virtual int64_t read(void* data, int64_t size) = 0;
  virtual int64_t write(const void* data, int64_t size) = 0;
  // 成功时把新位置写进 position
  virtual bool seek(int64_t offset, SeekOrigin origin, uint64_t* position) = 0;
  // 截断或扩展到指定长度
  virtual bool setSize(uint64_t size) = 0;
};

// ── 右键菜单项 ──
// 资源管理器/访达里挂到文件上的动作。命令行的拼接方式由后端决定。
struct ContextMenuEntry {
  std::wstring verb;   // 唯一标识，如 "AIZip.Extract"
  std::wstring label;  // 显示文字
  std::wstring command;  // 命令行开关，如 "-extract"
  // 挂到哪些类型：通配符后缀（"*" 为所有文件）；空表示所有文件与目录
  std::vector<std::wstring> extensions;
  bool topLevel = false;
};

// ── 弹出菜单 ──
struct MenuItem {
  std::wstring label;
  bool enabled = true;
  // 分隔线；此时 label 被忽略
  bool separator = false;
  // 左侧显示勾选标记；下拉框用它标出当前项
  bool checked = false;
};

// ── 消息框 ──

enum class MessageBoxIcon { kNone, kInfo, kWarning, kError, kQuestion };

enum class MessageBoxButtons { kOk, kOkCancel, kYesNo, kYesNoCancel };

enum class MessageBoxResult { kOk, kCancel, kYes, kNo };

struct MessageBoxParams {
  std::wstring title;
  std::wstring message;
  MessageBoxIcon icon = MessageBoxIcon::kInfo;
  MessageBoxButtons buttons = MessageBoxButtons::kOk;
  // 默认聚焦的按钮：kNo 表示默认停在「否」，避免误触删除
  MessageBoxResult defaultButton = MessageBoxResult::kOk;
};

// 窗口把原生事件翻译成中性事件后回调宿主。
// 整棵树共用一个窗口，所以这里只有一套回调。
class WindowHost {
 public:
  virtual ~WindowHost() = default;

  // 需要重绘；update 是本次要重画的窗口区域
  virtual void onExpose(const Rect& update) = 0;
  virtual void onResize(int width, int height) = 0;

  virtual void onMouseDown(const MouseEvent& e) = 0;
  virtual void onMouseUp(const MouseEvent& e) = 0;
  virtual void onMouseMove(const MouseEvent& e) = 0;
  virtual void onMouseLeave() = 0;
  virtual void onMouseWheel(const MouseEvent& e) = 0;
  virtual void onDoubleClick(const MouseEvent& e) = 0;
  virtual void onContextMenu(const MouseEvent& e) = 0;

  // 返回 true 表示已消费
  virtual bool onKeyDown(const Key& key, const Modifiers& mods) = 0;
  virtual bool onChar(wchar_t ch) = 0;
  virtual void onFocusChanged(bool focused) = 0;
  virtual void onTimer() = 0;
  // 一次性延时回调到期
  virtual void onDelayTimer() {}

  // 非客户区命中：窗口坐标。标题栏返回 kCaption、边框返回缩放方向，
  // 默认 kClient 表示全部按客户区处理。
  virtual HitTarget onHitTest(const Point& position) {
    (void)position;
    return HitTarget::kClient;
  }

  // 关闭请求（Alt+F4、系统菜单、平台侧关闭）；返回 true 表示已处理，
  // 平台不再销毁窗口。默认返回 false 走正常关闭。
  virtual bool onCloseRequest() { return false; }
};

// 窗口外观属性
struct WindowStyle {
  // 顶层窗口保留系统外框（投影、圆角、边缘缩放）；子窗口只铺满父客户区
  bool topLevel = false;
  bool resizable = true;
  bool minimizable = true;
  bool maximizable = true;
  // 自绘标题栏的视觉总高（96 DPI 设计值）；0 表示等于逻辑高度。
  // 逻辑高度（按钮悬停底色与垂直居中的基准）由 TitleBar::setBandHeight 控制。
  int captionHeight = 0;
  // 左/右/下三条边的缩放带宽度（96 DPI 设计值），贴外沿即触发
  int borderResizeWidth = 4;
  // 顶部缩放带高度（96 DPI 设计值）。顶部有标题栏，缩放带落在内侧，
  // 比其他三条边宽一点才好抓。
  int topResizeWidth = 8;
};

// 窗口创建参数
struct WindowParams {
  bool visible = true;
  bool tabStop = true;
  std::wstring className;  // 窗口类名，需在进程内唯一
  std::wstring title;
  WindowStyle style;
  // 最小尺寸（96 DPI 设计值），0 表示不限制
  int minWidth = 0;
  int minHeight = 0;
};
// 平台窗口：整棵视图树共用一个
class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;

  virtual NativeHandle handle() const = 0;
  virtual Rect clientRect() const = 0;
  virtual int dpi() const = 0;

  // 离屏画布，只覆盖 area（窗口客户区坐标）；nullptr 表示当前无法绘制
  virtual std::unique_ptr<Canvas> createCanvas(const Rect& area) = 0;

  virtual void setBounds(const Rect& bounds) = 0;
  // 含边框与标题栏的整窗矩形，居中与吸附用
  virtual Rect windowRect() const = 0;
  // 客户区左上的屏幕坐标；弹出菜单定位用
  virtual Point clientOrigin() const { return Point{windowRect().left, windowRect().top}; }
  virtual void show(bool visible) = 0;
  virtual void invalidate() = 0;
  // 只失效一块区域，用于悬停、选中这类局部更新
  virtual void invalidateRect(const Rect& rc) = 0;
  // 窗口背景色：窗口变大时系统先铺它，避免新增区域露出黑底
  virtual void setBackgroundColor(Color color) { (void)color; }
  virtual void setFocus() = 0;
  virtual void setCursorShape(CursorShape shape) = 0;
  virtual void captureMouse(bool capture) = 0;

  // 光标闪烁定时器：整棵树共用一个
  virtual void setBlinkTimer(bool on) = 0;
  // 一次性延时定时器：ms 毫秒后触发 onDelayTimer，再次调用会重新计时；
  // 0 表示取消。光标闪烁是全树共享的周期定时器，滚动条自动隐藏这类
  // 「稍后做一次」的需求需要独立的单次计时。
  virtual void setDelayTimer(int ms) { (void)ms; }
  // 让输入法候选框跟随光标（客户区坐标）
  virtual void setImeCaret(int x, int y) = 0;

  // ── 标题栏与窗口控制 ──
  virtual void setTitle(const std::wstring& title) = 0;
  virtual void minimize() = 0;
  // 最大化与还原之间切换
  virtual void toggleMaximize() = 0;
  virtual bool isMaximized() const = 0;
  // 还原态下的外框矩形；最大化时也返回还原尺寸，用于持久化
  virtual Rect normalBounds() const { return windowRect(); }
  virtual void close() = 0;
  // 拖动移动与从边缘缩放，供自绘标题栏调用
  virtual void startDragMove() = 0;
  virtual void startDragResize(HitTarget edge) = 0;

  // ── 文件拖放 ──
  // 接受从资源管理器/访达拖入的文件。onDrop 收到绝对路径列表，
  // 在 UI 线程调用。传空函数即关闭拖放。
  virtual void setFileDropHandler(std::function<void(const std::vector<std::wstring>& paths)> fn) {
    (void)fn;
  }
};

// 平台后端
class Platform {
 public:
  virtual ~Platform() = default;

  virtual PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                                       WindowHost& host) = 0;
  virtual void destroyWindow(PlatformWindow* window) = 0;

  virtual int screenDpi() = 0;
  // 屏幕可用区域（不含任务栏/菜单栏）；窗口居中与尺寸收敛用
  virtual Rect workArea() = 0;

  // ── 事件循环 ──
  // 进入主消息循环，直到 quitEventLoop 被调用。返回退出码。
  virtual int runEventLoop() = 0;
  virtual void quitEventLoop(int exitCode) = 0;

  // ── 文本与剪贴板 ──
  // 系统默认界面字体族；各平台自带的中文/西文界面字体不同
  virtual const wchar_t* defaultFontFamily() = 0;
  // 写入纯文本；成功返回 true
  virtual bool setClipboardText(const std::wstring& text) = 0;
  // 读纯文本；没有文本内容时返回空串
  virtual std::wstring clipboardText() = 0;
  // 是否同一条路径前缀（忽略大小写，按分隔符边界判断）
  virtual bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) = 0;
  // 路径比较：忽略大小写且按系统排序规则。Windows 的盘符与扩展名都不区分大小写，
  // 不应直接拿 std::wstring 比较。
  virtual bool equalsPath(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
  }
  // 路径分隔符
  virtual wchar_t pathSeparator() = 0;
  // 文本比较：忽略大小写，返回负数/0/正数。列表排序用，
  // 各平台的区域规则不同，不能直接拿 wstring 比较。
  virtual int compareText(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
      const wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
      const wchar_t cb = static_cast<wchar_t>(towlower(b[i]));
      if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
  }
  // 忽略大小写的子串查找；搜索框过滤用
  virtual bool containsTextIgnoreCase(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
      size_t j = 0;
      while (j < needle.size() &&
             towlower(text[i + j]) == towlower(needle[j])) {
        ++j;
      }
      if (j == needle.size()) return true;
    }
    return false;
  }
  // 文本编码：落盘与跨进程传递统一用 UTF-8，不依赖系统区域设置
  virtual std::string toUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) {
      // 超出 ASCII 的直接按 UTF-8 三字节编码
      if (c < 0x80) {
        out.push_back(static_cast<char>(c));
      } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      }
    }
    return out;
  }
  virtual std::wstring fromUtf8(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c < 0x80) {
        out.push_back(static_cast<wchar_t>(c));
        i += 1;
      } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
        out.push_back(static_cast<wchar_t>(((c & 0x1F) << 6) |
                                          (static_cast<unsigned char>(text[i + 1]) & 0x3F)));
        i += 2;
      } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
        out.push_back(static_cast<wchar_t>(((c & 0x0F) << 12) |
                                          ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                                          (static_cast<unsigned char>(text[i + 2]) & 0x3F)));
        i += 3;
      } else {
        // 非法序列跳过一字节，避免死循环
        i += 1;
      }
    }
    return out;
  }

  // ── 字体 ──
  virtual Font createFont(const wchar_t* family, float pointSize, bool bold, int dpi) = 0;
  virtual void destroyFont(void* handle) = 0;
  // 文本像素宽度：不依赖任何窗口，测量阶段直接可用。
  // 含少量安全余量，防末字在裁剪边界被切掉；排版与换行判断用这个。
  virtual int measureText(const std::wstring& text, const Font& font) = 0;
  // 与绘制完全对齐的占位宽度，无安全余量。
  // 光标定位与点击命中必须用它，否则插入点会与绘制结果错开。
  virtual int advanceText(const std::wstring& text, const Font& font) {
    return measureText(text, font);
  }
  // 整串一次排版，返回每个字符的左右边界：长度 = size*2，[2i] 左缘、[2i+1] 右缘。
  // 光标必须按整串定位：逐前缀调用 advanceText 会把每段各自的排版留白累加
  // 进来，文本越长插入点离末字越远。返回空表示不支持，调用方自行退回。
  virtual std::vector<int> glyphBounds(const std::wstring& text, const Font& font) {
    (void)text;
    (void)font;
    return {};
  }

  // ── 图片 ──
  // 从文件解码位图；失败返回无效 Image
  virtual Image loadImage(const std::wstring& path) = 0;
  virtual Size imageSize(const Image& image) = 0;
  virtual void releaseImage(Image image) = 0;
  // 本进程可执行文件的小图标；标题栏左侧显示用，与系统标题栏一致
  virtual Image appIcon() = 0;

  // ── 时间 ──
  // 单调递增毫秒计时器；只需能相互比较差值，不做日历换算
  virtual int64_t monotonicMillis() = 0;

  // ── 应用数据 ──
  // 应用配置目录（各平台约定不同：Windows 用 %APPDATA%，macOS 用
  // ~/Library/Application Support，Linux 用 $XDG_CONFIG_HOME）。目录不存在时创建。
  virtual std::wstring appDataDir(const std::wstring& appName) {
    (void)appName;
    return {};
  }

  // ── 文件系统 ──
  // 下面这组都有默认实现（返回"不支持"），后端可以只实现自己用得上的。
  // 目录下的一级条目。skipHidden 用于文件浏览器这类场景，
  // 系统目录与隐藏目录通常不该出现在左侧树里。
  virtual std::vector<DirEntry> listDirectory(const std::wstring& dir, bool skipHidden) {
    (void)dir;
    (void)skipHidden;
    return {};
  }
  virtual bool pathExists(const std::wstring& path) {
    (void)path;
    return false;
  }
  virtual bool isDirectory(const std::wstring& path) {
    (void)path;
    return false;
  }
  virtual bool createDirectory(const std::wstring& path) {
    (void)path;
    return false;
  }
  // 文件字节数；不是普通文件时返回 0
  virtual uint64_t fileSize(const std::wstring& path) {
    (void)path;
    return 0;
  }
  // 最后修改时间，Unix 纪元毫秒；未知返回 0
  virtual int64_t modifiedTime(const std::wstring& path) {
    (void)path;
    return 0;
  }
  // 设置最后修改时间；解压后还原归档里记录的时间用
  virtual bool setModifiedTime(const std::wstring& path, int64_t millis) {
    (void)path;
    (void)millis;
    return false;
  }
  // 读整个文件；大于 maxBytes 或读失败返回空
  virtual std::string readFile(const std::wstring& path, size_t maxBytes) {
    (void)path;
    (void)maxBytes;
    return {};
  }
  // 覆盖写整个文件；目录不存在时失败
  virtual bool writeFile(const std::wstring& path, const std::string& data) {
    (void)path;
    (void)data;
    return false;
  }
  virtual bool deleteFile(const std::wstring& path) {
    (void)path;
    return false;
  }
  // 打开文件流；openWrite 为 false 时只读。失败返回 nullptr。
  virtual std::unique_ptr<FileStream> openFileStream(const std::wstring& path, bool openWrite) {
    (void)path;
    (void)openWrite;
    return nullptr;
  }
  // 移走或改名；目标已存在时失败
  virtual bool moveFile(const std::wstring& from, const std::wstring& to) {
    (void)from;
    (void)to;
    return false;
  }
  // 绝对化并消去 "." 与 ".."；失败时原样返回
  virtual std::wstring absolutePath(const std::wstring& path) {
    return path;
  }
  // 本进程可执行文件路径
  virtual std::wstring executablePath() { return {}; }
  // 本进程命令行参数（不含程序名），已按平台规则拆好
  virtual std::vector<std::wstring> commandLineArgs() { return {}; }
  // 临时目录
  virtual std::wstring tempDir() { return specialFolder(SpecialFolder::kTemp); }
  // 本进程 id，用于给临时文件起不重名
  virtual uint32_t processId() { return 0; }
  // 移到系统回收站/废纸篓；不支持回收站的平台退化为直接删除
  virtual bool moveToTrash(const std::wstring& path) { return deleteFile(path); }

  // ── 常见系统位置 ──
  // 桌面、用户主目录等。找不到时返回空串。
  virtual std::wstring specialFolder(SpecialFolder folder) {
    (void)folder;
    return {};
  }
  // 挂载的磁盘卷；文件系统视图的树根用
  virtual std::vector<DriveInfo> listDrives() { return {}; }

  // ── 消息框 ──
  // 阻塞直到用户响应。owner 为空时挂到前台窗口。
  virtual MessageBoxResult showMessageBox(NativeHandle owner, const MessageBoxParams& params) {
    (void)owner;
    (void)params;
    return MessageBoxResult::kOk;
  }
  // 该句柄当前适合当模态 owner 吗：窗口仍存在且未最小化。
  // 最小化时被 owner 的弹窗会一起隐藏，此时应退化为无 owner。
  virtual bool canOwnModal(NativeHandle window) {
    (void)window;
    return false;
  }

  // ── 弹出菜单 ──
  // 在屏幕坐标 position 处弹出；返回被选中项的下标，取消返回 -1。
  // 后端负责把菜单摆到屏幕内、以及失焦消失。
  virtual int showPopupMenu(NativeHandle owner, const Point& position,
                            const std::vector<MenuItem>& items) {
    (void)owner;
    (void)position;
    (void)items;
    return -1;
  }
  // 鼠标屏幕坐标；右键菜单用它取弹出位置
  virtual Point cursorPosition() { return Point{0, 0}; }
  // 把窗口提到前台。弹出菜单前必须调用，否则点别处菜单不消失。
  virtual void bringToFront(NativeHandle window) { (void)window; }

  // ── 系统集成 ──
  // 把动作挂到系统右键菜单；返回空串表示成功，否则是可读错误
  virtual std::wstring registerContextMenu(const std::vector<ContextMenuEntry>& entries) {
    (void)entries;
    return L"当前平台不支持";
  }
  virtual std::wstring unregisterContextMenu(const std::vector<ContextMenuEntry>& entries) {
    (void)entries;
    return L"当前平台不支持";
  }
  // 列出的动作是否都已挂好且指向当前可执行文件
  virtual bool isContextMenuRegistered(const std::vector<ContextMenuEntry>& entries) {
    (void)entries;
    return false;
  }

  // ── 进程与线程 ──
  // 当前是否在 UI 线程上；工作线程直接改控件要走 postToUiThread
  virtual bool isUiThread() { return true; }
  // 让出 CPU 一小段时间；聚合多进程请求这类轮询用
  virtual void sleepMillis(int ms) { (void)ms; }
  // 屏幕缩放比例（1.0 = 96 DPI）；跨屏移动时窗口可能变化
  virtual float scaleFactor() { return screenDpi() / 96.0f; }

  // ── 文件选择 ──
  // 系统原生选择器：这部分不自绘，各平台的原生体验差异太大
  virtual std::wstring pickFolder(NativeHandle owner, const std::wstring& title,
                                  const std::wstring& initialDir) {
    (void)owner;
    (void)title;
    (void)initialDir;
    return {};
  }
  // filters 为「显示名|通配符」成对出现，如 {"压缩包", "*.zip;*.7z"}
  virtual std::wstring pickSaveFile(NativeHandle owner, const std::wstring& title,
                                    const std::wstring& initialDir,
                                    const std::wstring& defaultName,
                                    const std::wstring& defaultExt,
                                    const std::vector<std::wstring>& filters) {
    (void)owner;
    (void)title;
    (void)initialDir;
    (void)defaultName;
    (void)defaultExt;
    (void)filters;
    return {};
  }
  virtual std::wstring pickOpenFile(NativeHandle owner, const std::wstring& title,
                                    const std::wstring& initialDir,
                                    const std::vector<std::wstring>& filters) {
    (void)owner;
    (void)title;
    (void)initialDir;
    (void)filters;
    return {};
  }

  // ── Shell 集成 ──
  // 用系统关联程序打开文件或目录
  virtual bool openWithSystem(const std::wstring& path) {
    (void)path;
    return false;
  }
  // 显示系统属性面板
  virtual bool showProperties(NativeHandle owner, const std::wstring& path) {
    (void)owner;
    (void)path;
    return false;
  }

  // ── 线程投递 ──
  // 把 fn 排到 UI 线程执行。工作线程回传进度、弹询问框都靠这个，
  // 调用方不得假设它同步执行。默认实现直接调用：单线程后端可用，
  // 有工作线程的后端必须覆盖，否则会在工作线程上跑 UI 代码。
  virtual void postToUiThread(std::function<void()> fn) {
    if (fn) fn();
  }
  // 在工作线程请求 UI 线程执行并等待完成；本来就在 UI 线程时直接执行。
  // 进度框这类需要"改完再继续算"的场景用；postToUiThread 只保证最终会跑。
  virtual void invokeOnUiThread(std::function<void()> fn) {
    if (fn) fn();
  }

  // ── 单实例与进程间转递 ──
  // 同一应用只允许一个实例时，第二个实例把启动参数交给第一个。
  // 返回 false 表示已有实例在运行，本进程应退出。
  virtual bool claimSingleInstance(const std::wstring& appId) {
    (void)appId;
    return true;
  }
  // 注册被唤醒时的回调；payload 是另一个实例传来的命令行
  virtual void setLaunchPayloadHandler(std::function<void(const std::wstring& payload)> fn) {
    (void)fn;
  }
  // 把启动参数交给已运行的实例；成功返回 true
  virtual bool forwardToRunningInstance(const std::wstring& appId,
                                        const std::wstring& payload) {
    (void)appId;
    (void)payload;
    return false;
  }

  // ── 绘制子系统 ──
  virtual void startupDrawing() = 0;
  virtual void shutdownDrawing() = 0;
  virtual bool isDrawingReady() = 0;
};

// 当前平台后端；进程内唯一
Platform* platform();

// 测试用：替换当前平台后端，传 nullptr 恢复默认实现
void setPlatformForTesting(Platform* platform);

// 宿主启动/退出时调用
void startupPlatform();
void shutdownPlatform();
bool isPlatformReady();

// 进入主循环前的平台初始化：DPI 感知、公共控件、线程投递窗口。
// 必须在创建任何窗口之前调用。
void initHostProcess();
// 进程级初始化失败时的最后提示；此时图形界面还不可用
void reportFatalError(const wchar_t* message);

// 模态循环：禁用 owner 并只在自己窗口存活期间跑消息循环。
// owner 为空或不属于本进程时按非模态处理，避免锁死外部界面。
void runModalLoop(NativeHandle modal, NativeHandle owner);

}  // namespace AiUI
