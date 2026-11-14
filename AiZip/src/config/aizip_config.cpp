// 爱压「系统配置」能力的实现
//
// 只做两件事，都通过 AiUI 的中性接口完成，不出现注册表、COM、HWND：
//   - 右键菜单：释放内嵌组件并注册；
//   - 文件关联：把本程序登记为压缩包后缀的候选打开方式。
//
// 导出的是 AiInstaller 的标准配置 ABI：通用安装器只认那四个函数名，
// 因此它不需要认识爱压；主程序自愈调的也是同一份实现。
#include "aizip_config.h"

#include "AiUI/shell.h"

#include <string>
#include <vector>

namespace {

// ── 与主程序共享的稳定标识 ──
// 与 AiZip/src/app/shell_menu_model.cpp 保持一致。发布后不能再改：系统里的
// 注册位置与组件标识都由它推出，改了会让已安装用户的菜单失效。
constexpr const wchar_t* kHandlerId = L"AIZip";
constexpr const wchar_t* kHandlerClsid = L"{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}";
constexpr const wchar_t* kProgId = L"AIZip.Archive";
constexpr const wchar_t* kMenuDisplayName = L"爱压 右键菜单";
constexpr const wchar_t* kAssociationDescription = L"爱压 压缩包";

// 组件资源 id：由 CMake 把 aizip.dll 作为 RCDATA 打进本模块。
constexpr int kComponentResourceId = 200;

// 受支持的压缩包后缀：与 arc::Archive::IsArchiveFile、shell_menu_model.cpp
// 保持同一张表，三处必须同步。
const wchar_t* const kAssociableExts[] = {
    L".zip", L".7z",  L".rar", L".tar", L".gz",  L".tgz",  L".bz2", L".tbz2",
    L".xz",  L".txz", L".cab", L".iso", L".wim", L".img",  L".jar", L".apk",
    L".aar", L".epub", L".lzh", L".arj", L".z",   L".001",
};

// 结果缓冲：返回给调用方的 message 必须活到下次调用，不能是栈上的字符串
std::wstring g_message;

AiZipConfigResult okResult() {
  g_message.clear();
  return AiZipConfigResult{0, g_message.c_str()};
}

AiZipConfigResult failResult(const std::wstring& message) {
  g_message = message;
  return AiZipConfigResult{1, g_message.c_str()};
}

// 请求校验：必填项缺失时说清楚缺哪个，别让调用方去猜
bool validate(const AiZipConfigRequest* request, std::wstring& error) {
  if (!request) {
    error = L"请求为空";
    return false;
  }
  if (request->apiVersion != AIZIP_CONFIG_API_VERSION) {
    error = L"接口版本不匹配";
    return false;
  }
  if (!request->installDir || !*request->installDir) {
    error = L"缺少安装目录";
    return false;
  }
  if (!request->hostExePath || !*request->hostExePath) {
    error = L"缺少主程序路径";
    return false;
  }
  return true;
}

AiUI::InstallScope scopeOf(const AiZipConfigRequest& request) {
  return request.allUsers ? AiUI::InstallScope::kAllUsers : AiUI::InstallScope::kCurrentUser;
}

// 文件关联的后端签名不带 scope，内部读的是当前进程范围。这里临时切换，
// 出作用域自动还原，避免把「安装范围」泄漏给调用方进程的其余逻辑。
class ScopeGuard {
 public:
  explicit ScopeGuard(AiUI::InstallScope scope) : previous_(AiUI::currentInstallScope()) {
    AiUI::setCurrentInstallScope(scope);
  }
  ~ScopeGuard() { AiUI::setCurrentInstallScope(previous_); }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

