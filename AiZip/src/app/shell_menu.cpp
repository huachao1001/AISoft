// 右键菜单的注册入口：只描述用哪个组件、挂到哪些对象，落地交给 AiUI 后端。
//
// 为什么要组件：静态动词的文字在注册那一刻就定死了，做不出「压缩到 rnd.zip」。
// 动态文案只能由资源管理器进程内的处理器算出来，组件作为资源内嵌在 exe 里，
// 首次运行时由后端释放并注册，分发仍然只有一个文件。
#include "shell_menu.h"

#include "AiUI/platform.h"
#include "AiUI/shell.h"

namespace shell {
namespace {

// 组件的稳定标识。发布后不能再改：系统里的注册位置与组件标识都由它推出，
// 改了会让已安装用户的菜单失效。
constexpr const wchar_t* kHandlerId = L"AIZip";

// 组件标识。与 CMake 里 aiui_add_shell_menu_dll 的 CLSID 参数必须一致。
constexpr const wchar_t* kHandlerClsid = L"{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}";

// 组件永远与宿主 exe 同目录：安装版在程序目录，开发期在构建目录。
// 两者都可写，也不再需要任何标记文件来判断「是不是安装版」。
std::wstring componentDir() {
  const std::wstring exe = AiUI::platform()->executablePath();
  if (exe.empty()) return {};
  const size_t slash = exe.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
}

AiUI::ShellMenuHandler handler(AiUI::InstallScope scope) {
  AiUI::ShellMenuHandler h;
  h.id = kHandlerId;
  h.clsid = kHandlerClsid;
  h.componentName = L"aizip.dll";
  h.componentResourceId = 200;
  h.displayName = L"爱压 右键菜单";
  // 文件与目录都要挂：右键文件夹是最常见的压缩入口
  h.targets = {L"*", L"Directory"};
  h.scope = scope;
  h.scopeExplicit = true;
  h.componentDir = componentDir();
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

AiUI::InstallScope InstalledScope() {
  // 不额外存状态：注册表里哪一边挂着我们的菜单，就是哪一边装的。
  // 两边都没有（尚未装过）按当前用户处理。
  return AiUI::detectShellMenuScope(handler(AiUI::InstallScope::kCurrentUser));
}

namespace {

// 受支持的压缩包后缀：与 arc::Archive::IsArchiveFile 保持同一张表。
// 改一处必须同步另一处，否则「双击能打开」和「归我们处理」会对不上。
const wchar_t* const kAssociableExts[] = {
    L".zip", L".7z",  L".rar", L".tar", L".gz",  L".tgz",  L".bz2", L".tbz2",
    L".xz",  L".txz", L".cab", L".iso", L".wim", L".img",  L".jar", L".apk",
    L".aar", L".epub", L".lzh", L".arj", L".z",   L".001",
};

AiUI::FileAssociation association(AiUI::InstallScope scope) {
  AiUI::FileAssociation a;
  a.progId = L"AIZip.Archive";
  a.description = L"爱压 压缩包";
  for (const wchar_t* ext : kAssociableExts) a.extensions.emplace_back(ext);
  (void)scope;  // 范围由 AiUI::currentInstallScope() 决定
  return a;
}

}  // namespace

std::wstring RegisterFileAssociations() {
  return AiUI::registerFileAssociations(association(AiUI::currentInstallScope()));
}

bool AreFileAssociationsRegistered() {
  return AiUI::areFileAssociationsRegistered(association(AiUI::currentInstallScope()));
}

}  // namespace shell
