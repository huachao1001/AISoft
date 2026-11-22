// ImageView：显示位图或矢量图标
//
// 与 AiUI 的 ImageView 对应，只保留 PyUI 用得到的那部分：给一张已经加载好的
// 位图、或者一个内置矢量图标，按 ImageScale 摆进自己的矩形。**不负责加载
// 文件**——路径、格式、解码全在平台层，核心层拿到的永远是 Image 句柄。
// 这条边界让三个平台的 ImageView 绘制代码完全一致（铁律五）。
//
// 为什么单独一个控件而不是让 TitleBar 直接 drawImage：标题栏的应用图标在
// XML 里是一个节点（id 0x7A02），宿主可以 findView 换掉它、改可见性、改大小。
// 画在父控件的 onPaint 里就做不到这些——那是「绘制」而不是「视图」。
#pragma once

#include "pyui/painter.h"
#include "pyui/view.h"

namespace pyui {

class ImageView : public View {
 public:
  ImageView() = default;

  // 直接给一个已加载好的位图。传无效 Image 等价于 clearImage()。
  void setImage(const Image& image);
  const Image& image() const { return image_; }

  // 内置矢量图标；设图标会清掉位图，反之亦然——同一个控件只显示一个东西。
  void setIcon(IconId icon);
  IconId icon() const { return icon_; }

  void clearImage();
  bool hasImage() const { return image_.isValid() || icon_ != IconId::kNone; }

  // 位图的摆放方式；矢量图标总是等比居中（图标本身没有原始尺寸的概念）
  void setScale(ImageScale scale);
  ImageScale scale() const { return scale_; }

  // 矢量图标的颜色；kNoColor 表示跟随主题正文色
  void setIconColor(Color color);
  Color iconColor() const { return iconColor_; }

  // 位图的原始尺寸需要后端回答。核心层不认得 Bitmap 结构，所以由调用方
  // 在拿到 Image 之后告诉它一次；0 表示未知，此时按 kStretch 处理。
  void setSourceSize(Size size);
  Size sourceSize() const { return sourceSize_; }

  const char* className() const override { return "ImageView"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

 private:
  // 按 scale_ 算出位图实际占用的矩形
  Rect targetRect(const Rect& inner) const;
  Color resolveIconColor() const;

  Image image_{};
  IconId icon_ = IconId::kNone;
  ImageScale scale_ = ImageScale::kFit;
  Color iconColor_ = kNoColor;
  Size sourceSize_{};
};

}  // namespace pyui
