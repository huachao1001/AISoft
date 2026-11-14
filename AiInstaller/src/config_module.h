// 配置模块调用器：按标准 ABI 加载并调用安装目录里的 dll
//
// 安装器、主程序自愈、卸载三方共用的都是这一份。它只认固定的四个导出名，
// 因此对任何应用都成立——这正是「安装包只负责解压与删除」能落地的前提。
#pragma once

#include <string>

#include "AiInstaller/path_policy.h"

namespace AiInstaller {
namespace detail {

// 一次调用的结果
struct ConfigCallResult {
  // true 表示模块可用且调用成功
  bool ok = false;
  // 失败原因（模块缺失、版本不符、模块返回的错误）
  std::wstring error;
};

// 安装目录里 moduleName 指向的配置模块是否可用（文件存在、导出齐全、版本匹配）
bool isConfigModuleUsable(const std::wstring& installDir, const std::wstring& moduleName);

// apply=true 让配置生效，false 撤销。
//
// 模块不可用时：apply 视为失败（配置没做上，安装不该报成功），
// remove 视为成功（文件都要删了，残留注册表只是无效项，不该拦住卸载）。
ConfigCallResult callConfigModule(const std::wstring& installDir, const std::wstring& hostExePath,
                                  const std::wstring& moduleName, Scope scope, bool apply);

// 查询配置是否已就绪。模块不可用返回 false。
bool queryConfigModule(const std::wstring& installDir, const std::wstring& hostExePath,
                       const std::wstring& moduleName, Scope scope);

}  // namespace detail
}  // namespace AiInstaller
