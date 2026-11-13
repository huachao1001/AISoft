// 字体集实现
#include "AiUI/font.h"

#include "AiUI/platform.h"

namespace AiUI {

FontSet fonts;

void FontSet::create(int dpi) {
  destroy();
  Platform* p = platform();
  if (dpi <= 0) dpi = p->screenDpi();
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

void FontSet::destroy() {
  Platform* p = platform();
  for (Font* f : {&ui, &uiBold, &uiSmall, &uiLarge, &uiLargeBold, &uiHuge, &uiHugeBold}) {
    if (f->isValid()) p->destroyFont(f->handle());
    *f = Font();
  }
}

void releaseFonts() { fonts.destroy(); }

}  // namespace AiUI
