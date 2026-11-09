// 左侧文件夹树：自绘扁平列表 + 懒加载子目录
#include "ui_tree.h"

#include "app.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"

#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace ui {
namespace {

constexpr const wchar_t* kTreeClass = L"AIZipSideTree";

constexpr int kRowH = 30;      // 每行高度
constexpr int kIndent = 18;    // 每层缩进
constexpr int kPadL = 14;      // 树的内边距
constexpr int kIconSz = 16;
constexpr int kArrowW = 14;

struct Node {
  std::wstring label;
  std::wstring path;      // 文件系统完整路径，或包内相对路径
  int depth = 0;
  bool expanded = false;
  bool loaded = false;
  bool hasChildren = true;
  bool isRoot = false;    // 压缩包根 / 盘符 / 桌面：用 home 或目录图标
  IconId icon = IconId::kFolder;
  Node* parent = nullptr;
  std::vector<std::unique_ptr<Node>> children;
};

HWND g_tree = nullptr;
std::unique_ptr<Node> g_root;             // 虚拟根（不显示）
std::vector<Node*> g_rows;                // 扁平化后的可见行
int g_scroll = 0;
int g_hot = -1;
Node* g_active = nullptr;

int RowHeight() { return g_app.px(kRowH); }

bool IsArchiveRootPath(const std::wstring& p) { return p.empty(); }

// 枚举目录下的一级子目录
std::vector<std::wstring> SubDirs(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                                    (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
      continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    // 隐藏目录不进树：用户目录下大量 .xxx 会把可视区占满
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
    if (fd.cFileName[0] == L'.') continue;
    out.emplace_back(fd.cFileName);
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  std::sort(out.begin(), out.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            });
  return out;
}

Node* MakeNode(const std::wstring& label, const std::wstring& path, int depth, Node* parent,
               IconId icon) {
  auto n = std::make_unique<Node>();
  n->label = label;
  n->path = path;
  n->depth = depth;
  n->parent = parent;
  n->icon = icon;
  return n.release();
}

void LoadChildren(Node* n) {
  if (!n || n->loaded) return;
  n->loaded = true;
  n->children.clear();

  if (g_app.archiveMode) {
    // 包内：从条目里收集当前层的直接子目录
    const std::wstring prefix = n->path.empty() ? L"" : n->path + L"\\";
    std::vector<std::wstring> names;
    for (const auto& e : g_app.archive.Entries()) {
      if (e.path.size() <= prefix.size()) continue;
      if (!prefix.empty() &&
          ::CompareStringOrdinal(e.path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                 static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
        continue;
      }
      const std::wstring rel = e.path.substr(prefix.size());
      const size_t slash = rel.find_first_of(L"\\/");
      // 只有还带子路径的条目（或其本身是目录）才算树节点，文件不进树
      if (slash == std::wstring::npos && !e.isDir) continue;
      const std::wstring name = (slash == std::wstring::npos) ? rel : rel.substr(0, slash);
      if (name.empty()) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) {
                return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
              });
    for (const auto& name : names) {
      const std::wstring inner = n->path.empty() ? name : n->path + L"\\" + name;
      n->children.emplace_back(MakeNode(name, inner, n->depth + 1, n, IconId::kFolder));
    }
    n->hasChildren = !n->children.empty();
    return;
  }

  // 文件系统
  const auto dirs = SubDirs(n->path);
  for (const auto& d : dirs) {
    n->children.emplace_back(
        MakeNode(d, util::JoinPath(n->path, d), n->depth + 1, n, IconId::kFolder));
  }
  n->hasChildren = !n->children.empty();
}

void Flatten(Node* n) {
  for (auto& c : n->children) {
    g_rows.push_back(c.get());
    if (c->expanded) Flatten(c.get());
  }
}

void Rebuild() {
  g_rows.clear();
  if (g_root) Flatten(g_root.get());
}

// 建根：压缩包模式只放包本身；文件系统放桌面、各盘符、用户目录
void BuildRoot() {
  auto root = std::make_unique<Node>();
  root->label = L"";
  root->depth = -1;
  root->loaded = true;
  root->expanded = true;

  if (g_app.archiveMode) {
    Node* n = MakeNode(util::BaseName(g_app.archivePath) + L"（根目录）", L"", 0, root.get(),
                       IconId::kHome);
    n->isRoot = true;
    root->children.emplace_back(n);
  } else {
    wchar_t desktop[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop) == S_OK) {
      Node* n = MakeNode(L"桌面", desktop, 0, root.get(), IconId::kHome);
      n->isRoot = true;
      root->children.emplace_back(n);
    }
    const DWORD drives = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (!(drives & (1u << i))) continue;
      std::wstring path;
      path.push_back(static_cast<wchar_t>(L'A' + i));
      path += L":\\";
      const UINT type = ::GetDriveTypeW(path.c_str());
      if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
      if ((type == DRIVE_REMOVABLE || type == DRIVE_CDROM) && !util::PathExists(path)) continue;
      std::wstring label = path.substr(0, 2);
      wchar_t vol[MAX_PATH] = {};
      if (::GetVolumeInformationW(path.c_str(), vol, MAX_PATH, nullptr, nullptr, nullptr, nullptr,
                                  0) &&
          vol[0]) {
        label = std::wstring(vol) + L" (" + path.substr(0, 2) + L")";
      }
      Node* n = MakeNode(label, path, 0, root.get(), IconId::kFolder);
      n->isRoot = true;
      root->children.emplace_back(n);
    }
    wchar_t profile[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) == S_OK) {
      Node* n = MakeNode(L"用户目录", profile, 0, root.get(), IconId::kFolder);
      n->isRoot = true;
      root->children.emplace_back(n);
    }
  }
  g_root = std::move(root);
}

