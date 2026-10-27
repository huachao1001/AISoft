// 界面公共绘制工具与常量
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>

namespace ui {

// 工具栏：顶部近白、向下极淡地收进蓝灰，避免大片死白显得廉价
constexpr COLORREF kToolbarTop = RGB(253, 254, 255);
constexpr COLORREF kToolbarBottom = RGB(235, 242, 249);
constexpr COLORREF kToolbarLine = RGB(203, 216, 228);
constexpr COLORREF kWindowBg = RGB(255, 255, 255);
constexpr COLORREF kPanelBg = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(196, 214, 226);
constexpr COLORREF kText = RGB(32, 32, 32);
constexpr COLORREF kTextDim = RGB(120, 128, 136);
constexpr COLORREF kSelection = RGB(204, 232, 255);
constexpr COLORREF kSelectionInactive = RGB(229, 234, 238);
constexpr COLORREF kHeaderTop = RGB(250, 252, 254);
constexpr COLORREF kHeaderBottom = RGB(238, 245, 250);
constexpr COLORREF kAccent = RGB(0, 120, 215);
constexpr COLORREF kToolbarText = RGB(58, 70, 82);

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
  HFONT uiSmall = nullptr;
  HFONT icon = nullptr;

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
void DrawVLine(HDC dc, int x, int top, int bottom, COLORREF color);
void DrawHLine(HDC dc, int left, int right, int y, COLORREF color);
void DrawTextLeft(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);
void DrawTextCenter(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);
void DrawTextEllipsis(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);

// 工具栏按钮图标（GDI 自绘，无外部图片）
enum class IconId {
  kAdd,
  kExtract,
  kDelete,
  kFolder,
  kFile,
  kArchive,
  kArrowBack,
  kArrowForward,
  kArrowUp,
  kRefresh,
  kZip,
  kWinMin,
  kWinMax,
  kWinRestore,
  kWinClose,
};

// inkOverride 非 CLR_INVALID 时用它替换图标主色（关闭键悬停转白用）
void DrawIcon(HDC dc, IconId id, const RECT& rc, bool enabled = true,
              COLORREF inkOverride = CLR_INVALID);

// 系统图标缓存（shell32）
int ShellIconIndex(const std::wstring& path, bool isDir);
HIMAGELIST ShellImageList();
void ClearIconCache();

// 可缩放边框厚度（96 DPI 基准），无边框窗口判断缩放热区用
int ResizeBorder(int dpi);

}  // namespace ui
