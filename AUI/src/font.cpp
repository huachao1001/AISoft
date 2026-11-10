#include "AUI/font.h"

#include <initializer_list>

namespace AUI {

FontSet fonts;

void FontSet::Create(int dpi) {
  Destroy();
  auto make = [&](int pt, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return ::CreateFontIndirectW(&lf);
  };
  ui = make(10, FW_NORMAL);
  uiBold = make(10, FW_SEMIBOLD);
  uiSmall = make(9, FW_NORMAL);
}

void FontSet::Destroy() {
  for (HFONT* f : {&ui, &uiBold, &uiSmall}) {
    if (*f) {
      ::DeleteObject(*f);
      *f = nullptr;
    }
  }
}

}  // namespace AUI
