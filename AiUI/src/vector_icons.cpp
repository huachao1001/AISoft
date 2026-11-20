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
  constexpr float cu = 10.3f, cv = 10.3f, r = 6.5f;
  Stroke(g).arc(cu, cv, r, 0.0f, 2.0f * kPi).paint(painter, color);
  // 手柄从圆周右下切点向外，不压进圆内
  const float k = r * 0.7071f;
  Stroke(g).line(cu + k, cv + k, 20.4f, 20.4f).paint(painter, color);
  Stroke(g).line(cu - 3.5f, cv, cu + 3.5f, cv).paint(painter, color);
  if (plus) Stroke(g).line(cu, cv - 3.5f, cu, cv + 3.5f).paint(painter, color);
}

// 旋转：下方圆角画框 + 上方弧形箭头。
//
// 比例照参考图量：画框占下半，弧的半径几乎顶到图标上沿，从画框左外侧
// 起弧、越过顶部、在右上方收成箭头。弧画小了会像一道拱门贴着框顶，
// 认不出在转。左旋是同一份几何的左右镜像。
void paintRotate(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(5.2f, 12.4f, 20.2f, 21.4f, 2.6f).paint(painter, color);

  // 弧：圆心略偏左，半径撑满图标宽度，弧顶贴着上沿
  constexpr float cu = 11.5f, cv = 13.5f, r = 9.5f;
  constexpr float aStart = kPi * 0.955f;  // 左外侧，略低于圆心
  constexpr float aEnd = kPi * 1.695f;    // 越过顶部后落在右上方
  Stroke(g).arc(cu, cv, r, aStart, aEnd).paint(painter, color);

  // 箭头：落地在弧终点，两翼沿弧的切线张开
  const float tangent = aEnd + kPi * 0.5f;
  const float tu = cu + std::cos(aEnd) * r;
  const float tv = cv + std::sin(aEnd) * r;
  constexpr float kBarb = 2.6f;
  constexpr float kSpread = 0.78f;
  const float b1u = tu - std::cos(tangent - kSpread) * kBarb;
  const float b1v = tv - std::sin(tangent - kSpread) * kBarb;
  const float b2u = tu - std::cos(tangent + kSpread) * kBarb;
  const float b2v = tv - std::sin(tangent + kSpread) * kBarb;
  Stroke(g).line(b1u, b1v, tu, tv).line(b2u, b2v, tu, tv).paint(painter, color);
}

// 适应窗口：四角各一个朝外的 L 形箭头 + 一段斜杆
void paintFitWindow(Painter& painter, const Grid& g, Color color) {
  constexpr float kArm = 4.0f;    // 两翼长度
  constexpr float kShaft = 5.4f;  // 斜杆末端
  const float cu[4] = {4.0f, 20.0f, 4.0f, 20.0f};
  const float cv[4] = {4.0f, 4.0f, 20.0f, 20.0f};
  const float dx[4] = {1.0f, -1.0f, 1.0f, -1.0f};
  const float dy[4] = {1.0f, 1.0f, -1.0f, -1.0f};
  for (int i = 0; i < 4; ++i) {
    const float x = cu[i], y = cv[i], sx = dx[i], sy = dy[i];
    // L 形翅膀贴着角的两条边
    Stroke(g).line(x, y, x + sx * kArm, y).line(x, y, x, y + sy * kArm).paint(painter, color);
    // 斜杆从角内侧指向外
    Stroke(g).line(x + sx * 1.0f, y + sy * 1.0f, x + sx * kShaft, y + sy * kShaft)
        .paint(painter, color);
  }
}

// 实际大小：方框 + 两条竖线夹着比例点，读作 1:1
void paintZoomActual(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(4.2f, 4.2f, 19.8f, 19.8f, 3.8f).paint(painter, color);
  Stroke(g).line(9.6f, 8.4f, 9.6f, 15.6f).paint(painter, color);
  Stroke(g).line(14.4f, 8.4f, 14.4f, 15.6f).paint(painter, color);
  // 比例点要够大才看得见：线宽收细后 0.6 的半径只剩两三个像素
  dot(painter, g, 12.0f, 10.1f, 0.85f, color);
  dot(painter, g, 12.0f, 13.9f, 0.85f, color);
}

// 演示：屏幕 + 播放三角 + 支脚
void paintPresent(Painter& painter, const Grid& g, Color color) {
  Stroke(g).roundRect(3.2f, 4.4f, 20.8f, 17.0f, 2.4f).paint(painter, color);
  // 播放三角也用描边：整排都是细线，一块实心会很扎眼。
  // 三角在屏幕里要居中，且得够大——小了就是一坨尖角，认不出是播放
  Stroke(g)
      .line(9.6f, 7.6f, 15.4f, 10.7f)
      .line(15.4f, 10.7f, 9.6f, 13.8f)
      .line(9.6f, 13.8f, 9.6f, 7.6f)
      .paint(painter, color);
  // 支脚
  Stroke(g).line(9.2f, 17.0f, 7.4f, 20.6f).paint(painter, color);
  Stroke(g).line(14.8f, 17.0f, 16.6f, 20.6f).paint(painter, color);
}

// 另存为：软盘轮廓。参考图没有这个符号，按同一套线宽画最通用的那个
void paintSaveAs(Painter& painter, const Grid& g, Color color) {
  // 外壳：右上角切一刀是软盘的辨识特征
  Stroke(g)
      .line(4.8f, 19.4f, 4.8f, 5.4f)
      .line(4.8f, 5.4f, 5.4f, 4.6f)
      .line(5.4f, 4.6f, 16.8f, 4.6f)
      .line(16.8f, 4.6f, 19.4f, 6.0f)
      .line(19.4f, 6.0f, 19.4f, 19.4f)
      .line(19.4f, 19.4f, 4.8f, 19.4f)
      .paint(painter, color);
  // 上部滑盖
  Stroke(g)
      .line(8.8f, 4.6f, 8.8f, 9.2f)
      .line(8.8f, 9.2f, 15.4f, 9.2f)
      .line(15.4f, 9.2f, 15.4f, 4.6f)
      .paint(painter, color);
  // 下部标签
  Stroke(g)
      .line(7.6f, 13.2f, 7.6f, 19.4f)
      .line(7.6f, 19.4f, 16.6f, 19.4f)
      .line(16.6f, 19.4f, 16.6f, 13.2f)
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
