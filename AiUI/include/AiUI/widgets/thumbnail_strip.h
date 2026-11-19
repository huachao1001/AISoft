// ThumbnailStrip：竖排缩略图条，浏览同目录时的导航用
//
// 与 ListView 的区别：这里每项是一张「卡片」（圆角白底 + 阴影 + 上方图片
// + 下方文件名），不是表格行。缩略图按需解码并缓存——一个目录几千张图，
// 打开时全部解码既慢又占内存，实际只有看得见的那几项会被画到。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/scroll_view.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AiUI {

// 卡片外观（96 DPI 设计值）
struct ThumbnailStyle {
  // 卡片left/right外边距，也是内容区的上下留白
  int sideMargin = 10;
  // 卡片内部留白
  int cardPadX = 8;
  int cardPadY = 8;
  // 图区高度；宽度由卡片宽度决定，图片按长边等比塞进去
  int thumbH = 92;
  int labelGap = 6;
  int labelHeight = 18;
  int cardRadius = 8;
  // 卡片之间的竖直间隙；必须大于阴影两侧扩散之和，否则相邻卡片的阴影会连成一片
  int itemGap = 22;
  // 阴影：向外扩散多少像素、向下偏移多少、最深一层的 alpha
  int shadowSpread = 8;
  int shadowOffsetY = 3;
  int shadowAlpha = 96;
  int thumbMaxSide = 256;  // 解码上限，按屏幕密度再放大
  // kNoColor 表示跟随主题
  Color background = kNoColor;
  Color cardBg = kNoColor;
  Color cardHoverBg = kNoColor;
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
  // 卡片样式。不叫 style()：ScrollView 已经用这个名字表示滚动条样式
  const ThumbnailStyle& cardStyle() const { return cardStyle_; }

  // 点击某一项；参数是索引
  using SelectHandler = std::function<void(int)>;
  void setOnSelect(SelectHandler fn) { onSelect_ = std::move(fn); }

  int contentHeight() const override;
  int viewportHeight() const override;
  int itemHeight() const;
  // 卡片矩形（不含阴影）；滚动后已经在视口坐标里
  Rect itemRect(int index) const;

 protected:
  Size onMeasure(const Size& available) override;
  void onPaint(Painter& painter, const Rect& rect) override;
  void onScrollChanged() override;
  // 布局完成后补一次待处理的定位：首次 setSelected 时视口还是 0
  void onLayout(const Rect& frame) override;
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
  // 画一张卡片的柔和阴影：多层半透明圆角矩形叠出来，Painter 没有模糊接口
  void paintShadow(Painter& painter, const Rect& card, int radius) const;

  std::vector<std::wstring> items_;
  std::unordered_map<std::wstring, Image> thumbs_;
  int selected_ = -1;
  int hot_ = -1;
  // 布局前调用 scrollToItem 时先记下目标，等拿到真实视口高度再滚
  int pendingScrollTo_ = -1;
  ThumbnailStyle cardStyle_{};
  SelectHandler onSelect_;
};

}  // namespace AiUI
