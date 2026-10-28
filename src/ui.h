// 界面公共绘制工具与常量
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>

namespace ui {

constexpr COLORREF kDialogBg = RGB(240, 247, 252);

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

// ── 窗口/对话框公共设施 ──
// 注册窗口类；同类已注册视为成功。iconId 非 0 时同时挂大/小图标。
bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId = 0);

// 按 96 DPI 基准坐标创建子控件
HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle = 0);

// 铺对话框整块底色（吞掉 WM_ERASEBKGND 后必须自己填）
void PaintDialogBackground(HWND hwnd);

// WM_CTLCOLORBTN / WM_CTLCOLORSTATIC 公用：标签、分组框、单选/复选铺对话框底色。
// 返回 nullptr 表示交给系统默认绘制（普通按钮）
HBRUSH DialogCtlBrush(UINT msg, HWND ctl);

// 以 owner 为参照居中摆放 w x h 的对话框；owner 为空时居中到工作区
RECT DialogRect(HWND owner, int w, int h);

// 模态运行对话框直到它被销毁，期间禁用 owner。
// onBeforeEnable 在恢复 owner 之前调用（进度框在这里 join 工作线程，避免重入）。
void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable = {});

}  // namespace ui
