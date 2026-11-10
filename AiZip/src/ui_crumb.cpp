// 面包屑导航条：后退按钮 + 路径节点 + 搜索框。
// 平时只读展示，点空白处或 Ctrl+L 切成可编辑输入框。
#include "ui_crumb.h"

#include "app.h"
#include "ui.h"
#include "ui_list.h"
#include "ui_main.h"
#include "util.h"
#include "AUI/input_box.h"
#include "AUI/theme.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>

using namespace AUI;

namespace ui {
namespace {

constexpr const wchar_t* kCrumbClass = L"AIZipCrumbBar";

// 节点样式：padding 4px 7px，圆角 5
constexpr int kNodePadX = 7;
constexpr int kNodePadY = 4;
constexpr int kNodeGap = 2;
constexpr int kBackBtnW = 30;
constexpr int kBackBtnH = 30;
constexpr int kIconSm = 16;
constexpr int kIconGap = 6;  // 图标与文字的间距，宽度计算与绘制必须一致
constexpr int kSearchW = 270;

struct Node {
  std::wstring label;
  std::wstring target;  // 文件系统目录或包内路径
  bool current = false;
  bool home = false;
  RECT rc{};  // 计算出的命中原区
};

HWND g_bar = nullptr;
// 搜索框直接就是 AUI 控件，不再套一层适配函数
std::unique_ptr<AUI::InputBox> g_search;
std::unique_ptr<AUI::InputBox> g_editBox;  // 编辑态输入框，懒创建
bool g_editing = false;
int g_hot = -1;
bool g_backHot = false;
bool g_pressed = false;
std::vector<Node> g_nodes;
RECT g_backRect{};
// 编辑框外框（含 1px 边框），自绘时用
RECT g_editFrame{};

// 压缩包模式：包名 + 包内各层；文件系统模式：盘符 + 各级目录
void BuildNodes() {
  g_nodes.clear();
  if (g_app.archiveMode) {
    Node root;
    root.label = util::BaseName(g_app.archivePath);
    root.target.clear();  // 空 = 包根
    root.current = g_app.innerPath.empty();
    root.home = true;
    g_nodes.push_back(root);

    if (!g_app.innerPath.empty()) {
      std::wstring acc;
      size_t pos = 0;
      const std::wstring& p = g_app.innerPath;
      while (pos <= p.size()) {
        size_t sep = p.find(L'\\', pos);
        if (sep == std::wstring::npos) sep = p.size();
        const std::wstring seg = p.substr(pos, sep - pos);
        pos = sep + 1;
        if (seg.empty()) continue;
        if (!acc.empty()) acc += L"\\";
        acc += seg;
        Node n;
        n.label = seg;
        n.target = acc;
        n.current = (sep >= p.size());
        g_nodes.push_back(n);
        if (pos > p.size()) break;
      }
    }
    return;
  }

  // 文件系统：拆成 "盘符\a\b\c"
  const std::wstring& dir = g_app.curDir;
  if (dir.empty()) return;
  size_t pos = 0;
  // 盘符根 "C:\"
  if (dir.size() >= 2 && dir[1] == L':') {
    Node n;
    n.label = dir.substr(0, 2);
    n.target = dir.substr(0, 2) + L"\\";
    n.home = true;
    g_nodes.push_back(n);
    pos = (dir.size() >= 3 && (dir[2] == L'\\' || dir[2] == L'/')) ? 3 : 2;
  }
  std::wstring acc = g_nodes.empty() ? std::wstring() : g_nodes[0].target;
  while (pos < dir.size()) {
    size_t sep = dir.find_first_of(L"\\/", pos);
    if (sep == std::wstring::npos) sep = dir.size();
    const std::wstring seg = dir.substr(pos, sep - pos);
    pos = sep + 1;
    if (seg.empty()) continue;
    if (!acc.empty() && acc.back() != L'\\') acc += L'\\';
    acc += seg;
    Node n;
    n.label = seg;
    n.target = acc;
    g_nodes.push_back(n);
    if (pos > dir.size()) break;
  }
  if (!g_nodes.empty()) g_nodes.back().current = true;
}

// 搜索框位置：始终贴右；宽度随窗口收缩，避开面包屑
RECT SearchRect(int cx) {
  const int h = g_app.px(34);
  const int pad = g_app.px(16);
  // 至少要给面包屑留出后退按钮 + 一段路径的空间
  const int minCrumb = g_app.px(kBackBtnW) + g_app.px(120);
  const int avail = cx - pad * 2 - minCrumb;
  const int w = (std::max)(g_app.px(120), (std::min)(g_app.px(kSearchW), avail));
  return RECT{cx - pad - w, (g_app.px(kCrumbH) - h) / 2, cx - pad,
              (g_app.px(kCrumbH) - h) / 2 + h};
}

void Layout(int cx) {
  HDC dc = ::GetDC(g_bar);
  Painter p(dc);
  const int barH = g_app.px(kCrumbH);

  g_backRect = RECT{g_app.px(16), (barH - g_app.px(kBackBtnH)) / 2,
                    g_app.px(16) + g_app.px(kBackBtnW),
                    (barH - g_app.px(kBackBtnH)) / 2 + g_app.px(kBackBtnH)};

  int x = g_backRect.right + g_app.px(6);
  // 面包屑最多占到搜索框左侧，超出的节点不绘制也不可点
  const int limit = SearchRect(cx).left - g_app.px(8);

  for (size_t i = 0; i < g_nodes.size(); ++i) {
    Node& n = g_nodes[i];
    int w = p.TextWidth(n.label, fonts.ui) + g_app.px(kNodePadX) * 2;
    if (n.home) w += g_app.px(kIconSm) + g_app.px(kIconGap);
    const int h = g_app.px(kNodePadY) * 2 + g_app.px(20);
    n.rc = RECT{x, (barH - h) / 2, x + w, (barH - h) / 2 + h};
    // 放不下的节点标记成空矩形，既不绘制也不可点
    if (n.rc.right > limit) {
      n.rc = RECT{};
      for (size_t j = i; j < g_nodes.size(); ++j) g_nodes[j].rc = RECT{};
      break;
    }
    x += w;

    if (i + 1 < g_nodes.size()) {
      const int sepW = g_app.px(16) + g_app.px(kNodeGap) * 2;
      x += sepW;
    }
  }
  ::ReleaseDC(g_bar, dc);
}

void PaintBar(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, kPaneBg);

