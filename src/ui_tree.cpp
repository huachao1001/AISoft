// 左侧文件夹树：只在展开时枚举子目录，避免启动时扫描磁盘
#include "ui_tree.h"

#include "app.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <vector>

namespace ui {
namespace {

HWND g_tree = nullptr;
// 程序化选中节点期间置位，用于抑制 TVN_SELCHANGED 回灌。
// 否则“同步树 → 触发选中 → 再次导航”会互相打转。
bool g_syncing = false;

// 节点数据：完整路径
struct NodeData {
  std::wstring path;
};

NodeData* ItemData(HWND tree, HTREEITEM item) {
  TVITEMW tv{};
  tv.mask = TVIF_PARAM;
  tv.hItem = item;
  if (!::SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tv))) return nullptr;
  return reinterpret_cast<NodeData*>(tv.lParam);
}

// 在 parent 的直接子项里按完整路径查找
HTREEITEM FindChild(HWND tree, HTREEITEM parent, const std::wstring& path) {
  HTREEITEM child = reinterpret_cast<HTREEITEM>(
      ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
  while (child) {
    NodeData* d = ItemData(tree, child);
    if (d && ::CompareStringOrdinal(d->path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) {
      return child;
    }
    child = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
  }
  return nullptr;
}

void FreeNodeData(LPARAM lp) { delete reinterpret_cast<NodeData*>(lp); }

// 判断目录下是否有子目录，决定是否显示展开箭头
bool HasSubDirectory(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  do {
    if (fd.cFileName[0] == L'.') continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      found = true;
      break;
    }
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return found;
}

HTREEITEM InsertDirNode(HWND tree, HTREEITEM parent, const std::wstring& fullPath,
                        const std::wstring& label) {
  TVINSERTSTRUCTW ins{};
  ins.hParent = parent;
  ins.hInsertAfter = TVI_LAST;
  ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
  ins.item.pszText = const_cast<wchar_t*>(label.c_str());
  ins.item.lParam = reinterpret_cast<LPARAM>(new NodeData{fullPath});
  ins.item.cChildren = HasSubDirectory(fullPath) ? 1 : 0;
  ins.item.iImage = 0;
  ins.item.iSelectedImage = 1;
  return reinterpret_cast<HTREEITEM>(::SendMessageW(tree, TVM_INSERTITEMW, 0,
                                                    reinterpret_cast<LPARAM>(&ins)));
}

void AddChildren(HWND tree, HTREEITEM parent, const std::wstring& dir) {
  // 清掉占位子节点
  HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD,
                                                               reinterpret_cast<LPARAM>(parent)));
  while (child) {
    HTREEITEM next = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = child;
    ::SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    FreeNodeData(item.lParam);
    ::SendMessageW(tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(child));
    child = next;
  }

  std::vector<std::wstring> dirs;
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.cFileName[0] == L'.') continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.emplace_back(fd.cFileName);
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);

  std::sort(dirs.begin(), dirs.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            });

  for (const auto& name : dirs) {
    InsertDirNode(tree, parent, util::JoinPath(dir, name), name);
  }
}

}  // namespace

HWND CreateTreeView(HWND parent, HINSTANCE inst, int id) {
  HWND tree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_HASBUTTONS |
                                    TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                                0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                inst, nullptr);

  ::SendMessageW(tree, TVM_SETEXTENDEDSTYLE, 0, TVS_EX_DOUBLEBUFFER);

  HIMAGELIST il = ::ImageList_Create(g_app.px(16), g_app.px(16), ILC_COLOR32 | ILC_MASK, 2, 2);
  // 两个小图标：普通文件夹 / 打开文件夹
  for (int pass = 0; pass < 2; ++pass) {
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, g_app.px(16), g_app.px(16));
    HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem, bmp));
    RECT rc{0, 0, g_app.px(16), g_app.px(16)};
    FillRect(mem, rc, RGB(255, 255, 255));
    RECT iconRc{1, 1, g_app.px(15), g_app.px(15)};
    DrawIcon(mem, IconId::kFolder, iconRc);
    ::SelectObject(mem, old);
    ::ImageList_Add(il, bmp, nullptr);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
  }
  ::SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_NORMAL, reinterpret_cast<LPARAM>(il));

  g_tree = tree;
  return tree;
}

