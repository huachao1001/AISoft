// 资源管理器/访达右键菜单注册
//
// 只列动作清单，落到注册表还是各平台自己的机制由 AiUI::shell 决定，
// 应用侧不碰系统 API。
#pragma once

#include <string>

namespace shell {

// 写入 / 撤销系统右键菜单。成功后返回空串，失败返回原因。
std::wstring RegisterContextMenu();
std::wstring UnregisterContextMenu();

// 菜单是否已注册且指向当前可执行文件
bool IsContextMenuRegistered();

}  // namespace shell
