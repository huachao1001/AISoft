// 字体集实现
#include "AiUI/font.h"

#include "AiUI/platform.h"

namespace AiUI {

FontSet fonts;

void FontSet::create(int dpi) {
  destroy();
  if (dpi <= 0) dpi = platform()->screenDpi();
  Platform* p = platform();
  ui = p->createFont(L"Microsoft YaHei UI", 10.0f, false, dpi);
  uiBold = p->createFont(L"Microsoft YaHei UI", 10.0f, true, dpi);
  uiSmall = p->createFont(L"Microsoft YaHei UI", 9.0f, false, dpi);
}

void FontSet::destroy() {
  Platform* p = platform();
  for (Font* f : {&ui, &uiBold, &uiSmall}) {
    if (f->isValid()) p->destroyFont(f->handle());
    *f = Font();
  }
}

void releaseFonts() { fonts.destroy(); }

}  // namespace AiUI