// 展开到当前目录并选中
// p 是 target 本身或它的祖先目录（按路径分隔符边界判断，避免 chaohua2 误配 chaohua）
bool IsAncestorOrSelf(const std::wstring& p, const std::wstring& target) {
  if (p.empty()) return true;
  if (target.size() < p.size()) return false;
  if (::CompareStringOrdinal(target.c_str(), static_cast<int>(p.size()), p.c_str(),
                             static_cast<int>(p.size()), TRUE) != CSTR_EQUAL) {
    return false;
  }
  if (target.size() == p.size()) return true;
  if (p.back() == L'\\' || p.back() == L'/') return true;
  return target[p.size()] == L'\\' || target[p.size()] == L'/';
}

Node* SyncTo(Node* n, const std::wstring& target) {
  if (!n) return nullptr;
  if (!IsAncestorOrSelf(n->path, target)) return nullptr;

  LoadChildren(n);
  n->expanded = true;
  // 继续往深处找更贴近 target 的子节点
  for (auto& c : n->children) {
    Node* deeper = SyncTo(c.get(), target);
    if (deeper) return deeper;
  }
  return n;
}

void SetActive(const std::wstring& target) {
  g_active = nullptr;
  if (!g_root) return;
  for (auto& c : g_root->children) {
    Node* found = SyncTo(c.get(), target);
    if (found) {
      g_active = found;
      break;
    }
  }
  if (g_active) {
    // 滚到可见
    const int row = static_cast<int>(std::find(g_rows.begin(), g_rows.end(), g_active) -
                                     g_rows.begin());
    (void)row;
  }
}

RECT RowRect(int index, int cx) {
  const int h = RowHeight();
  const int top = index * h - g_scroll;
  return RECT{g_app.px(kPadL) - g_app.px(4), top, cx - g_app.px(kPadL) + g_app.px(4),
              top + h};
}

