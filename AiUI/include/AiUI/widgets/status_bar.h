// StatusBar：左侧路径 + 可选格式标签 + 大小，右侧统计
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <string>

namespace AiUI {

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
  int tagHeight = 19;
  int tagRadius = 4;
  Color background = kColorTool;
  Color text = kColorTextMuted;
  Color divider = kColorLine;
};

class StatusBar : public Widget {
 public:
  StatusBar() { className_ = L"AiUIStatusBar"; }

  void setFields(const StatusFields& fields);
  void setStyle(const StatusStyle& style);
  const StatusFields& fields() const { return fields_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  Color backgroundColor() const override { return style_.background; }

 private:
  StatusFields fields_{};
  StatusStyle style_{};
};

}  // namespace AiUI
