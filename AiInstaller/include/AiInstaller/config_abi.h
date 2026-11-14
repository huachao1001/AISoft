// 标准配置模块 ABI
//
// 安装器是通用的：它不知道自己在装哪个应用，更不可能认识某个应用私有的
// 导出名（如 AiZipConfigApply）。所以配置能力必须收敛到一套固定的契约上：
// 任何应用的配置 dll 都按这里导出，安装器只认这套。
//
// 这样才真正做到「安装包只负责解压与删除，配置一律调用 dll」，而且应用
// 主程序自己也能按同一契约调它来自愈（右键菜单丢了、程序换了位置）。
//
// 导出的是纯 C 接口 + 固定结构体：调用方用 LoadLibrary + GetProcAddress
// 取得，不依赖 C++ ABI，也不强求两边用同一种 CRT。
//
// 约定：模块必须同时导出下面四个函数，缺一即视为不可用。
//   AiSetupConfigApply     让配置生效（幂等）
//   AiSetupConfigRemove    撤销配置
//   AiSetupConfigIsReady   查询是否已就绪（1 就绪 / 0 需修复 / 负数为错误）
//   AiSetupConfigVersion   返回 AISETUP_CONFIG_API_VERSION
#pragma once

#if defined(_WIN32) && defined(AISETUP_CONFIG_EXPORTS)
#define AISETUP_CONFIG_API __declspec(dllexport)
#else
// 调用方一律动态加载，不需要 dllimport，也就不必链接导入库
#define AISETUP_CONFIG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 接口版本。结构与语义有不兼容变化时递增，调用方据此拒绝加载旧模块。
#define AISETUP_CONFIG_API_VERSION 1

// 配置项开关：位含义由各应用自己定义（例如右键菜单、文件关联各占一位）。
// 0 表示「全部」，这样最小实现只要忽略 flags 就够了。
#define AISETUP_CONFIG_ALL 0u

typedef struct AiSetupConfigRequest {
  // 调用方填 AISETUP_CONFIG_API_VERSION
  int apiVersion;
  // 安装目录（不含尾部分隔符）。组件、关联等相对它定位。必填。
  const wchar_t* installDir;
  // 主程序完整路径。注册表里的菜单与关联都指向它。必填。
  // 不让模块自己猜：调用方可能是安装器，猜出来的是安装器自己。
  const wchar_t* hostExePath;
  // 配置项组合；0 等同全部
  unsigned flags;
  // 非 0 表示按「所有用户」写 HKLM，0 表示写 HKCU
  int allUsers;
} AiSetupConfigRequest;

// 返回码 0 表示成功；非 0 时 message 是可读原因。
// message 指向模块内部缓冲，下次调用会被覆盖，调用方要留存就自己复制。
typedef struct AiSetupConfigResult {
  int code;
  const wchar_t* message;
} AiSetupConfigResult;

AISETUP_CONFIG_API AiSetupConfigResult AiSetupConfigApply(const AiSetupConfigRequest* request);
AISETUP_CONFIG_API AiSetupConfigResult AiSetupConfigRemove(const AiSetupConfigRequest* request);
AISETUP_CONFIG_API int AiSetupConfigIsReady(const AiSetupConfigRequest* request);
AISETUP_CONFIG_API int AiSetupConfigVersion(void);

#ifdef __cplusplus
}
#endif
