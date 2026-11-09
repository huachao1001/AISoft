// 面包屑导航条：后退按钮 + 路径节点 + 搜索框。
// 平时只读展示，点空白处或 Ctrl+L 切成可编辑输入框。
#include "ui_crumb.h"

#include "app.h"
#include "ui.h"
#include "ui_list.h"
#include "ui_main.h"
#include "ui_search.h"
#include "util.h"

#include <windowsx.h>

#include <algorithm>

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
constexpr int kSearchW = 270;

struct Node {
  std::wstring label;
  std::wstring target;  // 文件系统目录或包内路径
  bool current = false;
  bool home = false;
  RECT rc{};  // 计算出的命中原区
};

HWND g_bar = nullptr;
HWND g_search = nullptr;
HWND g_edit = nullptr;  // 编辑态输入框，懒创建
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

// 搜索框位置：始终贴右
RECT SearchRect(int cx) {
  const int h = g_app.px(34);
  const int w = g_app.px(kSearchW);
  return RECT{cx - g_app.px(16) - w, (g_app.px(kCrumbH) - h) / 2, cx - g_app.px(16),
              (g_app.px(kCrumbH) - h) / 2 + h};
}

void Layout(int cx) {
  HDC dc = ::GetDC(g_bar);
  const int barH = g_app.px(kCrumbH);

  g_backRect = RECT{g_app.px(16), (barH - g_app.px(kBackBtnH)) / 2,
                    g_app.px(16) + g_app.px(kBackBtnW),
                    (barH - g_app.px(kBackBtnH)) / 2 + g_app.px(kBackBtnH)};

  int x = g_backRect.right + g_app.px(6);
  // 面包屑最多占到搜索框左侧，超出的节点不绘制也不可点
  const int limit = SearchRect(cx).left - g_app.px(8);

  for (size_t i = 0; i < g_nodes.size(); ++i) {
    Node& n = g_nodes[i];
    int w = TextWidth(dc, n.label, fonts.ui) + g_app.px(kNodePadX) * 2;
    if (n.home) w += g_app.px(kIconSm) + g_app.px(4);
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
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kPaneBg);

  // 编辑态：只画编辑框，面包屑与后退按钮都让位
  if (g_editing && !::IsRectEmpty(&g_editFrame)) {
    const int radius = g_app.px(7);
    FillRoundRect(dc, g_editFrame, kInputBg, radius);
    StrokeRoundRect(dc, g_editFrame, kInputFocusBorder, radius, 1);
    DrawHLine(dc, 0, rc.right, rc.bottom - 1, kLine);
    mem.Commit();
    ::ReleaseDC(hwnd, target);
    return;
  }

  // 后退按钮
  if (g_backHot) FillRoundRect(dc, g_backRect, kHoverBg, g_app.px(6));
  const bool canBack = AppCanBack();
  const int bs = g_app.px(18);
  RECT bi{g_backRect.left + (g_backRect.right - g_backRect.left - bs) / 2,
          g_backRect.top + (g_backRect.bottom - g_backRect.top - bs) / 2,
          g_backRect.left + (g_backRect.right - g_backRect.left - bs) / 2 + bs,
          g_backRect.top + (g_backRect.bottom - g_backRect.top - bs) / 2 + bs};
  DrawIcon(dc, IconId::kArrowBack, bi, canBack ? kInk : kTextFaint, canBack);

  for (size_t i = 0; i < g_nodes.size(); ++i) {
    const Node& n = g_nodes[i];
    if (n.rc.right > rc.right) break;
    const bool hot = (g_hot == static_cast<int>(i));

    if (n.current) {
      FillRoundRect(dc, n.rc, kCurrentBg, g_app.px(5));
    } else if (hot) {
      FillRoundRect(dc, n.rc, kLinkHover, g_app.px(5));
    }

    int textLeft = n.rc.left + g_app.px(kNodePadX);
    if (n.home) {
      const int is = g_app.px(kIconSm);
      RECT ir{textLeft, n.rc.top + (n.rc.bottom - n.rc.top - is) / 2, textLeft + is,
              n.rc.top + (n.rc.bottom - n.rc.top - is) / 2 + is};
      DrawIcon(dc, IconId::kHome, ir, n.current ? kInk : kLink);
      textLeft = ir.right + g_app.px(4);
    }
    RECT tr{textLeft, n.rc.top, n.rc.right - g_app.px(kNodePadX), n.rc.bottom};
    DrawTextLeft(dc, tr, n.label, n.current ? RGB(0x11, 0x11, 0x11) : kLink, fonts.ui);

    // 分隔箭头
    if (i + 1 < g_nodes.size()) {
      const int sepW = g_app.px(16) + g_app.px(kNodeGap) * 2;
      RECT sr{n.rc.right + g_app.px(kNodeGap), n.rc.top, n.rc.right + sepW, n.rc.bottom};
      if (sr.right > rc.right) break;
      const int cs = g_app.px(14);
      RECT cr{sr.left + (sr.right - sr.left - cs) / 2, sr.top + (sr.bottom - sr.top - cs) / 2,
              sr.left + (sr.right - sr.left - cs) / 2 + cs,
              sr.top + (sr.bottom - sr.top - cs) / 2 + cs};
      DrawIcon(dc, IconId::kChevron, cr, RGB(0x9c, 0xa3, 0xaf));
    }
  }

  DrawHLine(dc, 0, rc.right, rc.bottom - 1, kLine);
  mem.Commit();
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

LRESULT CALLBACK PathEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                              DWORD_PTR /*ref*/) {
  if (msg == WM_KEYDOWN) {
    if (wParam == VK_RETURN) {
      CommitEdit();
      ::SetFocus(MainList());
      return 0;
    }
    if (wParam == VK_ESCAPE) {
      HideEdit();
      ::SetFocus(MainList());
      return 0;
    }
  }
  if (msg == WM_CHAR && wParam == L'\r') return 0;
  if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, PathEditProc, id);
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// 编辑态：把输入框摆到面包屑位置，填入当前完整路径
void ShowEdit() {
  if (!g_edit) {
    // 不用 CLIENTEDGE：边框由面包屑栏自绘，保持圆角与配色一致
    g_edit = ::CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, g_bar,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)),
                               reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(g_bar, GWLP_HINSTANCE)),
                               nullptr);
    ::SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
    ::SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(g_app.px(8), g_app.px(8)));
    ::SetWindowSubclass(g_edit, PathEditProc, 1, 0);
  }
  std::wstring text;
  if (g_app.archiveMode) {
    text = g_app.archivePath;
    if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
  } else {
    text = g_app.curDir;
  }
  RepositionEdit();
  ::SetWindowTextW(g_edit, text.c_str());
  // 选中全部，直接输入即可替换（与 Explorer 的 Ctrl+L 一致）
  ::SendMessageW(g_edit, EM_SETSEL, 0, -1);
  ::ShowWindow(g_edit, SW_SHOW);
  ::SetFocus(g_edit);
  g_editing = true;
  // 自绘圆角边框在父窗口，必须让面包屑栏重画
  ::InvalidateRect(g_bar, nullptr, TRUE);
}

