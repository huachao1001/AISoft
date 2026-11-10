// 系统图标：取自 Segoe Fluent Icons（Win11），缺失时回退 Segoe MDL2 Assets。
// 字形按墨迹框贴合目标矩形，任意尺寸都能画得居中且不越界。
#pragma once

#include <windows.h>

#include <string>

namespace AUI {

enum class IconId {
  kRefresh,
  kExtract,     // 提取选中
  kExtractAll,  // 全部解压
  kAdd,         // 添加文件
  kTrash,       // 删除
  kArrowBack,
  kChevron,     // 面包屑分隔
  kChevronUp,   // 表头升序
  kChevronDown, // 表头降序
  kHome,
  kFolder,
  kFile,
  kSearch,
};

// 画在 HDC 上，按墨迹框居中贴合 rc
void DrawGlyph(HDC dc, IconId id, const RECT& rc, COLORREF color, bool enabled = true);

// shell 文件图标：句柄缓存在本进程内，调用方不要销毁
HICON ShellIconFor(const std::wstring& path, bool isDir);
void ClearShellIconCache();

}  // namespace AUI
