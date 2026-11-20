// 手绘矢量图标实现
//
// 几何写在 24 单位的作图网格上（与主流图标字体的设计网格一致），按目标
// 矩形的最短边等比缩放并居中。线宽也随缩放走，小尺寸下不会糊成一片。
//
// 角度约定：屏幕坐标 y 向下，所以角度增大在视觉上是顺时针
// （0=右、π/2=下、π=左、-π/2=上）。
#include "AiUI/vector_icons.h"

#include "AiUI/painter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace AiUI {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kGrid = 24.0f;
// 线宽：参考图里 24 网格约 1.5 单位。再粗就糊成加粗体，
// 放大镜的圆、比例点这些小结构会先糊掉。
constexpr float kStroke = 1.5f;

// 作图网格：24 单位，原点在目标矩形里居中。
// mirror 时整体左右翻转：左右旋转这类镜像对只需要写一份几何。
struct Grid {
  float ox = 0.0f;
  float oy = 0.0f;
  float k = 1.0f;  // 一个网格单位对应多少设备像素
  bool mirror = false;

  float x(float u) const { return ox + (mirror ? (kGrid - u) : u) * k; }
  float y(float v) const { return oy + v * k; }
};

Grid makeGrid(const Rect& rc, bool mirror = false) {
  const float side = static_cast<float>((std::min)(rc.width(), rc.height()));
  Grid g;
  g.k = side / kGrid;
  g.mirror = mirror;
  g.ox = static_cast<float>(rc.left) + (static_cast<float>(rc.width()) - side) * 0.5f;
  g.oy = static_cast<float>(rc.top) + (static_cast<float>(rc.height()) - side) * 0.5f;
  return g;
}

// 折线收集器：链式拼路径，最后一次描边
class Stroke {
 public:
  explicit Stroke(Grid grid) : g_(grid) {}

  Stroke& line(float u0, float v0, float u1, float v1) {
    pts_.push_back(PointF{g_.x(u0), g_.y(v0)});
    pts_.push_back(PointF{g_.x(u1), g_.y(v1)});
    return *this;
  }

  // 圆弧：从 a0 扫到 a1（弧度）
  Stroke& arc(float cu, float cv, float r, float a0, float a1) {
    constexpr int kSeg = 24;
    const float step = (a1 - a0) / static_cast<float>(kSeg);
    for (int i = 0; i <= kSeg; ++i) {
      const float a = a0 + step * static_cast<float>(i);
      pts_.push_back(PointF{g_.x(cu + std::cos(a) * r), g_.y(cv + std::sin(a) * r)});
    }
    return *this;
  }

  // 圆角矩形整圈：四段 90° 弧接起来
  Stroke& roundRect(float u0, float v0, float u1, float v1, float r) {
    const float l = u0 + r, t = v0 + r, rt = u1 - r, b = v1 - r;
    arc(l, t, r, kPi, kPi * 1.5f);         // 左上
    arc(rt, t, r, kPi * 1.5f, kPi * 2.0f); // 右上
    arc(rt, b, r, 0.0f, kPi * 0.5f);       // 右下
    arc(l, b, r, kPi * 0.5f, kPi);         // 左下
    arc(l, t, r, kPi, kPi * 1.5f);         // 收口
    return *this;
  }

  void paint(Painter& painter, Color color) const {
    if (pts_.size() < 2) return;
    painter.strokePolylineF(pts_.data(), static_cast<int>(pts_.size()), color, kStroke * g_.k);
  }

 private:
  Grid g_;
  std::vector<PointF> pts_;
};

// 小圆点：比例记号用
void dot(Painter& painter, const Grid& g, float u, float v, float r, Color color) {
  constexpr int kSeg = 16;
  PointF pts[kSeg];
  for (int i = 0; i < kSeg; ++i) {
    const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kSeg);
    pts[i] = PointF{g.x(u + std::cos(a) * r), g.y(v + std::sin(a) * r)};
  }
  painter.fillPolygonF(pts, kSeg, color);
}

