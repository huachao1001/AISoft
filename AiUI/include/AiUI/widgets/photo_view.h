// PhotoView：可缩放、平移、旋转的图片查看控件
//
// 与 ImageView 的分工：ImageView 只做「等比塞进目标矩形」的静态展示；
// 看图中用户要能放大到像素级、拖动看局部、转个方向，那些交互都收在这里。
//
// 变换模型：图像坐标 → 视图坐标只有三步——
//   scale（每图像像素多少视图像素）+ 旋转 + 平移。
// 旋转角度固定是 90° 的整数倍：任意角度需要重采样，看图的清晰度会受损，
// 而用户要的「转一下」几乎都是竖直/横置。
#pragma once

#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/view.h"

#include <functional>
#include <string>

namespace AiUI {

class PhotoView : public View {
 public:
  PhotoView();

  // 从文件加载；失败返回 false 并清空当前图
  bool loadFile(const std::wstring& path);
  // 直接给一张已解码的位图（旋转后重建时用）
  void setImage(const Image& image);
  void clearImage();

  bool hasImage() const { return image_.isValid(); }
  // 图像原始像素尺寸
  Size imageSize() const { return sourceSize_; }
  const std::wstring& path() const { return path_; }

  // 缩放倍数：1.0 = 一个图像像素对应一个视图像素（物理像素）
  float zoom() const { return zoom_; }
  void setZoom(float zoom);
  // 适应窗口：整张图放得下，不放大
  void zoomToFit();
  // 缩放到 100%（物理像素 1:1）
  void zoomToActual();
  // 在当前倍数上乘以 factor，围绕视图中心
  void zoomBy(float factor);
  // 是否处于「适应窗口」状态：此状态下视图尺寸变化时自动重算倍数。
  // 用户一旦自己缩放就退出该状态——否则拖大窗口会把他刚定的倍数作废。
  bool isFitToWindow() const { return fitToWindow_; }

  // 旋转：每调用一次转 90°，顺时针为正
  void rotate(int quarterTurns);
  int rotation() const { return rotation_; }

  // 平移量（视图像素）；0 表示图像中心对齐视图中心
  Point pan() const { return pan_; }
  void setPan(Point pan);

  // 双击与滚轮的行为在宿主里决定，这里只报事件
  using ZoomHandler = std::function<void(float factor)>;
  void setWheelZoomHandler(ZoomHandler fn) { onWheelZoom_ = std::move(fn); }
  // 平移或缩放变化后通知宿主刷新状态栏
  using ChangeHandler = std::function<void()>;
  void setChangeHandler(ChangeHandler fn) { onChange_ = std::move(fn); }

  // 视图中心处的图像像素坐标：状态栏与取色都要用
  Point centerPixel() const;
  // 图像尺寸按当前旋转后的宽高
  Size rotatedSize() const;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  // 尺寸变化时若处于「适应窗口」，重算倍数
  void onLayout(const Rect& frame) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  bool onMouseWheel(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  void onAttach() override;
  Color backgroundColor() const override;

 private:
  // 把平移量夹到「图像边缘不进到视图内部」的范围
  void clampPan();
  // 视图状态变化后通知宿主刷新状态栏
  void notifyChanged();

  Image image_{};
  std::wstring path_;
  Size sourceSize_{};
  float zoom_ = 1.0f;
  // 处于「适应窗口」：视图尺寸变化时重算 zoom_，用户自己缩放则清掉
  bool fitToWindow_ = false;
  // 90° 的整数倍，取值 0..3；绘制时只做坐标换序，不重采样
  int rotation_ = 0;
  Point pan_{};
  bool dragging_ = false;
  Point dragOrigin_{};
  Point dragStartPan_{};

  ZoomHandler onWheelZoom_;
  ChangeHandler onChange_;
};

}  // namespace AiUI
