#include "pyui/testing/recording_painter.h"

#include <algorithm>
#include <sstream>

namespace pyui {

namespace {

const char* kindName(DrawCommand::Kind kind) {
  switch (kind) {
    case DrawCommand::Kind::kFillRect:
      return "fillRect";
    case DrawCommand::Kind::kFillRoundRect:
      return "fillRoundRect";
    case DrawCommand::Kind::kStrokeRoundRect:
      return "strokeRoundRect";
    case DrawCommand::Kind::kGradientRect:
      return "gradientRect";
    case DrawCommand::Kind::kGradientRoundRect:
      return "gradientRoundRect";
    case DrawCommand::Kind::kCheckerboard:
      return "checkerboard";
    case DrawCommand::Kind::kCoverageMask:
      return "coverageMask";
    case DrawCommand::Kind::kText:
      return "text";
    case DrawCommand::Kind::kTextRun:
      return "textRun";
    case DrawCommand::Kind::kIcon:
      return "icon";
    case DrawCommand::Kind::kImage:
      return "image";
    case DrawCommand::Kind::kImageScaled:
      return "imageScaled";
    case DrawCommand::Kind::kSetClip:
      return "setClip";
    case DrawCommand::Kind::kResetClip:
      return "resetClip";
    case DrawCommand::Kind::kClipRect:
      return "clipRect";
    case DrawCommand::Kind::kClipRoundRect:
      return "clipRoundRect";
    case DrawCommand::Kind::kTranslate:
      return "translate";
    case DrawCommand::Kind::kRotate:
      return "rotate";
    case DrawCommand::Kind::kSave:
      return "save";
    case DrawCommand::Kind::kRestore:
      return "restore";
  }
  return "?";
}

std::string rectText(const Rect& rc) {
  std::ostringstream out;
  out << rc.left << ',' << rc.top << ',' << rc.right << ',' << rc.bottom;
  return out.str();
}

}  // namespace

bool DrawCommand::operator==(const DrawCommand& o) const {
  // 逐字段比较。不做字符串往返——浮点格式化在不同 CRT 下会有差异，
  // 而这条等式必须在三个平台上得出同一个结果。
  return kind == o.kind && rect == o.rect && rect2 == o.rect2 && color == o.color &&
         color2 == o.color2 && radius == o.radius && lineWidth == o.lineWidth &&
         intValue == o.intValue && intValue2 == o.intValue2 && floatValue == o.floatValue &&
         text == o.text && fontHeight == o.fontHeight && icon == o.icon;
}

std::string DrawCommand::describe() const {
  std::ostringstream out;
  out << kindName(kind) << ' ' << rectText(rect);
  switch (kind) {
    case Kind::kFillRect:
      out << " color=#" << std::hex << color;
      break;
    case Kind::kFillRoundRect:
      out << " radius=" << std::dec << radius << " color=#" << std::hex << color;
      break;
    case Kind::kStrokeRoundRect:
      out << " radius=" << std::dec << radius << " width=" << lineWidth << " color=#" << std::hex
          << color;
      break;
    case Kind::kGradientRect:
    case Kind::kGradientRoundRect:
      out << " radius=" << std::dec << radius << " from=#" << std::hex << color << " to=#"
          << color2;
      break;
    case Kind::kCheckerboard:
      out << " cell=" << std::dec << intValue << " a=#" << std::hex << color << " b=#" << color2;
      break;
    case Kind::kCoverageMask:
      out << " color=#" << std::hex << color;
      break;
    case Kind::kText:
      out << " align=" << std::dec << intValue << " font=" << fontHeight << " color=#" << std::hex
          << color << " text=\"" << text << '"';
      break;
    case Kind::kTextRun:
      out << " font=" << std::dec << fontHeight << " color=#" << std::hex << color << " text=\""
          << text << '"';
      break;
    case Kind::kIcon:
      out << " icon=" << std::dec << static_cast<int>(icon) << " color=#" << std::hex << color;
      break;
    case Kind::kImage:
      out << " image=" << (color != 0 ? "set" : "null");
      break;
    case Kind::kImageScaled:
      out << " image=" << (color != 0 ? "set" : "null") << " src=" << rectText(rect2)
          << " alpha=" << std::dec << floatValue;
      break;
    case Kind::kClipRect:
    case Kind::kSetClip:
      break;
    case Kind::kClipRoundRect:
      out << " radius=" << std::dec << radius;
      break;
    case Kind::kTranslate:
      out << " dx=" << std::dec << intValue << " dy=" << intValue2;
      break;
    case Kind::kRotate:
      out << " degrees=" << std::dec << floatValue;
      break;
    case Kind::kResetClip:
    case Kind::kSave:
    case Kind::kRestore:
      break;
  }
  return out.str();
}

std::string RecordingPainter::diff(const std::vector<DrawCommand>& a,
                                   const std::vector<DrawCommand>& b) {
  const size_t common = std::min(a.size(), b.size());
  for (size_t i = 0; i < common; ++i) {
    if (a[i] == b[i]) continue;
    std::ostringstream out;
    out << "第一条差异在 #" << i << "\n  A: " << a[i].describe() << "\n  B: "
        << b[i].describe();
    return out.str();
  }
  if (a.size() == b.size()) return {};

  std::ostringstream out;
  if (a.size() > b.size()) {
    out << "B 少了 " << (a.size() - b.size()) << " 条，第 " << common << " 条起缺失：\n  A: "
        << a[common].describe();
  } else {
    out << "B 多了 " << (b.size() - a.size()) << " 条，第 " << common << " 条起多出：\n  B: "
        << b[common].describe();
  }
  return out.str();
}

void RecordingPainter::dumpTo(std::ostream& os) const {
  os << commands_.size() << " 条绘制命令\n";
  for (size_t i = 0; i < commands_.size(); ++i) {
    os << "  #" << i << ' ' << commands_[i].describe() << '\n';
  }
}

std::string RecordingPainter::dump() const {
  std::ostringstream out;
  dumpTo(out);
  return out.str();
}

// ── PaintBackend ──

void RecordingPainter::fillRect(const Rect& rc, Color color) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kFillRect;
  cmd.rect = rc;
  cmd.color = color;
  push(std::move(cmd));
}

