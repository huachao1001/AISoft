#include "config_module.h"

#include "internal_util.h"

#include "AiInstaller/config_abi.h"
#include "AiUI/install.h"
#include "AiUI/platform.h"

#include <string>

namespace AiInstaller {
namespace detail {
namespace {

using ApplyFn = AiSetupConfigResult (*)(const AiSetupConfigRequest*);
using RemoveFn = AiSetupConfigResult (*)(const AiSetupConfigRequest*);
using ReadyFn = int (*)(const AiSetupConfigRequest*);
using VersionFn = int (*)(void);

// 一次加载的会话：构造时装载，析构时卸载。
//
// 每次调用都重新加载而不是缓存句柄：这些调用只发生在安装、卸载与启动校对，
// 频率极低；缓存反而要在程序生命周期里一直持有 dll，升级时会锁住文件。
class Session {
 public:
  Session(const std::wstring& installDir, const std::wstring& moduleName)
      : installDir_(installDir), moduleName_(moduleName) {
    const std::wstring path = joinPath(installDir, moduleName);
    std::wstring reason;
    if (!module_.load(path, reason)) {
      error_ = L"配置模块加载失败：" + reason;
      return;
    }

    apply_ = reinterpret_cast<ApplyFn>(module_.symbol("AiSetupConfigApply"));
    remove_ = reinterpret_cast<RemoveFn>(module_.symbol("AiSetupConfigRemove"));
    ready_ = reinterpret_cast<ReadyFn>(module_.symbol("AiSetupConfigIsReady"));
    version_ = reinterpret_cast<VersionFn>(module_.symbol("AiSetupConfigVersion"));

    if (!apply_ || !remove_ || !ready_ || !version_) {
      error_ = L"配置模块缺少标准导出：" + moduleName;
      return;
    }
    // 版本不符时宁可不用：语义已经变了，按旧理解调用可能写坏注册表
    if (version_() != AISETUP_CONFIG_API_VERSION) {
      error_ = L"配置模块接口版本不匹配：" + moduleName;
      return;
    }
  }

  bool usable() const { return error_.empty(); }
  const std::wstring& error() const { return error_; }

  // 参数里的指针指向本对象的成员，调用期间一直有效
  AiSetupConfigRequest makeRequest(const std::wstring& hostExePath, Scope scope) {
    AiSetupConfigRequest request{};
    request.apiVersion = AISETUP_CONFIG_API_VERSION;
    request.installDir = installDir_.c_str();
    request.hostExePath = hostExePath.c_str();
    request.flags = AISETUP_CONFIG_ALL;
    request.allUsers = scope == Scope::kAllUsers ? 1 : 0;
    return request;
  }

  AiSetupConfigResult apply(const AiSetupConfigRequest* request) { return apply_(request); }
  AiSetupConfigResult remove(const AiSetupConfigRequest* request) { return remove_(request); }
  int ready(const AiSetupConfigRequest* request) { return ready_(request); }

  AiUI::Module& module() { return module_; }

 private:
  std::wstring installDir_;
  std::wstring moduleName_;
  std::wstring error_;
  AiUI::Module module_;
  ApplyFn apply_ = nullptr;
  RemoveFn remove_ = nullptr;
  ReadyFn ready_ = nullptr;
  VersionFn version_ = nullptr;
};

std::wstring resultText(const AiSetupConfigResult& result) {
  return result.code == 0 ? std::wstring()
                          : std::wstring(result.message ? result.message : L"配置模块返回失败");
}

}  // namespace

bool isConfigModuleUsable(const std::wstring& installDir, const std::wstring& moduleName) {
  if (installDir.empty() || moduleName.empty()) return false;
  Session session(installDir, moduleName);
  return session.usable();
}

ConfigCallResult callConfigModule(const std::wstring& installDir, const std::wstring& hostExePath,
                                  const std::wstring& moduleName, Scope scope, bool apply) {
  ConfigCallResult out;

  // 没有配置模块是合法状态：这个应用就是纯解压安装
  if (moduleName.empty()) {
    out.ok = true;
    return out;
  }

  Session session(installDir, moduleName);
  if (!session.usable()) {
    out.ok = !apply;  // 卸载时模块缺失不算失败
    out.error = apply ? session.error() : std::wstring();
    return out;
  }

  const AiSetupConfigRequest request = session.makeRequest(hostExePath, scope);
  const std::wstring error = resultText(apply ? session.apply(&request) : session.remove(&request));

  out.ok = error.empty();
  out.error = error;
  return out;
}

bool queryConfigModule(const std::wstring& installDir, const std::wstring& hostExePath,
                       const std::wstring& moduleName, Scope scope) {
  if (moduleName.empty()) return true;

  Session session(installDir, moduleName);
  if (!session.usable()) return false;

  const AiSetupConfigRequest request = session.makeRequest(hostExePath, scope);
  return session.ready(&request) == 1;
}

}  // namespace detail
}  // namespace AiInstaller
