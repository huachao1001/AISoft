// 字体集实现
#include "AiUI/font.h"

#include "AiUI/platform.h"

namespace AiUI {

FontSet fonts;

void FontSet::create(int dpi) {
  destroy();
  Platform* p = platform();
  if (dpi <= 0) dpi = p->screenDpi();
  dpi_ = dpi;
  const wchar_t* family = p->defaultFontFamily();
  ui = p->createFont(family, 10.0f, false, dpi);
  uiBold = p->createFont(family, 10.0f, true, dpi);
  uiSmall = p->createFont(family, 9.0f, false, dpi);
  // 大字：进度条这类需要一眼看清的主行
  uiLarge = p->createFont(family, 13.0f, false, dpi);
  uiLargeBold = p->createFont(family, 13.0f, true, dpi);
  // 超大：单行信息窗口（进度条）的主标题
  uiHuge = p->createFont(family, 20.0f, false, dpi);
  uiHugeBold = p->createFont(family, 20.0f, true, dpi);
}

const Font& FontSet::forPixelSize(int px) {
  if (px < 1) px = 1;
  // 量化档位：缩放动画期间 cell 每帧都在变，按 1px 一档缓存会一路建出上百个
  // 字体。字号差 1~2 像素肉眼分不出来，按量级分档足够，缓存条目也有限。
  const int step = px <= 20 ? 1 : (px <= 60 ? 2 : (px <= 200 ? 5 : 10));
  px = (px + step / 2) / step * step;
  if (px < 1) px = 1;

  auto it = bySize_.find(px);
  if (it != bySize_.end()) return it->second;

  Platform* p = platform();
  int dpi = dpi_ > 0 ? dpi_ : p->screenDpi();
  // createFont 收的是磅值：1pt = dpi/72 像素，反过来除一下
  const float pointSize = static_cast<float>(px) * 72.0f / static_cast<float>(dpi);
  Font font = p->createFont(p->defaultFontFamily(), pointSize, false, dpi);
  if (!font.isValid()) {
    // 造不出来就退回正文，至少不会画出空白
    it = bySize_.emplace(px, ui).first;
    return it->second;
  }
  it = bySize_.emplace(px, font).first;
  return it->second;
}

void FontSet::destroy() {
  Platform* p = platform();
  for (Font* f : {&ui, &uiBold, &uiSmall, &uiLarge, &uiLargeBold, &uiHuge, &uiHugeBold}) {
    if (f->isValid()) p->destroyFont(f->handle());
    *f = Font();
  }
  for (auto& entry : bySize_) {
    if (entry.second.isValid()) p->destroyFont(entry.second.handle());
  }
  bySize_.clear();
  dpi_ = 0;
}

void releaseFonts() { fonts.destroy(); }

}  // namespace AiUI