void PopulateTree(HWND tree) {
  if (!tree) return;
  ::SendMessageW(tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

  // 桌面
  wchar_t desktop[MAX_PATH];
  if (::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop) == S_OK) {
    InsertDirNode(tree, TVI_ROOT, desktop, L"桌面");
  }
  // 此电脑下的常用盘符
  const DWORD drives = ::GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1 << i))) continue;
    std::wstring root;
    root.push_back(static_cast<wchar_t>(L'A' + i));
    root += L":\\";
    const UINT type = ::GetDriveTypeW(root.c_str());
    if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
    // 可移动盘与光驱可能没插介质，无介质的盘符点了只会报错，直接跳过
    if ((type == DRIVE_REMOVABLE || type == DRIVE_CDROM) && !util::PathExists(root)) continue;
    std::wstring label = root;
    wchar_t volName[MAX_PATH] = {};
    if (::GetVolumeInformationW(root.c_str(), volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr,
                                0) &&
        volName[0]) {
      label = std::wstring(volName) + L" (" + root.substr(0, 2) + L")";
    }
    InsertDirNode(tree, TVI_ROOT, root, label);
  }

  // 用户目录
  wchar_t profile[MAX_PATH];
  if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) == S_OK) {
    InsertDirNode(tree, TVI_ROOT, profile, L"用户目录");
  }
}

void SyncTreeToCurrentDir() {
  if (!g_tree || g_app.archiveMode) return;
  // 逐级展开并选中当前目录。原来只匹配盘符根节点，选中根又触发 SELCHANGED
  // 把当前目录打回根，等于点一下就丢路径。
  const std::wstring target = g_app.curDir;
  if (target.size() < 3) return;
  const std::wstring root = target.substr(0, 3);

  HTREEITEM node = reinterpret_cast<HTREEITEM>(
      ::SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
  HTREEITEM current = nullptr;
  while (node) {
    NodeData* d = ItemData(g_tree, node);
    if (d && ::CompareStringOrdinal(d->path.c_str(), -1, root.c_str(), -1, TRUE) == CSTR_EQUAL) {
      current = node;
      break;
    }
    node = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(node)));
  }
  if (!current) return;

  // 从盘符往下按路径分量逐层展开；子节点在展开时才枚举，所以必须先展开再找
  size_t pos = root.size();
  while (pos < target.size()) {
    while (pos < target.size() && (target[pos] == L'\\' || target[pos] == L'/')) ++pos;
    if (pos >= target.size()) break;
    size_t end = target.find_first_of(L"\\/", pos);
    if (end == std::wstring::npos) end = target.size();
    const std::wstring prefix = target.substr(0, end);
    pos = end;

    if (!FindChild(g_tree, current, prefix)) {
      g_syncing = true;
      ::SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(current));
      g_syncing = false;
    }
    HTREEITEM next = FindChild(g_tree, current, prefix);
    if (!next) break;  // 树里没有该层（隐藏目录等），停在最深的已知节点
    current = next;
  }

  g_syncing = true;
  ::SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(current));
  ::SendMessageW(g_tree, TVM_ENSUREVISIBLE, 0, reinterpret_cast<LPARAM>(current));
  g_syncing = false;
}

LRESULT HandleTreeNotify(HWND mainWnd, NMHDR* hdr) {
  if (hdr->code == TVN_ITEMEXPANDINGW) {
    auto* exp = reinterpret_cast<NMTREEVIEWW*>(hdr);
    if (exp->action == TVE_EXPAND) {
      auto* data = reinterpret_cast<NodeData*>(exp->itemNew.lParam);
      if (data) AddChildren(g_tree, exp->itemNew.hItem, data->path);
    }
    return 0;
  }
  if (hdr->code == TVN_SELCHANGEDW) {
    if (g_syncing) return 0;  // 程序化同步产生的选中不回灌，避免打回父目录
    auto* sel = reinterpret_cast<NMTREEVIEWW*>(hdr);
    auto* data = reinterpret_cast<NodeData*>(sel->itemNew.lParam);
    if (data && !g_app.archiveMode) {
      if (::CompareStringOrdinal(data->path.c_str(), -1, g_app.curDir.c_str(), -1, TRUE) !=
          CSTR_EQUAL) {
        AppNavigateTo(data->path);
      }
    }
    return 0;
  }
  (void)mainWnd;
  return 0;
}

}  // namespace ui
