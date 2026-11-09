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
// 选中项能否解压：包内模式任意选中项可提取；文件系统下只允许单个受支持的压缩包
// 右侧没有选中项时，退回左侧树的当前节点判断
bool CanExtractSelected();
// 解压入口：右侧有选中项按选中项，否则用左侧树当前节点
void ExtractSelection(HWND owner);

void SetFilterFromEdit(HWND edit);
// 悬停行高亮由主窗口转发的鼠标位置驱动
void SetListHotRow(int row);

void DeleteItems(HWND mainWnd, const std::vector<int>& indices);

}  // namespace ui