void HideEdit() {
  if (g_edit) ::ShowWindow(g_edit, SW_HIDE);
  g_editing = false;
  ::InvalidateRect(g_bar, nullptr, TRUE);
}

// 只重排编辑框，不改文本也不动光标（窗口缩放时用）
void RepositionEdit() {
  if (!g_edit) return;
  RECT cr;
  ::GetClientRect(g_bar, &cr);
  const int h = g_app.px(32);
  const int y = (g_app.px(kCrumbH) - h) / 2;
  const int right = SearchRect(cr.right).left - g_app.px(8);
  const int left = g_app.px(16);
  const int width = std::max(g_app.px(120), right - left);
  g_editFrame = RECT{left, y, left + width, y + h};

  // 单行 EDIT 把文字贴顶，所以让它只有一行高再居中；
  // 内缩 2px 留白，避免选中蓝底压住自绘圆角描边。
  HDC dc = ::GetDC(g_bar);
  int lineH = TextHeight(dc, fonts.ui);
  ::ReleaseDC(g_bar, dc);
  if (lineH <= 0) lineH = g_app.px(16);
  const int avail = h - g_app.px(8);
  const int eh = (lineH < avail) ? lineH : avail;
  const int insetX = g_app.px(10);
  const int ew = width - insetX * 2;
  ::MoveWindow(g_edit, left + insetX, y + (h - eh) / 2, ew, eh, TRUE);
  if (ew > g_app.px(2) && eh > g_app.px(2)) {
    ::SetWindowRgn(g_edit, ::CreateRoundRectRgn(0, 0, ew, eh, 0, 0), TRUE);
  }
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
  wchar_t buf[MAX_PATH * 2];
  ::GetWindowTextW(g_edit, buf, MAX_PATH * 2);
  const std::wstring target = buf;
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
      Layout(cr.right);
      if (g_editing) RepositionEdit();
      return 0;
    }

    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kPaneBg);
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

    case WM_COMMAND: {
      // 地址编辑框失去焦点视为放弃编辑（回车提交在 HandleCrumbKey 里）
      if (LOWORD(wParam) == kPathEditId && HIWORD(wParam) == EN_KILLFOCUS && g_editing) {
        HideEdit();
      }
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
    registered = RegisterWindowClass(kCrumbClass, CrumbProc, inst, CS_HREDRAW | CS_VREDRAW,
                                     nullptr);
  }
  g_bar = ::CreateWindowExW(0, kCrumbClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);

  g_search = CreateSearchBox(g_bar, inst, kSearchId, L"在压缩包内搜索文件或文件夹");
  SetSearchBoxChangeHandler(g_search, [] {
    SetFilterFromEdit(SearchBoxEdit(g_search));
  });
  return g_bar;
}

HWND CrumbSearchEdit() { return SearchBoxEdit(g_search); }

bool CrumbIsEditing() { return g_editing; }

void BeginCrumbEdit() {
  if (g_editing) {
    // 已在编辑：把焦点交回去，不要重填文本把用户输入冲掉
    if (g_edit) ::SetFocus(g_edit);
    return;
  }
  ShowEdit();
}

void RefreshCrumbBar() {
  if (!g_bar) return;
  BuildNodes();
  RECT cr;
  ::GetClientRect(g_bar, &cr);
  const RECT sr = SearchRect(cr.right);
  ::MoveWindow(g_search, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, TRUE);
  SetSearchBoxCue(g_search, g_app.archiveMode ? L"在压缩包内搜索文件或文件夹"
                                              : L"搜索文件或文件夹");
  Layout(cr.right);
  if (g_editing) RepositionEdit();
  ::InvalidateRect(g_bar, nullptr, FALSE);
}

bool HandleCrumbKey(MSG* msg) {
  if (!g_editing || !g_edit) return false;
  // 按键目标是地址编辑框就接管，不能依赖 GetFocus：主循环此刻可能还没同步焦点
  if (msg->hwnd != g_edit) return false;
  if (msg->message != WM_KEYDOWN) return false;

  if (msg->wParam == VK_RETURN) {
    CommitEdit();
    ::SetFocus(MainList());
    return true;
  }
  if (msg->wParam == VK_ESCAPE) {
    HideEdit();
    ::SetFocus(MainList());
    return true;
  }
  return false;
}

}  // namespace ui