  // 编辑态：输入框自己画圆角与描边，这里只补底部细线
  if (g_editing && !::IsRectEmpty(&g_editFrame)) {
    p.HLine(0, rc.right, rc.bottom - 1, kLine);
    canvas.Commit();
    ::ReleaseDC(hwnd, target);
    return;
  }

  // 后退按钮
  if (g_backHot) p.FillRound(g_backRect, kHoverBg, g_app.px(6));
  const bool canBack = AppCanBack();
  const int bs = g_app.px(18);
  RECT bi{g_backRect.left + (g_backRect.right - g_backRect.left - bs) / 2,
          g_backRect.top + (g_backRect.bottom - g_backRect.top - bs) / 2,
          g_backRect.left + (g_backRect.right - g_backRect.left - bs) / 2 + bs,
          g_backRect.top + (g_backRect.bottom - g_backRect.top - bs) / 2 + bs};
  p.Icon(IconId::kArrowBack, bi, canBack ? kInk : kTextFaint, canBack);

  for (size_t i = 0; i < g_nodes.size(); ++i) {
    const Node& n = g_nodes[i];
    if (n.rc.right > rc.right) break;
    const bool hot = (g_hot == static_cast<int>(i));

    if (n.current) {
      p.FillRound(n.rc, kCurrentBg, g_app.px(5));
    } else if (hot) {
      p.FillRound(n.rc, kLinkHover, g_app.px(5));
    }

    int textLeft = n.rc.left + g_app.px(kNodePadX);
    if (n.home) {
      const int is = g_app.px(kIconSm);
      RECT ir{textLeft, n.rc.top + (n.rc.bottom - n.rc.top - is) / 2, textLeft + is,
              n.rc.top + (n.rc.bottom - n.rc.top - is) / 2 + is};
      p.Icon(IconId::kHome, ir, n.current ? kInk : kLink);
      textLeft = ir.right + g_app.px(kIconGap);
    }
    RECT tr{textLeft, n.rc.top, n.rc.right - g_app.px(kNodePadX), n.rc.bottom};
    p.TextLeft(tr, n.label, n.current ? RGB(0x11, 0x11, 0x11) : kLink, fonts.ui);

    // 分隔箭头
    if (i + 1 < g_nodes.size()) {
      const int sepW = g_app.px(16) + g_app.px(kNodeGap) * 2;
      RECT sr{n.rc.right + g_app.px(kNodeGap), n.rc.top, n.rc.right + sepW, n.rc.bottom};
      if (sr.right > rc.right) break;
      const int cs = g_app.px(14);
      RECT cr{sr.left + (sr.right - sr.left - cs) / 2, sr.top + (sr.bottom - sr.top - cs) / 2,
              sr.left + (sr.right - sr.left - cs) / 2 + cs,
              sr.top + (sr.bottom - sr.top - cs) / 2 + cs};
      p.Icon(IconId::kChevron, cr, RGB(0x9c, 0xa3, 0xaf));
    }
  }

