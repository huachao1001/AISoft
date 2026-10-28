// Windows 资源管理器右键菜单注册（HKCU\Software\Classes，无需管理员）
#pragma once

#include <string>

namespace shell {

// 写入 / 撤销资源管理器右键菜单。成功后返回空串，失败返回原因。
// 注册的是当前 exe 路径，程序挪位置后重新注册即可。
std::wstring RegisterContextMenu();
std::wstring UnregisterContextMenu();

// 菜单是否已注册且指向当前 exe（旧版本路径或删掉 exe 都算未注册）
bool IsContextMenuRegistered();

}  // namespace shell