// 放大镜：圆 + 手柄；plus 决定镜内是加号还是减号
void paintMagnifier(Painter& painter, const Grid& g, Color color, bool plus) {
  // 圆与手柄一起撑满网格：字形图标、参考图里这个符号都是满格的，
  // 画小了整排看过去会一块一块地瘪下去
  constexpr float cu = 9.9f, cv = 9.9f, r = 8.3f;
  Stroke(g).arc(cu, cv, r, 0.0f, 2.0f * kPi).paint(painter, color);
  // 手柄从圆周右下切点向外
  const float k = r * 0.7071f;
  Stroke(g).line(cu + k, cv + k, 22.2f, 22.2f).paint(painter, color);
  Stroke(g).line(cu - 4.4f, cv, cu + 4.4f, cv).paint(painter, color);
  if (plus) Stroke(g).line(cu, cv - 4.4f, cu, cv + 4.4f).paint(painter, color);
}

// 旋转：圆角画框 + 左上角一小段带弧度的箭头。
//
// 弧不绕整圈：绕圈会把画框整个罩住，两者抢同一块地方，缩到 19px 就糊成
// 一团。只在框的左上角留一小段弧和箭头，指向转向即可——主流图片编辑器的
// 「旋转」符号就是这套画法。
//
// 弧要离开框边一点：紧贴着画会像框上长出一只钩子，那个缺口是「转」的暗示。
// 左旋是同一份几何的镜像。
void paintRotate(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(3.2f, 5.4f, 20.8f, 21.6f, 3.2f).paint(painter, color);

  // 弧：圆心靠近画框左上角，取约 110° 的一段。整段落在框外，与框线留缝；
  // 再短就退化成一条斜杠，认不出是弧
  constexpr float cu = 6.2f, cv = 8.2f, r = 4.6f;
  constexpr float aStart = kPi * 1.00f;   // 左侧，与圆心齐平
  constexpr float aEnd = kPi * 1.58f;     // 越过顶点，略偏右
  Stroke(g).arc(cu, cv, r, aStart, aEnd).paint(painter, color);

  // 箭头落在弧终点，两翼沿切线张开，朝右指（顺时针＝右旋）
  const float tangent = aEnd + kPi * 0.5f;
  const float tu = cu + std::cos(aEnd) * r;
  const float tv = cv + std::sin(aEnd) * r;
  constexpr float kBarb = 3.0f;
  constexpr float kSpread = 0.62f;
  const float b1u = tu - std::cos(tangent - kSpread) * kBarb;
  const float b1v = tv - std::sin(tangent - kSpread) * kBarb;
  const float b2u = tu - std::cos(tangent + kSpread) * kBarb;
  const float b2v = tv - std::sin(tangent + kSpread) * kBarb;
  Stroke(g).line(b1u, b1v, tu, tv).line(b2u, b2v, tu, tv).paint(painter, color);
}

// 适应窗口：四角各一个朝外的 L 形箭头 + 一段斜杆
void paintFitWindow(Painter& painter, const Grid& g, Color color) {
  constexpr float kArm = 4.6f;    // 两翼长度
  constexpr float kShaft = 6.2f;  // 斜杆末端
  const float cu[4] = {3.0f, 21.0f, 3.0f, 21.0f};
  const float cv[4] = {3.0f, 3.0f, 21.0f, 21.0f};
  const float dx[4] = {1.0f, -1.0f, 1.0f, -1.0f};
  const float dy[4] = {1.0f, 1.0f, -1.0f, -1.0f};
  for (int i = 0; i < 4; ++i) {
    const float x = cu[i], y = cv[i], sx = dx[i], sy = dy[i];
    // L 形翅膀贴着角的两条边
    Stroke(g).line(x, y, x + sx * kArm, y).line(x, y, x, y + sy * kArm).paint(painter, color);
    // 斜杆从角内侧指向外
    Stroke(g).line(x + sx * 1.1f, y + sy * 1.1f, x + sx * kShaft, y + sy * kShaft)
        .paint(painter, color);
  }
}

