// ThumbnailStrip：竖排缩略图条，浏览同目录时的导航用
//
// 与 ListView 的区别：这里每项的主体是图，不是文本表格，行高固定、无表头、
// 无列概念。缩略图按需解码并缓存——一个目录几千张图，打开时全部解码既慢
// 又占内存，实际只有看得见的那几项会被画到。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/scroll_view.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AiUI {

struct ThumbnailStyle {
  // 96 DPI 设计值
  int padX = 10;
  int padY = 8;
  int thumbSide = 84;   // 缩略图方框短边
  int labelGap = 4;
  int labelHeight = 18;
  int radius = 6;
  int itemGap = 6;
  int thumbMaxSide = 256;  // 解码上限，按屏幕密度再放大
  // kNoColor 表示跟随主题
  Color background = kNoColor;
  Color itemBg = kNoColor;
  Color itemHoverBg = kNoColor;
  Color selBg = kNoColor;
  Color selBorder = kNoColor;
  Color text = kNoColor;
};

class ThumbnailStrip : public ScrollView {
 public:
  ThumbnailStrip();
  ~ThumbnailStrip() override;

  // 路径列表整体替换；会释放旧的缩略图缓存
  void setItems(std::vector<std::wstring> paths);
  const std::vector<std::wstring>& items() const { return items_; }
  int itemCount() const { return static_cast<int>(items_.size()); }

  // 当前项；-1 表示无选中
  void setSelected(int index);
  int selected() const { return selected_; }
  // 选中项滚进视口；已经可见时不动
  void scrollToItem(int index);

  void setStyle(const ThumbnailStyle& style);
  const ThumbnailStyle& style() const { return style_; }

  // 点击某一项；参数是索引
  using SelectHandler = std::function<void(int)>;
  void setOnSelect(SelectHandler fn) { onSelect_ = std::move(fn); }

  int contentHeight() const override;
  int viewportHeight() const override;
  int itemHeight() const;
  Rect itemRect(int index) const;

 protected:
  Size onMeasure(const Size& available) override;
  void onPaint(Painter& painter, const Rect& rect) override;
  void onScrollChanged() override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onDetach() override;
  Color backgroundColor() const override;

 private:
  int hitItem(Point local) const;
  // 取（必要时解码）某一项的缩略图；失败返回无效 Image
  const Image& thumbnailFor(int index);
  void releaseThumbnails();

  std::vector<std::wstring> items_;
  std::unordered_map<std::wstring, Image> thumbs_;
  int selected_ = -1;
  int hot_ = -1;
  ThumbnailStyle style_{};
  SelectHandler onSelect_;
};

}  // namespace AiUI
