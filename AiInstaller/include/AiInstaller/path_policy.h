// 安装路径策略：默认装到哪个盘、哪个目录
//
// 策略是「优先非系统盘」：系统盘（通常是 C）装满系统与页面文件，把应用装到
// 其它固定盘更稳。这条规则与具体应用无关，所以放在公共库里，各应用不必
// 各写一遍。
//
// 本库只决定「哪个盘」：子目录永远来自清单的 installSubdir。装到盘的哪个
// 位置是应用自己的事，策略不该替它拼目录。
#pragma once

#include <string>
#include <vector>

#include "AiInstaller/app_info.h"

namespace AiInstaller {

// 安装范围。与 AiUI 的 InstallScope 同义，单独定义是为了让本库的调用方
// 不必依赖 AiUI 的头文件。
enum class Scope {
  kCurrentUser,
  kAllUsers,
};

// 一个候选盘符。由平台层填写，策略层只做选择。
struct Volume {
  // 盘符根，如 "D:\\"
  std::wstring root;
  // 是否为系统盘
  bool system = false;
  // 可用空间（字节）；0 表示未知
  uint64_t freeBytes = 0;
};

// 列出可用于安装的固定盘。可移动盘与光驱被排除：拔掉后程序就没了。
std::vector<Volume> listInstallVolumes();

// 盘根是否可写：在根上建一个探针文件再删掉。
//
// 有些机器只给管理员写盘根（ACL 里 Authenticated Users 只有读权限），这类盘
// 上装不进去。提前探测比让用户在安装末尾看到「无法创建目录」要好。
// 只探盘根：子目录由安装器自己新建，新建出来的目录当前用户必然可写。
bool isVolumeWritable(const std::wstring& root);

// 按策略挑一个盘：
//   1. 非系统盘且可写，按盘符字母序取第一个；
//   2. 没有这样的盘时，退而取可写的系统盘；
//   3. 全都不合用时返回空串。
// 「优先非系统盘」是默认行为，不是硬规则：只有系统盘可用时必须能用它。
std::wstring pickInstallVolume();

// 默认安装目录：挑一个盘 + 应用的安装子目录。
// 返回形如 "D:\\AISoft\\AiZip"，不含尾部分隔符；没有可用盘时返回空串。
std::wstring defaultInstallDir(const AppInfo& app);

// 「仅当前用户」范围的安装目录。
//
// 与所有用户范围共用同一套选盘与子目录：范围只决定写 HKCU 还是 HKLM、
// 要不要提权，不该让安装位置跟着换到用户目录去。之前这里拼的是
// %USERPROFILE%\Programs，而用户目录永远在系统盘上——「不装 C 盘」这条
// 规则就被悄悄绕过了，静默安装（固定走当前用户）每次都会落到 C 盘。
std::wstring perUserInstallDir(const AppInfo& app);

// 把安装子目录拼到某个盘根上，如 ("D:\\", "AISoft\\AiZip") → "D:\\AISoft\\AiZip"
std::wstring joinInstallDir(const std::wstring& volumeRoot, const std::wstring& subdir);

}  // namespace AiInstaller