  p.HLine(0, rc.right, rc.bottom - 1, kLine);
  canvas.Commit();
  ::ReleaseDC(hwnd, target);
}

void NavToNode(const Node& n) {
  if (g_app.archiveMode) {
    g_app.innerPath = n.target;
    AppRefreshView();
  } else if (!n.target.empty()) {
    AppNavigateTo(n.target);
  }
}

// 只重排编辑框，不改文本也不动光标（窗口缩放时用）
void RepositionEdit();

// 地址编辑框自己的键盘处理：回车提交、Esc 取消。
void CommitEdit();
void HideEdit();

// 编辑态：把输入框摆到面包屑位置，填入当前完整路径
void ShowEdit() {
  if (!g_editBox) {
    g_editBox = std::make_unique<InputBox>();
    InputSpec spec;
    spec.padX = 10;
    spec.radius = 7;  // 圆角由 InputBox 自绘，父窗口不再重复描边
    spec.hasLeadIcon = false;
    g_editBox->SetSpec(spec);
    // 不用 CLIENTEDGE：边框由面包屑栏自绘，保持圆角与配色一致
    g_editBox->Create(g_bar,
                      reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(g_bar, GWLP_HINSTANCE)),
                      kPathEditId, L"AIZipPathEdit",
                      WS_CHILD | WS_TABSTOP);
    // 回车提交、Esc 放弃，都由控件回调接管
    g_editBox->SetSubmitHandler([](bool committed) {
      if (committed) {
        CommitEdit();
      } else {
        HideEdit();
      }
      ::SetFocus(MainList());
    });
    // 点到别处即放弃编辑，与原 EN_KILLFOCUS 行为一致
    g_editBox->SetBlurHandler([] {
      if (g_editing) HideEdit();
    });
  }
  std::wstring text;
  if (g_app.archiveMode) {
    text = g_app.archivePath;
    if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
  } else {
    text = g_app.curDir;
  }
  RepositionEdit();
  g_editBox->SetText(text);
  // 选中全部，直接输入即可替换（与 Explorer 的 Ctrl+L 一致）
  g_editBox->SelectAll();
  ::ShowWindow(g_editBox->hwnd(), SW_SHOW);
  ::SetFocus(g_editBox->hwnd());
  g_editing = true;
  // 自绘圆角边框在父窗口，必须让面包屑栏重画
  ::InvalidateRect(g_bar, nullptr, TRUE);
}

void HideEdit() {
  if (g_editBox && g_editBox->hwnd()) ::ShowWindow(g_editBox->hwnd(), SW_HIDE);
  g_editing = false;
  ::InvalidateRect(g_bar, nullptr, TRUE);
}

void RepositionEdit() {
  if (!g_editBox || !g_editBox->hwnd()) return;
  RECT cr;
  ::GetClientRect(g_bar, &cr);
  const int h = g_app.px(32);
  const int y = (g_app.px(kCrumbH) - h) / 2;
  const int right = SearchRect(cr.right).left - g_app.px(8);
  const int left = g_app.px(16);
  const int width = (std::max)(g_app.px(120), right - left);
  g_editFrame = RECT{left, y, left + width, y + h};

  ::MoveWindow(g_editBox->hwnd(), left, y, width, h, TRUE);
}

