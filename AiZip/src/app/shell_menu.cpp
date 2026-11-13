// 右键菜单的注册入口：只描述用哪个组件、挂到哪些对象，落地交给 AiUI 后端。
//
// 为什么要组件：静态动词的文字在注册那一刻就定死了，做不出「压缩到 rnd.zip」。
// 动态文案只能由资源管理器进程内的处理器算出来，组件作为资源内嵌在 exe 里，
// 首次运行时由后端释放并注册，分发仍然只有一个文件。
#include "shell_menu.h"

#include "util.h"

#include "AiUI/shell.h"

#include "AiUI/platform.h"

namespace shell {
namespace {

// 组件的稳定标识。发布后不能再改：系统里的注册位置与组件标识都由它推出，
// 改了会让已安装用户的菜单失效。
constexpr const wchar_t* kHandlerId = L"AIZip";

// 组件标识。与 CMake 里 aiui_add_shell_menu_dll 的 CLSID 参数必须一致。
constexpr const wchar_t* kHandlerClsid = L"{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}";

// 已安装的版本把组件放在程序目录，卸载时能随程序一起清掉；
// 开发期（build 目录里跑）仍放用户数据目录，免得构建产物混进 dll。
// 判据是安装程序写下的标记文件，而不是目录名之类的猜测。
std::wstring installedComponentDir() {
  const std::wstring exe = AiUI::platform()->executablePath();
  if (exe.empty()) return {};

  const size_t slash = exe.find_last_of(L'\\');
  if (slash == std::wstring::npos) return {};
  const std::wstring dir = exe.substr(0, slash);

  // 标记文件由安装程序落盘，存在即视为安装版
  if (!util::PathExists(dir + L"\\installed.marker")) return {};
  return dir;
}

// 安装版的范围由安装程序决定并存进标记文件（如 "all-users"）。
// 程序自己猜不出来：同一份 exe 装在 Program Files 还是用户目录，
// 只有安装时知道。读不到就按当前用户处理。
AiUI::InstallScope installedScope() {
  const std::wstring exe = AiUI::platform()->executablePath();
  if (exe.empty()) return AiUI::InstallScope::kCurrentUser;

  const size_t slash = exe.find_last_of(L'\\');
  if (slash == std::wstring::npos) return AiUI::InstallScope::kCurrentUser;

  std::string text = AiUI::platform()->readFile(exe.substr(0, slash) + L"\\installed.marker", 256);
  if (text.empty()) return AiUI::InstallScope::kCurrentUser;
  return text.find("all-users") != std::string::npos ? AiUI::InstallScope::kAllUsers
                                                     : AiUI::InstallScope::kCurrentUser;
}

AiUI::ShellMenuHandler handler(AiUI::InstallScope scope) {
  AiUI::ShellMenuHandler h;
  h.id = kHandlerId;
  h.clsid = kHandlerClsid;
  h.componentName = L"aicmd.dll";
  h.componentResourceId = 200;
  h.displayName = L"AIZip 右键菜单";
  // 文件与目录都要挂：右键文件夹是最常见的压缩入口
  h.targets = {L"*", L"Directory"};
  h.scope = scope;
  h.scopeExplicit = true;
  h.componentDir = installedComponentDir();
  return h;
}

}  // namespace

std::wstring RegisterContextMenu(AiUI::InstallScope scope) {
  return AiUI::registerShellMenuHandler(handler(scope));
}

std::wstring UnregisterContextMenu(AiUI::InstallScope scope) {
  return AiUI::unregisterShellMenuHandler(handler(scope));
}

bool IsContextMenuRegistered(AiUI::InstallScope scope) {
  return AiUI::isShellMenuHandlerRegistered(handler(scope));
}

AiUI::InstallScope InstalledScope() { return installedScope(); }

}  // namespace shell