 private:
  AiUI::InstallScope previous_;
};

// 右键组件描述。componentDir 显式给安装目录：组件跟主程序放一起，
// 卸载时随目录一并清掉，不留残渣在用户数据目录。
AiUI::ShellMenuHandler menuHandler(const AiZipConfigRequest& request) {
  AiUI::ShellMenuHandler handler;
  handler.id = kHandlerId;
  handler.clsid = kHandlerClsid;
  handler.componentName = L"aizip.dll";
  handler.componentResourceId = kComponentResourceId;
  handler.displayName = kMenuDisplayName;
  handler.targets = {L"*", L"Directory"};
  handler.scope = scopeOf(request);
  handler.scopeExplicit = true;
  handler.componentDir = request.installDir;
  handler.hostExePath = request.hostExePath;
  return handler;
}

AiUI::FileAssociation fileAssociation(const AiZipConfigRequest& request) {
  AiUI::FileAssociation association;
  association.progId = kProgId;
  association.description = kAssociationDescription;
  association.hostExePath = request.hostExePath;
  for (const wchar_t* ext : kAssociableExts) association.extensions.emplace_back(ext);
  return association;
}

// 统一执行入口：apply 为 true 时写入，false 时撤销
std::wstring runOne(const AiZipConfigRequest& request, unsigned flag, bool apply) {
  if (flag == kAiZipConfigShellMenu) {
    const AiUI::ShellMenuHandler handler = menuHandler(request);
    return apply ? AiUI::registerShellMenuHandler(handler)
                 : AiUI::unregisterShellMenuHandler(handler);
  }
  if (flag == kAiZipConfigFileAssociations) {
    const AiUI::FileAssociation association = fileAssociation(request);
    const ScopeGuard guard(scopeOf(request));
    return apply ? AiUI::registerFileAssociations(association)
                 : AiUI::unregisterFileAssociations(association);
  }
  return {};
}

// 逐项执行并汇总第一处失败：全部做完再报，避免一项失败把后面的也跳过
std::wstring runAll(const AiZipConfigRequest& request, unsigned flags, bool apply) {
  std::wstring firstError;
  for (const unsigned flag : {kAiZipConfigShellMenu, kAiZipConfigFileAssociations}) {
    if ((flags & flag) == 0) continue;
    const std::wstring error = runOne(request, flag, apply);
    if (!error.empty() && firstError.empty()) firstError = error;
  }
  return firstError;
}

// 查询只读：只回答能从系统直接读出来的项
bool readyOne(const AiZipConfigRequest& request, unsigned flag) {
  if (flag == kAiZipConfigShellMenu) {
    return AiUI::isShellMenuHandlerRegistered(menuHandler(request));
  }
  if (flag == kAiZipConfigFileAssociations) {
    const ScopeGuard guard(scopeOf(request));
    return AiUI::areFileAssociationsRegistered(fileAssociation(request));
  }
  return true;
}

}  // namespace

extern "C" {

int AiSetupConfigVersion(void) { return AIZIP_CONFIG_API_VERSION; }

AiSetupConfigResult AiSetupConfigApply(const AiSetupConfigRequest* request) {
  std::wstring error;
  if (!validate(request, error)) return failResult(error);

  const unsigned flags = request->flags == 0 ? kAiZipConfigAll : request->flags;
  const std::wstring failure = runAll(*request, flags, true);
  return failure.empty() ? okResult() : failResult(failure);
}

AiSetupConfigResult AiSetupConfigRemove(const AiSetupConfigRequest* request) {
  std::wstring error;
  if (!validate(request, error)) return failResult(error);

  const unsigned flags = request->flags == 0 ? kAiZipConfigAll : request->flags;
  const std::wstring failure = runAll(*request, flags, false);
  return failure.empty() ? okResult() : failResult(failure);
}

int AiSetupConfigIsReady(const AiSetupConfigRequest* request) {
  std::wstring error;
  if (!validate(request, error)) return -1;

  const unsigned flags = request->flags == 0 ? kAiZipConfigAll : request->flags;
  for (const unsigned flag : {kAiZipConfigShellMenu, kAiZipConfigFileAssociations}) {
    if ((flags & flag) == 0) continue;
    if (!readyOne(*request, flag)) return 0;
  }
  return 1;
}

}  // extern "C"
