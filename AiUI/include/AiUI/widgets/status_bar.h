// StatusBar：左侧路径 + 可选格式标签 + 大小，右侧统计
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <string>
#include <vector>

namespace AiUI {

struct StatusFields {
  std::wstring left;   // 路径或包路径
  std::wstring tag;    // 格式标签，空则不画
  std::wstring size;   // 大小
  // 追加的胶囊标签（分辨率、文件大小、序号之类）。渲染成与 tag 同款的
  // 圆角底，排在 size 之后；空串跳过，方便调用方按条件塞。
  std::vector<std::wstring> badges;
  std::wstring right;  // 右侧统计
  bool showHome = false;
};

struct StatusStyle {
  int padX = 16;
  int homeIconSize = 15;
  int tagPadX = 6;
  int tagHeight = 19;
  int tagRadius = 4;
  // 颜色：kNoColor 表示跟随主题
  Color background = kNoColor;
  Color text = kNoColor;
  Color divider = kNoColor;
  Color tagBg = kNoColor;
  Color tagInk = kNoColor;
};

class StatusBar : public View {
 public:
  StatusBar() = default;

  void setFields(const StatusFields& fields);
  void setStyle(const StatusStyle& style);
  const StatusFields& fields() const { return fields_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  Color backgroundColor() const override;

 private:
  StatusFields fields_{};
  StatusStyle style_{};
};

}  // namespace AiUI
