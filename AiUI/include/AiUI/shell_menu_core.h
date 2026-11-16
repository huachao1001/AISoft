// 菜单逻辑模块的 C 契约
//
// 菜单逻辑与 COM 外壳分成两个 dll 时，两者的边界必须是纯 C：
//   - 两个 dll 各有一份静态 CRT，跨边界传 std::wstring / new / delete，
//     等于让一个堆释放另一个堆的内存，必然崩资源管理器；
//   - 外壳会被资源管理器长期持有，逻辑 dll 却要能随时被替换，边界不稳
//     就没法各自演进。
//
// 所以这里只用定长缓冲：路径进、菜单项出，谁分配谁释放。
//
// 逻辑 dll 导出两个函数（名字固定，不用 C++ 修饰）：
//   AiuiShellMenuCoreVersion 返回 kShellMenuCoreApiVersion
//   AiuiBuildShellMenuCore   按选择算出菜单项
#pragma once

#include <string>
#include <vector>

namespace AiUI {

// 接口版本。结构或语义不兼容变化时递增，外壳据此拒绝加载旧模块——
// 按旧理解解读新结构会读越界。
constexpr int kShellMenuCoreApiVersion = 1;

// 一次选择里的一项。path 由调用方持有，只在本次调用期间有效。
struct ShellMenuCoreSelection {
  const wchar_t* path = nullptr;
  int isDirectory = 0;
};

// 菜单项文字上限。带引号的文件名再长也不会超过 MAX_PATH 太多，320 足够，
// 且定长让布局完全不依赖 dll 之间的分配器约定。
constexpr int kShellMenuCoreLabelCapacity = 320;
constexpr int kShellMenuCoreArgsCapacity = 96;

// 一次调用最多几项。与组件里 QueryContextMenu 的可用区间一致。
constexpr int kShellMenuCoreMaxItems = 32;

struct ShellMenuCoreItem {
  wchar_t label[kShellMenuCoreLabelCapacity] = {};
  wchar_t arguments[kShellMenuCoreArgsCapacity] = {};
  int separatorAfter = 0;
};

// 导出的函数名
constexpr const char* kShellMenuCoreVersionSymbol = "AiuiShellMenuCoreVersion";
constexpr const char* kShellMenuCoreBuildSymbol = "AiuiBuildShellMenuCore";

}  // namespace AiUI
