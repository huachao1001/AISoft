// 爱压的「系统配置」能力：资源管理器右键菜单、文件关联
//
// 这两项都是「注册表 + 组件落盘」，且只在装好之后才需要一个明确入口：
//   - 安装程序装完文件后调一次（首次安装）；
//   - 主程序启动时校对一次（菜单被别的软件清了、程序换了位置）；
//   - 卸载时撤销一次。
//
// 导出的是 AiInstaller 的标准配置 ABI（AiInstaller/config_abi.h）：
// 安装器是通用的，它不认识爱压，只按那四个固定导出名调用。爱压自己
// 也按同一份契约调用，两边永远一致。
//
// 快捷方式与卸载入口不在这里：那是安装器的通用职责。
#pragma once

#include "AiInstaller/config_abi.h"

// 本模块的版本，与标准 ABI 对齐。结构和语义有变时一起递增。
#define AIZIP_CONFIG_API_VERSION AISETUP_CONFIG_API_VERSION

// 配置项开关。可以只修复其中某一项，例如只重建右键菜单。
enum AiZipConfigFlags {
  kAiZipConfigShellMenu = 1 << 0,         // 资源管理器右键菜单（含内嵌组件的释放）
  kAiZipConfigFileAssociations = 1 << 1,  // 压缩包「打开方式」候选
  kAiZipConfigAll = 0x3,
};

// 参数结构沿用标准的：安装器只认那一套，多一层转换只会多一处出错。
using AiZipConfigRequest = AiSetupConfigRequest;
using AiZipConfigResult = AiSetupConfigResult;
