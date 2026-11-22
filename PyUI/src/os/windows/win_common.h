// Win32 后端的共享小工具
//
// 只给 src/os/windows/ 下的实现用；通用层与公开头文件都看不到它
// （AGENTS.md 铁律一）。
#pragma once

// 本文件的 include 顺序刻意是「先 pyui，后 windows」：
// windows.h 会定义 min/max 宏、并引入大量全局符号，先让 pyui 的头文件
// 看到干净的环境，能避免一大类只在 Windows 后端出现的编译错误。
#include "pyui/painter.h"
#include "pyui/types.h"

// 整个后端只走 Wide 版 API：核心层是 UTF-8，边界一次性转 UTF-16。
// 不定义 UNICODE 的话 IDC_ARROW / IDI_APPLICATION 会解析成 ...A 版本
// （LPSTR），和 LoadCursorW 的 LPCWSTR 对不上——这正是编译器报的那串错。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// GET_X_LPARAM / GET_Y_LPARAM 在 windowsx.h 里，windows.h 不带它。
// 顺手放进公共头：几乎所有后端 TU 都要拆 lParam。
#include <windowsx.h>

#include <string>
#include <string_view>

namespace pyui {
namespace win32 {

// UTF-8 ↔ UTF-16。整条字符串边界只在平台层转换，核心层一律 UTF-8。
std::wstring toWide(std::string_view text);
std::string toUtf8(std::wstring_view text);

// 把 GetLastError 的当前值转成可读原因（UTF-8）。
// 用法：调用失败后立刻取，别在中间再调别的 Win32 API 把错误码冲掉。
std::string lastError(const char* what);

// 注册一个进程内唯一的窗口类，已注册时直接返回。
// 返回 0 表示失败。
//
// style 由调用方给：自绘标题栏的窗口必须带 CS_DBLCLKS（双击交给系统判定），
// 而纯绘制窗口要显式关掉 CS_HREDRAW/CS_VREDRAW，否则系统会整块重画，
// 「只失效一行」就失去意义（拖动时闪）。两者不能由这里一刀切。
ATOM registerWindowClass(const wchar_t* className, WNDPROC proc, HICON icon, HCURSOR cursor,
                         UINT style = 0);

// 取本模块（exe/dll）的 HINSTANCE
HINSTANCE moduleHandle();

// 屏幕坐标 → 客户区坐标（考虑客户区在屏幕上的偏移）
Point screenToClientPoint(HWND hwnd, POINT pt);

// DPI：系统 DPI（96 = 100%）。用 GetDpiForWindow 当它可用，
// 老系统退回 GetDeviceCaps——Win10 1607 之前没有 per-monitor DPI。
int dpiForWindow(HWND hwnd);

// UTF-8 路径 → 宽字符路径（文件系统 API 一律 Wide 版，否则非 ANSI 字符会丢）
std::wstring toWidePath(const std::string& utf8);

// 系统是否处于深色模式（「应用」配色）。
//
// Windows 没有公开 API，只能读 HKCU 的 Personalize\AppsUseLightTheme
// （0 = 深色）。Win10 1809 之前没有这个键，读不到时按浅色——宁可误判成
// 亮色，也不要让旧系统上的窗口变成「黑底黑字」。
bool isSystemDarkMode();

// exe 自带的主图标（ALT+Tab / 任务栏 / 标题栏左上角那一枚）。
//
// 取的是「按像素尺寸最合适的那一档」：LoadIcon 只给 32×32，150% 屏上会被
// 放大成糊的方块。这里走 PrivateExtractIconsW 问 exe 要精确尺寸，没有内嵌
// 图标时退回 SHGetFileInfoW 的 shell 关联图标。
//
// 返回值由调用方 DestroyIcon。拿不到返回 nullptr。
HICON loadApplicationIcon(int pixelSize);

// HICON → 后端无关的 Image。
//
// 结果里的 handle 是一支 new 出来的 Surface*（像素格式 0xAARRGGBB），所有权
// 归调用方：不销毁，与 associationIcon 的约定一致（宿主在退出前统一释放）。
//
// 为什么不让控件直接拿 HICON：HICON 只有 GDI 能解释，而 ImageView 走的是
// SoftwareBackend 的位图路径——离屏渲染与像素测试都要能读像素。
Image iconToImage(HICON icon, int pixelSize);

}  // namespace win32
}  // namespace pyui