// 包内路径是否存在：任一条目就是它，或在它下面。
// 7z 条目里的分隔符可能是 '/'，两种都要认。
bool InnerPathExists(std::wstring inner) {
  if (inner.empty()) return true;
  std::replace(inner.begin(), inner.end(), L'/', L'\\');

  for (const auto& e : g_app.archive.Entries()) {
    std::wstring p = e.path;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    if (::CompareStringOrdinal(p.c_str(), -1, inner.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    if (p.size() > inner.size() && p[inner.size()] == L'\\' &&
        ::CompareStringOrdinal(p.c_str(), static_cast<int>(inner.size()), inner.c_str(),
                               static_cast<int>(inner.size()), TRUE) == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

void CommitEdit() {
  if (!g_editBox) return;
  const std::wstring target = g_editBox->Text();
  HideEdit();
  if (target.empty()) return;

  // 压缩包模式：完整 "包路径\内部\路径"，或直接输入包内相对路径
  if (g_app.archiveMode) {
    std::wstring inner;
    const std::wstring base = g_app.archivePath;
    if (target.size() > base.size() &&
        ::CompareStringOrdinal(target.c_str(), static_cast<int>(base.size()), base.c_str(),
                               static_cast<int>(base.size()), TRUE) == CSTR_EQUAL) {
      inner = target.substr(base.size());
      while (!inner.empty() && (inner.front() == L'\\' || inner.front() == L'/')) {
        inner.erase(inner.begin());
      }
    } else if (!util::PathExists(target)) {
      // 不是磁盘上的路径，按包内相对路径理解
      inner = target;
      while (!inner.empty() && (inner.front() == L'\\' || inner.front() == L'/')) {
        inner.erase(inner.begin());
      }
      while (!inner.empty() && (inner.back() == L'\\' || inner.back() == L'/')) inner.pop_back();
      std::replace(inner.begin(), inner.end(), L'/', L'\\');
    }

    if (target == base || !inner.empty()) {
      if (!InnerPathExists(inner)) {
        ::MessageBoxW(g_bar, (L"压缩包内没有该路径：\n" + inner).c_str(), L"AIZip",
                      MB_OK | MB_ICONWARNING);
        ::InvalidateRect(g_bar, nullptr, FALSE);
        return;
      }
      g_app.innerPath = inner;
      AppRefreshView();
      return;
    }
    // 输入的是别的包或目录
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      AppOpenArchive(target);
      return;
    }
  }

  if (util::IsDirectory(target)) {
    AppNavigateTo(target);
  } else if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
    AppOpenArchive(target);
  } else {
    ::MessageBoxW(g_bar, (L"路径不存在：\n" + target).c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
    ::InvalidateRect(g_bar, nullptr, FALSE);
  }
}

LRESULT CALLBACK CrumbProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SIZE: {
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      // 搜索框宿主是独立子窗口，缩放时必须跟着重定位
      if (g_search) {
        const RECT sr = SearchRect(cr.right);
        ::MoveWindow(g_search->hwnd(), sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top,
                     TRUE);
      }
      Layout(cr.right);
      if (g_editing) RepositionEdit();
      return 0;
    }

    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      Painter(dc).Fill(rc, kPaneBg);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintBar(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      int hit = -1;
      for (size_t i = 0; i < g_nodes.size(); ++i) {
        if (::PtInRect(&g_nodes[i].rc, pt)) {
          hit = static_cast<int>(i);
          break;
        }
      }
      const bool backHot = ::PtInRect(&g_backRect, pt) != 0;
      if (hit != g_hot || backHot != g_backHot) {
        g_hot = hit;
        g_backHot = backHot;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      g_hot = -1;
      g_backHot = false;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN: {
      // 只有按下发生在面包屑内才算有效点击，丢掉切窗/切桌面时残留的抬起消息
      RECT cr;
      ::GetClientRect(hwnd, &cr);
      g_pressed = ::PtInRect(&cr, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) != 0;
      return 0;
    }

    case WM_LBUTTONUP: {
      if (!g_pressed) return 0;
      g_pressed = false;
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (::PtInRect(&g_backRect, pt)) {
        if (AppCanBack()) AppGoBack();
        return 0;
      }
      for (size_t i = 0; i < g_nodes.size(); ++i) {
        if (::PtInRect(&g_nodes[i].rc, pt)) {
          NavToNode(g_nodes[i]);
          return 0;
        }
      }
      // 点空白处：进入编辑
      BeginCrumbEdit();
      return 0;
    }

    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, g_hot >= 0 || g_backHot ? IDC_HAND : IDC_ARROW));
      return TRUE;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND CreateCrumbBar(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered =
        RegisterWindowClass(kCrumbClass, CrumbProc, inst, CS_HREDRAW | CS_VREDRAW, nullptr);
  }
  g_bar = ::CreateWindowExW(0, kCrumbClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);

  // 搜索框：AUI 控件直接用，配置好图标与占位提示即可
  g_search = std::make_unique<AUI::InputBox>();
  AUI::InputSpec spec;
  spec.cue = L"在压缩包内搜索文件或文件夹";
  spec.hasLeadIcon = true;
  spec.leadIcon = AUI::IconId::kSearch;
  spec.leadIconSize = 14;
  spec.leadIconGap = 5;
  spec.padX = 11;
  spec.radius = 7;
  g_search->SetSpec(spec);
  g_search->Create(g_bar, inst, kSearchId, L"AIZipSearchBox");
  g_search->SetChangeHandler([] { SetFilterText(g_search->Text()); });
  return g_bar;
}

HWND CrumbSearchEdit() { return g_search ? g_search->Edit() : nullptr; }

bool CrumbIsEditing() { return g_editing; }

void BeginCrumbEdit() {
  if (g_editing) {
    // 已在编辑：把焦点交回去，不要重填文本把用户输入冲掉
    if (g_editBox && g_editBox->Edit()) ::SetFocus(g_editBox->Edit());
    return;
  }
  ShowEdit();
}

void RefreshCrumbBar() {
  if (!g_bar) return;
  BuildNodes();
  RECT cr;
  ::GetClientRect(g_bar, &cr);
  if (g_search) {
    const RECT sr = SearchRect(cr.right);
    ::MoveWindow(g_search->hwnd(), sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, TRUE);
    g_search->SetCue(g_app.archiveMode ? L"在压缩包内搜索文件或文件夹"
                                       : L"搜索文件或文件夹");
  }
  Layout(cr.right);
  if (g_editing) RepositionEdit();
  ::InvalidateRect(g_bar, nullptr, FALSE);
}

}  // namespace ui
