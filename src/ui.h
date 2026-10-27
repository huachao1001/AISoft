// 界面公共绘制工具与常量
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>

namespace ui {

// 工具栏：顶部近白、向下极淡地收进蓝灰，避免大片死白显得廉价
constexpr COLORREF kToolbarTop = RGB(253, 254, 255);
constexpr COLORREF kToolbarBottom = RGB(235, 242, 249);
constexpr COLORREF kToolbarLine = RGB(203, 216, 228);
constexpr COLORREF kWindowBg = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(196, 214, 226);
constexpr COLORREF kToolbarText = RGB(58, 70, 82);
// 对话框底色：控件（标签/分组框/单选/复选）也铺这个色，避免白块
constexpr COLORREF kDialogBg = RGB(240, 247, 252);

// 布局尺寸（96 DPI 基准）
constexpr int kToolbarHeight = 76;
constexpr int kAddressHeight = 28;
constexpr int kStatusHeight = 24;
constexpr int kSplitterWidth = 5;
constexpr int kTreeDefaultWidth = 210;

// 注意：不能把成员命名为 small —— rpcndr.h 把 small 定义成了宏
struct Fonts {
  HFONT ui = nullptr;
  HFONT uiBold = nullptr;

  void Create(int dpi);
  void Destroy();
};

extern Fonts fonts;

// 双缓冲绘制辅助
class MemDC {
 public:
  MemDC(HDC target, const RECT& rc);
  ~MemDC();
  HDC dc() const { return mem_; }

  void Commit();

 private:
  HDC target_;
  HDC mem_ = nullptr;
  HBITMAP bmp_ = nullptr;
  HBITMAP oldBmp_ = nullptr;
  RECT rc_{};
};

void FillRect(HDC dc, const RECT& rc, COLORREF color);
void FillRoundRect(HDC dc, const RECT& rc, COLORREF color, int radius);
void StrokeRoundRect(HDC dc, const RECT& rc, COLORREF color, int radius, int width);
// 垂直渐变；圆角版本内部用区域裁剪，边界保持圆角
void FillGradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom);
void FillGradientRoundRect(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom, int radius);
void DrawHLine(HDC dc, int left, int right, int y, COLORREF color);
void DrawTextLeft(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);
void DrawTextCenter(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);

// 工具栏按钮图标（GDI 自绘，无外部图片）
enum class IconId {
  kAdd,
  kExtract,
  kDelete,
  kFolder,
  kArrowBack,
  kArrowForward,
  kArrowUp,
  kRefresh,
  kWinMin,
  kWinMax,
  kWinRestore,
  kWinClose,
};

// inkOverride 非 CLR_INVALID 时用它替换图标主色（关闭键悬停转白用）
void DrawIcon(HDC dc, IconId id, const RECT& rc, bool enabled = true,
              COLORREF inkOverride = CLR_INVALID);

// 可缩放边框厚度（96 DPI 基准），无边框窗口判断缩放热区用
int ResizeBorder(int dpi);

// ── 窗口/对话框公共设施 ──
// 注册窗口类；同类已注册视为成功。iconId 非 0 时同时挂大/小图标。
bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId = 0);

// 按 96 DPI 基准坐标创建子控件
HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle = 0);

// 铺对话框整块底色（吞掉 WM_ERASEBKGND 后必须自己填）
void PaintDialogBackground(HWND hwnd);

// WM_CTLCOLORBTN / WM_CTLCOLORSTATIC 公用：标签、分组框、单选、复选铺对话框底色。
// 返回 nullptr 表示交给系统默认绘制（普通按钮）
HBRUSH DialogCtlBrush(UINT msg, HWND ctl);

// 以 owner 为参照居中摆放 w x h 的对话框；owner 为空时交给系统定位
RECT DialogRect(HWND owner, int w, int h);

// 模态运行对话框直到它被销毁，期间禁用 owner。
// onBeforeEnable 在恢复 owner 之前调用（进度框在这里 join 工作线程，避免重入）。
void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable = {});

// shell 文件图标（系统图像列表 + 索引缓存）
int ShellIconIndex(const std::wstring& path, bool isDir);
HIMAGELIST ShellImageList();
void ClearIconCache();

}  // namespace ui
