// 自绘子窗口基类：统一窗口类注册、消息分发与 DPI 换算
#pragma once

#include <windows.h>

namespace AUI {

// 派生类只实现钩子；基类负责注册窗口类并把消息转发到成员函数。
// 实例由调用方持有，窗口销毁时基类只解除绑定。
class Widget {
 public:
  virtual ~Widget() = default;

  HWND hwnd() const { return hwnd_; }
  int dpi() const { return dpi_; }
  // 96 DPI 设计值转物理像素
  int px(int v) const { return ::MulDiv(v, dpi_, 96); }

  // 创建子窗口；类名需在进程内唯一
  HWND Create(HWND parent, HINSTANCE inst, int id, const wchar_t* className,
              DWORD style = WS_CHILD | WS_VISIBLE, DWORD exStyle = 0);
  void Invalidate(bool erase = false) {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, erase ? TRUE : FALSE);
  }

  RECT ClientRect() const {
    RECT rc{};
    if (hwnd_) ::GetClientRect(hwnd_, &rc);
    return rc;
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

 protected:
  // 子窗口创建完成后回调，派生类在这里建自己的子控件
  virtual void OnCreated() {}
  // 未识别的消息交回派生类，默认走 DefWindowProc
  virtual LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
  }
  virtual void OnSize(int cx, int cy) { (void)cx; (void)cy; }
  virtual void OnPaint() {}
  // 返回 true 表示已擦除，基类直接返回 1
  virtual bool OnEraseBkgnd(HDC dc) {
    (void)dc;
    return false;
  }

  HWND hwnd_ = nullptr;
  int dpi_ = 96;
};

// 注册一次窗口类；重复注册时错误码为 ERROR_CLASS_ALREADY_EXISTS，视为成功
bool RegisterWidgetClass(const wchar_t* name, HINSTANCE inst, UINT style = CS_HREDRAW | CS_VREDRAW);

}  // namespace AUI
