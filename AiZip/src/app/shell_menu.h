// 资源管理器右键菜单的注册状态
//
// 组件形态、注册位置、释放流程都在 AiUI 的后端里（src/os/<platform>/），
// 这里只描述「用哪个组件、挂到哪些对象上」。应用不碰任何系统 API。
#pragma once

#include <string>

#include "AiUI/shell.h"

namespace shell {

// 写入 / 撤销系统右键菜单。成功后返回空串，失败返回原因。
// scope 决定写 HKCU 还是 HKLM；默认跟随后端的当前范围。
std::wstring RegisterContextMenu(AiUI::InstallScope scope = AiUI::currentInstallScope());
std::wstring UnregisterContextMenu(AiUI::InstallScope scope = AiUI::currentInstallScope());

// 菜单是否已注册且指向当前可执行文件
bool IsContextMenuRegistered(AiUI::InstallScope scope = AiUI::currentInstallScope());

// 安装时选定的范围（安装版从标记文件读，开发期为当前用户）。
// 程序每次启动都按这个范围校对菜单，避免重复注册到另一个根键。
AiUI::InstallScope InstalledScope();

}  // namespace shell
