// 绘制封装：Painter 包一层 HDC，Canvas 负责离屏双缓冲
#pragma once

#include <windows.h>

#include <string>

#include "AUI/icon.h"

namespace AUI {

// HDC 的轻量包装：所有绘制都从这里走，便于统一处理选字体/颜色/背景模式
class Painter {
 public:
  explicit Painter(HDC dc) : dc_(dc) {}
  HDC dc() const { return dc_; }

  void Fill(const RECT& rc, COLORREF color) const;
  void FillRound(const RECT& rc, COLORREF color, int radius) const;
  void StrokeRound(const RECT& rc, COLORREF color, int radius, int width = 1) const;
  void GradientV(const RECT& rc, COLORREF top, COLORREF bottom) const;
  void GradientRound(const RECT& rc, COLORREF top, COLORREF bottom, int radius) const;
  void HLine(int left, int right, int y, COLORREF color) const;
  void VLine(int top, int bottom, int x, COLORREF color) const;

  void TextLeft(const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) const;
  void TextCenter(const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) const;
  void TextRight(const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) const;
  void TextEllipsis(const RECT& rc, const std::wstring& text, COLORREF color, HFONT font) const;

  int TextWidth(const std::wstring& text, HFONT font) const;
  int TextHeight(HFONT font) const;

  void Icon(IconId id, const RECT& rc, COLORREF color, bool enabled = true) const;
  // 位图图标按矩形缩放绘制
  void BitmapIcon(HICON icon, const RECT& rc) const;

 private:
  HDC dc_;
};

// 离屏画布：先画到位图再一次性贴回，避免闪烁与半帧
class Canvas {
 public:
  Canvas(HDC target, const RECT& rc);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  Painter& painter() { return painter_; }
  HDC dc() const { return mem_; }
  void Commit();

 private:
  HDC target_ = nullptr;
  HDC mem_ = nullptr;
  HBITMAP bmp_ = nullptr;
  HBITMAP oldBmp_ = nullptr;
  RECT rc_{};
  Painter painter_;
};

}  // namespace AUI
