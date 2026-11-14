// 安装范围的探测
//
// 这里只回答一个问题：本程序当初装在哪个范围（HKCU 还是 HKLM）。
// 右键菜单、文件关联的注册与修复已经搬到 aizip_config.dll——那边才是
// 唯一的事实来源，主程序不该再存一份 CLSID、组件名和注册逻辑。
//
// 为什么范围还要在这里：配置模块不负责「装在哪」，它只按调用方给的
// 范围去写。谁来告诉它？主程序启动时得先自己知道。
#pragma once

#include "AiUI/shell.h"

namespace shell {

// 安装时选定的范围（安装版从注册表探测，开发期为当前用户）。
AiUI::InstallScope InstalledScope();

}  // namespace shell
