// 资源管理器右键菜单的注册状态
//
// 组件形态、注册位置、释放流程都在 AiUI 的后端里（src/os/<platform>/），
// 这里只描述「用哪个组件、挂到哪些对象上」。应用不碰任何系统 API。
#pragma once

#include <string>

namespace shell {

// 写入 / 撤销系统右键菜单。成功后返回空串，失败返回原因。
std::wstring RegisterContextMenu();
std::wstring UnregisterContextMenu();

// 菜单是否已注册且指向当前可执行文件
bool IsContextMenuRegistered();

}  // namespace shell
