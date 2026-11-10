#include "AUI/button_bar.h"

#include "AUI/painter.h"

namespace AUI {

Button* ButtonBar::Add(const ButtonSpec& spec, int group) {
  auto btn = std::make_unique<Button>();
  ButtonBarItem item;
  item.button = std::move(btn);
  item.group = group;
  items_.push_back(std::move(item));

  Button* raw = items_.back().button.get();
  raw->SetSpec(spec);
  raw->Create(hwnd_, reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE)), 0,
              L"AIZipButton");
  Relayout();
  return raw;
}

Button* ButtonBar::At(size_t index) {
  return index < items_.size() ? items_[index].button.get() : nullptr;
}

void ButtonBar::Clear() {
  for (auto& it : items_) {
    if (it.button->hwnd()) ::DestroyWindow(it.button->hwnd());
  }
  items_.clear();
  groupLines_.clear();
}

void ButtonBar::OnSize(int cx, int cy) {
  (void)cy;
  Relayout();
  (void)cx;
}

void ButtonBar::OnPaint() {
  const RECT rc = ClientRect();
  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, kToolBg);

  for (size_t i = 0; i < groupLines_.size(); ++i) {
    const RECT& r = groupLines_[i];
    p.VLine(r.top, r.bottom, r.left, RGB(0xdc, 0xdc, 0xdc));
  }
  p.HLine(0, rc.right, rc.bottom - 1, kLine);
  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}

bool ButtonBar::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, kToolBg);
  return true;
}

void ButtonBar::Relayout() {
  if (!hwnd_) return;
  groupLines_.clear();

  const RECT rc = ClientRect();
  const int h = px(height_);
  const int top = (rc.bottom - h) / 2;
  const int gap = px(gap_);
  const int groupGap = px(groupGap_);

  int x = px(edgePad_);
  int prevGroup = 0;
  for (size_t i = 0; i < items_.size(); ++i) {
    ButtonBarItem& it = items_[i];
    if (i > 0) {
      if (it.group != prevGroup) {
        // 组间用更宽的间距，分隔线插在中间
        x += groupGap;
        const int lineX = x - groupGap / 2;
        groupLines_.push_back(RECT{lineX, top, lineX + 1, top + h});
      } else {
        x += gap;
      }
    }
    prevGroup = it.group;
    if (!it.button->hwnd()) continue;
    const int w = it.button->PreferredWidth();
    ::MoveWindow(it.button->hwnd(), x, top, w, h, TRUE);
    x += w;
  }
  Invalidate();
}

}  // namespace AUI