// 实际大小：方框 + 两条竖线夹着比例点，读作 1:1。
//
// 外框要占满作图网格：它本身就是"画面"的轮廓，比同排其它图标小一圈会显得
// 缩小了半档。参考图里这个框和其它图标的外接矩形一样大。
void paintZoomActual(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(1.4f, 1.4f, 22.6f, 22.6f, 4.2f).paint(painter, color);
  // 两条竖线按参考图的相对位置：分别落在框宽的 28% 与 72% 处
  Stroke(g).line(7.3f, 7.1f, 7.3f, 16.0f).paint(painter, color);
  Stroke(g).line(16.7f, 7.1f, 16.7f, 16.0f).paint(painter, color);
  // 比例点要够大才看得见：线宽收细后小圆点只剩两三个像素
  dot(painter, g, 12.0f, 9.2f, 1.05f, color);
  dot(painter, g, 12.0f, 13.9f, 1.05f, color);
}

// 演示：屏幕 + 播放三角 + 支脚
void paintPresent(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(2.0f, 2.8f, 22.0f, 18.2f, 2.8f).paint(painter, color);
  // 播放三角也用描边：整排都是细线，一块实心会很扎眼。
  // 三角要够大——小了就是一坨尖角，认不出是播放
  Stroke(g)
      .line(9.6f, 6.6f, 15.6f, 10.5f)
      .line(15.6f, 10.5f, 9.6f, 14.4f)
      .line(9.6f, 14.4f, 9.6f, 6.6f)
      .paint(painter, color);
  // 支脚
  Stroke(g).line(8.6f, 18.2f, 6.8f, 22.4f).paint(painter, color);
  Stroke(g).line(15.4f, 18.2f, 17.2f, 22.4f).paint(painter, color);
}

// 另存为：软盘轮廓。参考图没有这个符号，按同一套线宽画最通用的那个
void paintSaveAs(Painter& painter, const Grid& g, Color color) {
  // 外壳：右上角切一刀是软盘的辨识特征
  Stroke(g)
      .line(2.6f, 21.4f, 2.6f, 2.6f)
      .line(2.6f, 2.6f, 3.4f, 1.8f)
      .line(3.4f, 1.8f, 18.6f, 1.8f)
      .line(18.6f, 1.8f, 22.2f, 4.2f)
      .line(22.2f, 4.2f, 22.2f, 21.4f)
      .line(22.2f, 21.4f, 2.6f, 21.4f)
      .paint(painter, color);
  // 上部滑盖
  Stroke(g)
      .line(7.6f, 1.8f, 7.6f, 7.6f)
      .line(7.6f, 7.6f, 16.6f, 7.6f)
      .line(16.6f, 7.6f, 16.6f, 1.8f)
      .paint(painter, color);
  // 下部标签
  Stroke(g)
      .line(6.2f, 12.4f, 6.2f, 21.4f)
      .line(6.2f, 21.4f, 18.6f, 21.4f)
      .line(18.6f, 21.4f, 18.6f, 12.4f)
      .paint(painter, color);
}

}  // namespace

bool paintVectorIcon(Painter& painter, IconId id, const Rect& rc, Color color) {
  if (rc.width() <= 0 || rc.height() <= 0) return false;
  const Grid g = makeGrid(rc);

  switch (id) {
    case IconId::kZoomIn:
      paintMagnifier(painter, g, color, true);
      return true;
    case IconId::kZoomOut:
      paintMagnifier(painter, g, color, false);
      return true;
    case IconId::kFitWindow:
      paintFitWindow(painter, g, color);
      return true;
    case IconId::kZoomActual:
      paintZoomActual(painter, g, color);
      return true;
    case IconId::kRotateLeft:
      paintRotate(painter, makeGrid(rc, true), color);
      return true;
    case IconId::kRotateRight:
      paintRotate(painter, g, color);
      return true;
    case IconId::kFullscreen:
      paintPresent(painter, g, color);
      return true;
    case IconId::kSaveAs:
      paintSaveAs(painter, g, color);
      return true;
    default:
      return false;
  }
}

}  // namespace AiUI
