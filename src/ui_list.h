// 文件列表：虚拟 ListView，支持排序、过滤、双击进入、右键菜单
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace ui {

HWND CreateFileList(HWND parent, HINSTANCE inst, int id);
void LayoutFileList(HWND list);
void RefreshFileList();
LRESULT HandleListNotify(HWND mainWnd, NMHDR* hdr);

// 当前选中项在 g_app.items 中的下标
std::vector<int> GetSelectedItems();
// 过滤后的可见行（g_app.items 的下标）
const std::vector<int>& VisibleRows();

// 搜索框内容变化时调用
void SetFilterFromEdit(HWND edit);

void DeleteItems(HWND mainWnd, const std::vector<int>& indices);

}  // namespace ui