void ScrollToActive() {
  if (!g_active) return;
  const auto it = std::find(g_rows.begin(), g_rows.end(), g_active);
  if (it == g_rows.end()) return;
  const int index = static_cast<int>(it - g_rows.begin());
  RECT rc;
  ::GetClientRect(g_tree, &rc);
  const int visible = rc.bottom / RowHeight();
  if (visible <= 0) return;
  if (index * RowHeight() < g_scroll) {
    g_scroll = index * RowHeight();
  } else if ((index + 1) * RowHeight() > g_scroll + rc.bottom) {
    g_scroll = (index + 1) * RowHeight() - rc.bottom;
  }
  if (g_scroll < 0) g_scroll = 0;
}

void UpdateScrollBar() {
  if (!g_tree) return;
  RECT rc;
  ::GetClientRect(g_tree, &rc);
  const int total = static_cast<int>(g_rows.size()) * RowHeight();
  SCROLLINFO si{sizeof(si)};
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = total > 0 ? total - 1 : 0;
  si.nPage = rc.bottom;
  si.nPos = g_scroll;
  ::SetScrollInfo(g_tree, SB_VERT, &si, TRUE);
}

void PaintTree(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  MemDC mem(target, rc);
  HDC dc = mem.dc();
  ::SetBkMode(dc, TRANSPARENT);
  FillRect(dc, rc, kPaneBg);

  for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) {
    Node* n = g_rows[i];
    RECT rr = RowRect(i, rc.right);
    if (rr.bottom < 0) continue;
    if (rr.top > rc.bottom) break;

    const bool active = (n == g_active);
    const bool hot = (g_hot == i);
    RECT pill = rr;
    pill.left += g_app.px(6);
    pill.right -= g_app.px(6);
    ::InflateRect(&pill, 0, -g_app.px(2));
    if (active) {
      FillRoundRect(dc, pill, kSelBg, g_app.px(5));
    } else if (hot) {
      FillRoundRect(dc, pill, kHoverBg, g_app.px(5));
    }

    int x = g_app.px(kPadL) + n->depth * g_app.px(kIndent);
    const int cy = (rr.top + rr.bottom) / 2;

    // 展开箭头
    if (n->hasChildren) {
      const int as = g_app.px(12);
      RECT ar{x, cy - as / 2, x + as, cy + as / 2};
      // 用 chevron 旋转：展开态画向下（这里直接画三角更省事）
      POINT pts[3];
      if (n->expanded) {
        pts[0] = POINT{ar.left + g_app.px(2), cy - g_app.px(3)};
        pts[1] = POINT{ar.right - g_app.px(2), cy - g_app.px(3)};
        pts[2] = POINT{(ar.left + ar.right) / 2, cy + g_app.px(3)};
      } else {
        pts[0] = POINT{ar.left + g_app.px(4), cy - g_app.px(5)};
        pts[1] = POINT{ar.left + g_app.px(4), cy + g_app.px(5)};
        pts[2] = POINT{ar.right - g_app.px(2), cy};
      }
      HBRUSH b = ::CreateSolidBrush(kTextMuted);
      HPEN p = ::CreatePen(PS_SOLID, 1, kTextMuted);
      HBRUSH ob = static_cast<HBRUSH>(::SelectObject(dc, b));
      HPEN op = static_cast<HPEN>(::SelectObject(dc, p));
      ::Polygon(dc, pts, 3);
      ::SelectObject(dc, ob);
      ::SelectObject(dc, op);
      ::DeleteObject(b);
      ::DeleteObject(p);
    }
    x += g_app.px(kArrowW);

    // 图标
    const int is = g_app.px(kIconSz);
    RECT ir{x, cy - is / 2, x + is, cy + is / 2};
    DrawIcon(dc, n->icon, ir, active ? RGB(0x25, 0x63, 0xeb) : RGB(0x6b, 0x72, 0x80));
    x = ir.right + g_app.px(6);

    RECT tr{x, rr.top, rr.right - g_app.px(8), rr.bottom};
    DrawTextEllipsis(dc, tr, n->label, active ? kActiveInk : kText, fonts.ui);
  }

  // 右边框
  DrawVLine(dc, 0, rc.bottom, rc.right - 1, kLine);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

