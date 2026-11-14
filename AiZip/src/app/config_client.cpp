// 配置模块的调用实现
//
// 只做三件事：找到同目录的 aizip_config.dll、校验接口版本、把调用转发过去。
// 具体写注册表、释放组件全在 dll 里，主程序不碰任何系统 API。
//
// 这里用的是 AiInstaller 的标准配置 ABI，与安装器调用的是同一份契约。
#include "config_client.h"

#include "AiUI/install.h"
#include "AiUI/platform.h"

#include <string>

namespace config_client {
namespace {

// 配置模块的文件名。与安装器、CMake 目标名保持一致。
constexpr const wchar_t* kModuleName = L"aizip_config.dll";

using ApplyFn = AiSetupConfigResult (*)(const AiSetupConfigRequest*);
using RemoveFn = AiSetupConfigResult (*)(const AiSetupConfigRequest*);
using ReadyFn = int (*)(const AiSetupConfigRequest*);
using VersionFn = int (*)(void);

// 一次加载的会话：构造时装载，析构时卸载。
//
// 每次调用都重新加载而不是缓存句柄：这些调用只发生在启动校对与修复，
// 频率极低；缓存反而要在程序生命周期里一直持有 dll，升级时会锁住文件。
class Session {
 public:
  explicit Session(const std::wstring& installDir) : installDir_(installDir) {
    const std::wstring path = installDir + L"\\" + kModuleName;
    if (!module_.load(path, reason_)) return;

    apply_ = reinterpret_cast<ApplyFn>(module_.symbol("AiSetupConfigApply"));
    remove_ = reinterpret_cast<RemoveFn>(module_.symbol("AiSetupConfigRemove"));
    ready_ = reinterpret_cast<ReadyFn>(module_.symbol("AiSetupConfigIsReady"));
    version_ = reinterpret_cast<VersionFn>(module_.symbol("AiSetupConfigVersion"));
  }

  bool usable() const {
    if (!apply_ || !remove_ || !ready_ || !version_) {
      return false;
    }
    // 版本不符时宁可不用：语义已经变了，按旧理解调用可能写坏注册表
    return version_() == AISETUP_CONFIG_API_VERSION;
  }

  const std::wstring& reason() const { return reason_; }
  ApplyFn apply() const { return apply_; }
  RemoveFn remove() const { return remove_; }
  ReadyFn ready() const { return ready_; }

 private:
  std::wstring installDir_;
  AiUI::Module module_;
  std::wstring reason_;
  ApplyFn apply_ = nullptr;
  RemoveFn remove_ = nullptr;
  ReadyFn ready_ = nullptr;
  VersionFn version_ = nullptr;
};

// 把调用参数装进 C 结构。字符串指针指向调用方传入的 wstring，调用期间有效。
AiSetupConfigRequest makeRequest(const std::wstring& installDir,
                                 const std::wstring& hostExePath, bool allUsers,
                                 unsigned flags) {
  AiSetupConfigRequest request{};
  request.apiVersion = AISETUP_CONFIG_API_VERSION;
  request.installDir = installDir.c_str();
  request.hostExePath = hostExePath.c_str();
  request.flags = flags;
  request.allUsers = allUsers ? 1 : 0;
  return request;
}

std::wstring resultText(const AiSetupConfigResult& result) {
  return result.code == 0 ? std::wstring()
                          : std::wstring(result.message ? result.message : L"");
}

}  // namespace

int apiVersion() { return AISETUP_CONFIG_API_VERSION; }

bool isReady(const std::wstring& installDir, const std::wstring& hostExePath, bool allUsers) {
  Session session(installDir);
  if (!session.usable()) return false;
  const AiSetupConfigRequest request = makeRequest(installDir, hostExePath, allUsers, 0);
  return session.ready()(&request) == 1;
}

std::wstring apply(const std::wstring& installDir, const std::wstring& hostExePath, bool allUsers,
                   unsigned flags) {
  Session session(installDir);
  if (!session.usable()) {
    // 说清是哪一步失败：缺文件、缺导出、版本不符，排查时差别很大
    return L"配置模块不可用：" + session.reason();
  }
  const AiSetupConfigRequest request = makeRequest(installDir, hostExePath, allUsers, flags);
  return resultText(session.apply()(&request));
}

std::wstring remove(const std::wstring& installDir, const std::wstring& hostExePath, bool allUsers,
                    unsigned flags) {
  Session session(installDir);
  if (!session.usable()) {
    return L"配置模块不可用：" + session.reason();
  }
  const AiSetupConfigRequest request = makeRequest(installDir, hostExePath, allUsers, flags);
  return resultText(session.remove()(&request));
}

}  // namespace config_client
