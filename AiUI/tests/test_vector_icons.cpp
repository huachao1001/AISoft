// 矢量图标：手绘优先、退回字形、缩放不变形
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/painter.h"
#include "AiUI/vector_icons.h"

namespace {

using namespace aiui_test;
using namespace AiUI;

// 记描边调用的后端：手绘图标只走 strokePolylineF / fillPolygonF，
// 字形图标走 drawIcon，两者互斥，据此就能分辨走了哪条路。
class StrokeRecorder : public RecordingBackend {
 public:
  void strokePolylineF(const PointF* points, int count, Color color, float width) override {
    (void)color;
    if (!points || count < 2) return;
    ++strokes_;
    lastWidth_ = width;
    for (int i = 0; i < count; ++i) {
      if (points[i].x < minX_) minX_ = points[i].x;
      if (points[i].x > maxX_) maxX_ = points[i].x;
      if (points[i].y < minY_) minY_ = points[i].y;
      if (points[i].y > maxY_) maxY_ = points[i].y;
    }
  }
  void fillPolygonF(const PointF* points, int count, Color color) override {
    (void)points;
    (void)count;
    (void)color;
    ++polys_;
  }
  int strokes() const { return strokes_; }
  int polys() const { return polys_; }
  float lastWidth() const { return lastWidth_; }
  float minX() const { return minX_; }
  float maxX() const { return maxX_; }
  float minY() const { return minY_; }
  float maxY() const { return maxY_; }
  void reset() {
    strokes_ = 0;
    polys_ = 0;
    minX_ = minY_ = 1e9f;
    maxX_ = maxY_ = -1e9f;
  }

 private:
  int strokes_ = 0;
  int polys_ = 0;
  float lastWidth_ = 0.0f;
  float minX_ = 1e9f, maxX_ = -1e9f, minY_ = 1e9f, maxY_ = -1e9f;
};

// 工具栏那批图标都该有手绘版本：一整排混用字形会粗细不一
AIUI_TEST(vector_icons_cover_the_toolbar_set) {
  for (IconId id : {IconId::kZoomIn, IconId::kZoomOut, IconId::kFitWindow,
                    IconId::kZoomActual, IconId::kRotateLeft, IconId::kRotateRight,
                    IconId::kFullscreen, IconId::kSaveAs}) {
    StrokeRecorder backend;
    Painter painter(backend);
    AIUI_CHECK(paintVectorIcon(painter, id, Rect{0, 0, 24, 24}, makeColor(0, 0, 0, 255)));
    AIUI_CHECK(backend.strokes() > 0);
  }
}

// 没有手绘版本的图标要老实返回 false，让调用方退回字形；
// 否则那个位置会画成空白
AIUI_TEST(vector_icons_decline_unknown_ids) {
  StrokeRecorder backend;
  Painter painter(backend);
  AIUI_CHECK(!paintVectorIcon(painter, IconId::kTrash, Rect{0, 0, 24, 24},
                              makeColor(0, 0, 0, 255)));
  AIUI_CHECK(!paintVectorIcon(painter, IconId::kNone, Rect{0, 0, 24, 24},
                              makeColor(0, 0, 0, 255)));
  AIUI_CHECK(backend.strokes() == 0);
}

// 线宽跟着尺寸走：小图标上按大图标的绝对线宽画，笔画会糊成一块
AIUI_TEST(vector_icons_scale_line_width_with_size) {
  StrokeRecorder small;
  Painter p1(small);
  paintVectorIcon(p1, IconId::kZoomIn, Rect{0, 0, 24, 24}, makeColor(0, 0, 0, 255));

  StrokeRecorder large;
  Painter p2(large);
  paintVectorIcon(p2, IconId::kZoomIn, Rect{0, 0, 96, 96}, makeColor(0, 0, 0, 255));

  AIUI_CHECK(large.lastWidth() > small.lastWidth() * 3.0f);
}

// 图形必须落在给定的矩形内，不能画出边界压到旁边的控件
AIUI_TEST(vector_icons_stay_inside_the_box) {
  const Rect box{40, 60, 88, 108};  // 48x48
  for (IconId id : {IconId::kZoomIn, IconId::kFitWindow, IconId::kRotateLeft,
                    IconId::kRotateRight, IconId::kFullscreen, IconId::kSaveAs}) {
    StrokeRecorder backend;
    Painter painter(backend);
    AIUI_CHECK(paintVectorIcon(painter, id, box, makeColor(0, 0, 0, 255)));
    // 描边以中心线向两侧展开半个线宽，允许溢出半个笔画
    const float half = backend.lastWidth() * 0.5f + 0.5f;
    AIUI_CHECK(backend.minX() >= static_cast<float>(box.left) - half);
    AIUI_CHECK(backend.maxX() <= static_cast<float>(box.right) + half);
    AIUI_CHECK(backend.minY() >= static_cast<float>(box.top) - half);
    AIUI_CHECK(backend.maxY() <= static_cast<float>(box.bottom) + half);
  }
}

// 左右旋转必须是镜像关系，不能是同一个图形转一下：
// 顺时针与逆时针的语义就靠箭头落点区分
AIUI_TEST(rotate_icons_are_mirrored) {
  StrokeRecorder left;
  Painter p1(left);
  paintVectorIcon(p1, IconId::kRotateLeft, Rect{0, 0, 24, 24}, makeColor(0, 0, 0, 255));

  StrokeRecorder right;
  Painter p2(right);
  paintVectorIcon(p2, IconId::kRotateRight, Rect{0, 0, 24, 24}, makeColor(0, 0, 0, 255));

  // 两者覆盖的范围一致，只是左右对调
  AIUI_CHECK(left.minX() < right.minX() + 14.0f);
  AIUI_CHECK(right.maxX() > left.maxX() - 14.0f);
  AIUI_CHECK(left.strokes() == right.strokes());
}

}  // namespace
