// 右键菜单的注册入口：只描述用哪个组件、挂到哪些对象，落地交给 AiUI 后端。
//
// 为什么要组件：静态动词的文字在注册那一刻就定死了，做不出「压缩到 rnd.zip」。
// 动态文案只能由资源管理器进程内的处理器算出来，组件作为资源内嵌在 exe 里，
// 首次运行时由后端释放并注册，分发仍然只有一个文件。
#include "shell_menu.h"

#include "AiUI/shell.h"

namespace shell {
namespace {

// 组件的稳定标识。发布后不能再改：系统里的注册位置与组件标识都由它推出，
// 改了会让已安装用户的菜单失效。
constexpr const wchar_t* kHandlerId = L"AIZip";

// 组件标识。与 CMake 里 aiui_add_shell_menu_dll 的 CLSID 参数必须一致。
constexpr const wchar_t* kHandlerClsid = L"{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}";

AiUI::ShellMenuHandler handler() {
  AiUI::ShellMenuHandler h;
  h.id = kHandlerId;
  h.clsid = kHandlerClsid;
  h.componentName = L"aicmd.dll";
  h.componentResourceId = 200;
  h.displayName = L"AIZip 右键菜单";
  // 文件与目录都要挂：右键文件夹是最常见的压缩入口
  h.targets = {L"*", L"Directory"};
  return h;
}

}  // namespace

std::wstring RegisterContextMenu() { return AiUI::registerShellMenuHandler(handler()); }

std::wstring UnregisterContextMenu() { return AiUI::unregisterShellMenuHandler(handler()); }

bool IsContextMenuRegistered() { return AiUI::isShellMenuHandlerRegistered(handler()); }

}  // namespace shell
