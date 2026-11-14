// 安装器的应用描述：被安装程序的身份与目录约定
//
// AiInstaller 是公共库，不知道自己在装什么。运行期从安装包清单里读出这些
// 描述，库只按描述做事。
#pragma once

#include <string>

namespace AiInstaller {

// 一个可安装应用的静态描述。运行期由清单填充，只读。
struct AppInfo {
  // 「应用与功能」与快捷方式里显示的名字，如「爱压」
  std::wstring displayName;
  // 版本号，如 "1.0.0"；只用于卸载项显示
  std::wstring version;
  // 发布者
  std::wstring publisher;
  // 主程序文件名，如 "AiZip.exe"。快捷方式指向安装目录下的它。
  std::wstring exeName;
  // 安装子目录，不含盘符、不含前导分隔符，如 "AISoft\AiZip"。
  //
  // 为什么不含盘符：装到哪个盘由 AiInstaller 的路径策略在运行时决定，
  // 应用不该把「D:\」这种本机事实写死进代码。
  std::wstring installSubdir;
  // 安装完成后要执行的命令（可带参数，相对安装目录解析）。空表示不启动任何东西。
  std::wstring afterInstall;
};

}  // namespace AiInstaller
