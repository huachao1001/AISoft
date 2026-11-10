// 状态栏：左侧位置 + 可选格式标签 + 大小，右侧统计
#pragma once

#include <windows.h>

#include <string>

#include "AUI/theme.h"
#include "AUI/widget.h"

namespace AUI {

struct StatusFields {
  std::wstring left;   // 路径或包路径
  std::wstring tag;    // 格式标签，空则不画
  std::wstring size;   // 大小
  std::wstring right;  // 右侧统计
  bool showHome = false;
};

struct StatusStyle {
  int padX = 16;
  int homeIconSize = 15;
  int tagPadX = 6;
  int tagH = 19;
  int tagRadius = 4;
  COLORREF bg = kToolBg;
  COLORREF text = kTextMuted;
  COLORREF divider = kLine;
};

class StatusBar : public Widget {
 public:
  void SetFields(const StatusFields& f);
  void SetStyle(const StatusStyle& style);
  const StatusFields& fields() const { return fields_; }

 protected:
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;
  LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

 private:
  StatusFields fields_{};
  StatusStyle style_{};
};

}  // namespace AUI
