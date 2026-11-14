// 主程序侧的配置模块调用者
//
// 右键菜单与文件关联的实现已经搬到 aizip_config.dll（安装器与主程序共用
// 同一份）。主程序不自己写注册表，只负责「发现需要修复时叫一下它」。
//
// 调用走的是 AiInstaller 的标准配置 ABI：安装器是通用的，只认那四个导出
// 名；主程序也按同一份契约调用，两边永远一致。
#pragma once

#include "AiInstaller/config_abi.h"

#include <string>

namespace config_client {

// 本模块的接口版本，用于拒绝加载旧版 dll
int apiVersion();

// 当前环境下的配置是否已就绪（菜单挂好且指向本程序、关联已登记）。
// 模块不可用或缺文件时返回 false：调用方据此尝试修复。
bool isReady(const std::wstring& installDir, const std::wstring& hostExePath,
             bool allUsers);

// 让配置生效。返回非空字符串表示失败原因。
std::wstring apply(const std::wstring& installDir, const std::wstring& hostExePath,
                   bool allUsers, unsigned flags = 0);

// 撤销配置（主程序退出、卸载时用）
std::wstring remove(const std::wstring& installDir, const std::wstring& hostExePath,
                    bool allUsers, unsigned flags = 0);

}  // namespace config_client
