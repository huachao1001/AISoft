// 安装范围的探测
//
// 只用 AiUI 的中性接口问「我们的菜单现在挂在哪个根键」。菜单本身的注册、
// 组件释放都在 aizip_config.dll 里，本文件不参与那件事。
#include "shell_menu.h"

#include "AiUI/platform.h"
#include "AiUI/shell.h"

namespace shell {
namespace {

// 与 aizip_config.cpp、CMake 里的 aiui_add_shell_menu_dll 必须一致。
// 发布后不能再改：系统里的注册位置与组件标识都由它推出。
constexpr const wchar_t* kHandlerId = L"AIZip";
constexpr const wchar_t* kHandlerClsid = L"{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}";

// 组件永远与宿主 exe 同目录：安装版在程序目录，开发期在构建目录。
std::wstring componentDir() {
  const std::wstring exe = AiUI::platform()->executablePath();
  if (exe.empty()) return {};
  const size_t slash = exe.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
}

AiUI::ShellMenuHandler probe() {
  AiUI::ShellMenuHandler h;
  h.id = kHandlerId;
  h.clsid = kHandlerClsid;
  h.componentName = L"aizip.dll";
  h.displayName = L"爱压 右键菜单";
  h.targets = {L"*", L"Directory"};
  h.componentDir = componentDir();
  return h;
}

}  // namespace

AiUI::InstallScope InstalledScope() {
  // 不额外存状态：注册表里哪一边挂着我们的菜单，就是哪一边装的。
  // 两边都没有（尚未装过）按当前用户处理。
  return AiUI::detectShellMenuScope(probe());
}

}  // namespace shell