int HitRow(POINT pt) {
  if (pt.x < 0) return -1;
  const int idx = (pt.y + g_scroll) / RowHeight();
  if (idx < 0 || idx >= static_cast<int>(g_rows.size())) return -1;
  return idx;
}

// 箭头热区：行左侧 14px
bool HitArrow(POINT pt, Node* n) {
  if (!n || !n->hasChildren) return false;
  const int left = g_app.px(kPadL) + n->depth * g_app.px(kIndent);
  return pt.x >= left && pt.x < left + g_app.px(kArrowW);
}

void ActivateNode(Node* n) {
  if (!n) return;
  g_active = n;
  if (g_app.archiveMode) {
    if (::CompareStringOrdinal(n->path.c_str(), -1, g_app.innerPath.c_str(), -1, TRUE) !=
        CSTR_EQUAL) {
      g_app.innerPath = n->path;
      AppRefreshView();
      return;
    }
  } else if (!n->path.empty() &&
             ::CompareStringOrdinal(n->path.c_str(), -1, g_app.curDir.c_str(), -1, TRUE) !=
                 CSTR_EQUAL) {
    AppNavigateTo(n->path);
    return;
  }
  ::InvalidateRect(g_tree, nullptr, FALSE);
}

}  // namespace

LRESULT CALLBACK TreeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SIZE:
      UpdateScrollBar();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

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
      PaintTree(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitRow(pt);
      if (hit != g_hot) {
        g_hot = hit;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      g_hot = -1;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitRow(pt);
      if (hit < 0) return 0;
      Node* n = g_rows[hit];
      if (HitArrow(pt, n)) {
        n->expanded = !n->expanded;
        if (n->expanded) LoadChildren(n);
        Rebuild();
        UpdateScrollBar();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      ActivateNode(n);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      const int total = static_cast<int>(g_rows.size()) * RowHeight();
      const int maxScroll = (std::max)(0, total - static_cast<int>(rc.bottom));
      g_scroll -= GET_WHEEL_DELTA_WPARAM(wParam) * RowHeight() / WHEEL_DELTA * 3;
      if (g_scroll < 0) g_scroll = 0;
      if (g_scroll > maxScroll) g_scroll = maxScroll;
      UpdateScrollBar();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_VSCROLL: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      const int total = static_cast<int>(g_rows.size()) * RowHeight();
      const int maxScroll = (std::max)(0, total - static_cast<int>(rc.bottom));
      SCROLLINFO si{sizeof(si)};
      si.fMask = SIF_ALL;
      ::GetScrollInfo(hwnd, SB_VERT, &si);
      int pos = g_scroll;
      switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= RowHeight(); break;
        case SB_LINEDOWN: pos += RowHeight(); break;
        case SB_PAGEUP: pos -= rc.bottom; break;
        case SB_PAGEDOWN: pos += rc.bottom; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
      }
      if (pos < 0) pos = 0;
      if (pos > maxScroll) pos = maxScroll;
      g_scroll = pos;
      UpdateScrollBar();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
      return TRUE;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateSideTree(HWND parent, HINSTANCE inst, int id) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kTreeClass, TreeProc, inst, CS_HREDRAW | CS_VREDRAW, nullptr);
  }
  g_tree = ::CreateWindowExW(0, kTreeClass, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  return g_tree;
}

void RefreshSideTree() {
  if (!g_tree) return;
  BuildRoot();
  Rebuild();
  SetActive(g_app.archiveMode ? g_app.innerPath : g_app.curDir);
  Rebuild();
  ScrollToActive();
  UpdateScrollBar();
  ::InvalidateRect(g_tree, nullptr, FALSE);
}

void LayoutSideTree() {
  if (!g_tree) return;
  UpdateScrollBar();
  ::InvalidateRect(g_tree, nullptr, FALSE);
}

}  // namespace ui
