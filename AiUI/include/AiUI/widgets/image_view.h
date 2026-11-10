// ImageView：显示位图或矢量图
//
// 位图（png/jpeg/gif/bmp/ico/webp）由平台解码器加载，可任意缩放；
// 加载失败时退回占位图标。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <string>

namespace AiUI {

class ImageView : public View {
 public:
  ImageView() = default;

  // 从文件加载：按扩展名选解码器，支持 png/jpeg/gif/bmp/ico/webp
  bool loadFile(const std::wstring& path);
  // 直接给一个已解码的图片句柄
  void setImage(const Image& image);
  // 用内建矢量图标
  void setIcon(IconId icon);
  void clearImage();

  bool hasImage() const { return image_.isValid() || icon_ != IconId::kNone; }
  void setScale(ImageScale scale);
  ImageScale scale() const { return scale_; }

  // 没有图片时显示的颜色
  void setPlaceholderColor(Color color);

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onAttach() override;

 private:
  // 按适配方式算出图片实际占用的矩形
  Rect targetRect(const Rect& inner) const;

  Image image_{};
  IconId icon_ = IconId::kNone;
  ImageScale scale_ = ImageScale::kFit;
  Color placeholderColor_ = kColorTextFaint;
  Size sourceSize_{};
};

}  // namespace AiUI