void RecordingPainter::fillRoundRect(const Rect& rc, Color color, int radius) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kFillRoundRect;
  cmd.rect = rc;
  cmd.color = color;
  cmd.radius = radius;
  push(std::move(cmd));
}

void RecordingPainter::strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kStrokeRoundRect;
  cmd.rect = rc;
  cmd.color = color;
  cmd.radius = radius;
  cmd.lineWidth = lineWidth;
  push(std::move(cmd));
}

void RecordingPainter::drawGradientRect(const Rect& rc, Color top, Color bottom) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kGradientRect;
  cmd.rect = rc;
  cmd.color = top;
  cmd.color2 = bottom;
  push(std::move(cmd));
}

void RecordingPainter::drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kGradientRoundRect;
  cmd.rect = rc;
  cmd.color = top;
  cmd.color2 = bottom;
  cmd.radius = radius;
  push(std::move(cmd));
}

void RecordingPainter::fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kCheckerboard;
  cmd.rect = rc;
  cmd.color = colorA;
  cmd.color2 = colorB;
  cmd.intValue = cell;
  push(std::move(cmd));
}

void RecordingPainter::blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                                         Color color) {
  // 蒙版内容不进命令序列：它是平台字体的栅格化结果，跨平台必然不同。
  // 这里只记「在哪块矩形上打了一层 color」，即控件意图的位置与颜色。
  (void)coverage;
  (void)stride;
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kCoverageMask;
  cmd.rect = rc;
  cmd.color = color;
  push(std::move(cmd));
}

void RecordingPainter::drawText(const Rect& rc, std::string_view text, Color color,
                                const Font& font, TextAlign align) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kText;
  cmd.rect = rc;
  cmd.color = color;
  cmd.text = std::string(text);
  cmd.fontHeight = font.pixelHeight();
  cmd.intValue = static_cast<int>(align);
  push(std::move(cmd));
}

void RecordingPainter::drawTextRun(const Rect& rc, std::string_view text, Color color,
                                   const Font& font) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kTextRun;
  cmd.rect = rc;
  cmd.color = color;
  cmd.text = std::string(text);
  cmd.fontHeight = font.pixelHeight();
  push(std::move(cmd));
}

int RecordingPainter::measureText(std::string_view text, const Font& font) {
  (void)font;
  return static_cast<int>(utf8Length(text)) * measureAdvance_;
}

int RecordingPainter::advanceText(std::string_view text, const Font& font) {
  (void)font;
  return static_cast<int>(utf8Length(text)) * measureAdvance_;
}

int RecordingPainter::textHeight(const Font& font) { return font.isValid() ? font.pixelHeight() : measureHeight_; }

void RecordingPainter::drawIcon(IconId id, const Rect& rc, Color color) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kIcon;
  cmd.rect = rc;
  cmd.color = color;
  cmd.icon = id;
  push(std::move(cmd));
}

void RecordingPainter::drawImage(const Image& image, const Rect& rc) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kImage;
  cmd.rect = rc;
  cmd.color = image.isValid() ? 1u : 0u;
  push(std::move(cmd));
}

void RecordingPainter::drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                                       ImageSmoothing smoothing, float alpha) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kImageScaled;
  cmd.rect = rc;
  cmd.rect2 = source;
  cmd.color = image.isValid() ? 1u : 0u;
  cmd.intValue = static_cast<int>(smoothing);
  cmd.floatValue = alpha;
  push(std::move(cmd));
}

void RecordingPainter::setClip(const Rect& rc) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kSetClip;
  cmd.rect = rc;
  push(std::move(cmd));
}

void RecordingPainter::resetClip() {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kResetClip;
  push(std::move(cmd));
}

void RecordingPainter::clipRect(const Rect& rc) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kClipRect;
  cmd.rect = rc;
  push(std::move(cmd));
}

void RecordingPainter::clipRoundRect(const Rect& rc, int radius) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kClipRoundRect;
  cmd.rect = rc;
  cmd.radius = radius;
  push(std::move(cmd));
}

void RecordingPainter::translate(int dx, int dy) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kTranslate;
  cmd.intValue = dx;
  cmd.intValue2 = dy;
  push(std::move(cmd));
}

void RecordingPainter::rotate(float degrees) {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kRotate;
  cmd.floatValue = degrees;
  push(std::move(cmd));
}

void RecordingPainter::save() {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kSave;
  push(std::move(cmd));
}

void RecordingPainter::restore() {
  DrawCommand cmd;
  cmd.kind = DrawCommand::Kind::kRestore;
  push(std::move(cmd));
}

}  // namespace pyui
