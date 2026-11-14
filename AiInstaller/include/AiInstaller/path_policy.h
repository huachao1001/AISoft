// 安装路径策略：默认装到哪个盘、哪个目录
//
// 策略是「优先非系统盘」：系统盘（通常是 C）装满系统与页面文件，把应用装到
// 其它固定盘更稳。这条规则与具体应用无关，所以放在公共库里，各应用不必
// 各写一遍。
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

// 按策略挑一个盘：跳过系统盘，从字母序靠前的非系统盘开始，都没有才用系统盘。
// 找不到任何可用盘时返回空串。
std::wstring pickInstallVolume();

// 默认安装目录：挑一个盘 + 应用的安装子目录。
// 返回形如 "D:\\AISoft\\AiZip"，不含尾部分隔符；没有可用盘时返回空串。
std::wstring defaultInstallDir(const AppInfo& app);

// 用户主目录下的安装目录（仅当前用户范围使用），如
// "C:\\Users\\x\\AppData\\Local\\Programs\\AISoft\\AiZip"。
// 该目录总在系统盘上，但只有当前用户能写，不需要提权。
std::wstring perUserInstallDir(const AppInfo& app);

// 把安装子目录拼到某个盘根上，如 ("D:\\", "AISoft\\AiZip") → "D:\\AISoft\\AiZip"
std::wstring joinInstallDir(const std::wstring& volumeRoot, const std::wstring& subdir);

}  // namespace AiInstaller
