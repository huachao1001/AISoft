// 字体集：按 DPI 创建一组界面字体，按值传递
#pragma once

#include "AiUI/painter.h"

#include <functional>
#include <map>

namespace AiUI {

class FontSet {
 public:
  Font ui;       // 正文：列表、按钮
  Font uiBold;   // 加粗：标题
  Font uiSmall;  // 小字：状态栏、页签
  // 大字：进度条这类需要一眼看清的场合
  Font uiLarge;
  Font uiLargeBold;
  // 超大：单行信息窗口（如进度条）的主标题
  Font uiHuge;
  Font uiHugeBold;

  void create(int dpi);
  void destroy();
  bool isValid() const { return ui.isValid(); }

  // 按像素字号取字体（px 是 em 尺寸，不是行高）。
  // 只给尺寸随内容变的场合用——像素格就是这么个情况：格子会从 20 像素一路
  // 放大到几百，用固定档位要么装不进去、要么小得看不见。
  // 返回的字体由 FontSet 持有，不要自己释放；create() 会一并清掉缓存。
  const Font& forPixelSize(int px);

 private:
  int dpi_ = 0;
  // 已建过的字号：几像素一档，逐档缓存，避免每帧重复创建字体
  std::map<int, Font> bySize_;
};

// 全局字体集，由宿主启动时 create、退出前 destroy
extern FontSet fonts;

// 释放全部字体；宿主退出前调用一次
void releaseFonts();

}  // namespace AiUI
