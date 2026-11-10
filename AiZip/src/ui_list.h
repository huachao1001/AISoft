// 文件列表：数据与交互编排，控件由 AUI::ListView 提供
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ui {

HWND CreateFileList(HWND parent, HINSTANCE inst, int id);
void LayoutFileList(HWND list);
void RefreshFileList();

// 当前选中项在 g_app.items 中的下标
std::vector<int> GetSelectedItems();
// 过滤后的可见行（g_app.items 的下标）
const std::vector<int>& VisibleRows();
// 选中项能否解压：包内模式任意选中项可提取；文件系统下只允许单个受支持的压缩包
// 右侧没有选中项时，退回左侧树的当前节点判断
bool CanExtractSelected();
// 解压入口：右侧有选中项按选中项，否则用左侧树当前节点
void ExtractSelection(HWND owner);

void SetFilterText(const std::wstring& text);

void DeleteItems(HWND mainWnd, const std::vector<int>& indices);

}  // namespace ui
