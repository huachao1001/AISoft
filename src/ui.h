// 界面公共绘制工具与常量
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>

#include "ui_theme.h"

namespace ui {

// 字体
struct Fonts {
  HFONT ui = nullptr;       // 10pt 常规：列表正文、按钮
  HFONT uiBold = nullptr;   // 10pt 半粗
  HFONT uiSmall = nullptr;  // 9pt 常规：状态栏、页签

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
void FillGradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom);
void FillGradientRoundRect(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom, int radius);
void DrawHLine(HDC dc, int left, int right, int y, COLORREF color);
void DrawVLine(HDC dc, int top, int bottom, int x, COLORREF color);
void DrawTextLeft(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);
void DrawTextCenter(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color, HFONT font);
void DrawTextEllipsis(HDC dc, const RECT& rc, const std::wstring& text, COLORREF color,
                      HFONT font);
// 文本宽度，用于面包屑/标签排版
int TextWidth(HDC dc, const std::wstring& text, HFONT font);
// 字体行高，用于把单行 EDIT 垂直居中
int TextHeight(HDC dc, HFONT font);

// 系统图标：取自 Segoe Fluent Icons（Win11），缺失时回退 Segoe MDL2 Assets
enum class IconId {
  kRefresh,
  kExtract,     // 提取选中
  kExtractAll,  // 全部解压
  kAdd,         // 添加文件
  kTrash,       // 删除
  kArrowBack,
  kChevron,     // 面包屑分隔
  kChevronDown, // 展开 / 降序
  kChevronUp,   // 折叠 / 升序
  kHome,
  kFolder,
  kFile,
  kSearch,
};

void DrawIcon(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled = true);

// ── 窗口/对话框公共设施 ──
bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId = 0);

HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle = 0);

void PaintDialogBackground(HWND hwnd);
HBRUSH DialogCtlBrush(UINT msg, HWND ctl);
RECT DialogRect(HWND owner, int w, int h);
void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable = {});

// shell 文件图标：句柄缓存在本进程内，调用方不要销毁
HICON ShellIconFor(const std::wstring& path, bool isDir);
// 行高 = 单元格边长的空图像列表，只用于把列表行撑高；行首图标自绘
HIMAGELIST ListImageList(int rowHeight);
void ClearIconCache();

}  // namespace ui
