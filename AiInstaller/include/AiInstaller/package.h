// 安装包：追加在 exe 末尾的「清单 + 载荷」
//
// 为什么要追加而不是编进资源：安装器要给所有应用共用，编译一次就固定下来，
// 应用那边只是把清单和文件贴到它末尾。这样「一个 exe 装任何东西」成立，
// 应用也不必为了换文案重新编译安装器。
//
// 文件布局（全部小端）：
//
//   ┌──────────────────────────┐
//   │ 通用安装器 AiSetup.exe    │
//   ├──────────────────────────┤ ← dataOffset
//   │ 清单（UTF-8 文本）        │  manifestSize 字节
//   ├──────────────────────────┤ ← payloadBase = dataOffset + manifestSize
//   │ 文件 1                    │
//   │ 文件 2 ...                │  清单里记录各自的 offset/size（相对 payloadBase）
//   ├──────────────────────────┤
//   │ Trailer（32 字节）        │
//   └──────────────────────────┘
//
// Trailer 放末尾而不是开头：exe 头部结构不能动，系统靠它加载映像。
// 读取时从文件尾倒着找魔数，找不到就是「没有被包装过的裸安装器」。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AiInstaller/manifest.h"

namespace AiInstaller {

// Trailer 的固定长度与魔数，打包器与读取器共用
constexpr size_t kTrailerSize = 32;
constexpr char kPackageMagic[8] = {'A', 'I', 'S', 'E', 'T', 'U', 'P', '1'};
constexpr uint32_t kPackageFormatVersion = 1;

// 一次拆包的结果
class Package {
 public:
  // 打开本进程可执行文件并定位包。
  // 返回 false 表示这不是一个打包过的安装器（error 为空表示「没打包」，
  // 非空表示文件损坏），调用方据此给出可读提示。
  bool openSelf(std::wstring& error);

  bool isPackaged() const { return packaged_; }
  const Manifest& manifest() const { return manifest_; }

  // 把某个载荷文件写到 targetPath（父目录自动创建）。返回空串表示成功。
  std::wstring extractFile(const ManifestFile& file, const std::wstring& targetPath) const;

  // 把包内某个文件整体读进内存。小文件（图标）用；大文件请走 extractFile。
  bool readBlob(const std::wstring& path, std::vector<uint8_t>& out) const;

  // 把「安装器本体」复制到 targetPath，去掉末尾的清单与载荷。
  //
  // 为什么不在安装目录里留完整安装包：那边已经有一份解压好的文件了，
  // 再存一份载荷等于每个应用多占一倍体积。卸载靠同目录的清单文件，
  // 不需要再从 exe 里读。复制出去的那份就是安装目录里的卸载程序。
  std::wstring copyInstallerTo(const std::wstring& targetPath) const;

  // 包内清单的原始文本（写进安装目录的清单文件，供卸载时读回）
  const std::wstring& manifestText() const { return manifestText_; }

  // 安装目录里卸载程序的文件名。
  //
  // 直接用安装器本体换个名字：本体已经带好了应用图标与版本资源，用户可以
  // 在目录里看到它、也可以在「应用与功能」里点到它，不必再编一个专用 exe。
  // 名字固定成 uninstall.exe 而不是安装包的名字（AiZipSetup.exe）：卸载入口
  // 叫「卸载」比叫「安装」符合用户预期。
  static const wchar_t* uninstallFileName() { return L"uninstall.exe"; }

  // 安装目录里记录清单的文件名
  static const wchar_t* manifestFileName() { return L"installer.manifest"; }

 private:
  std::wstring selfPath_;
  bool packaged_ = false;
  Manifest manifest_;
  std::wstring manifestText_;
  // 载荷区起点（清单之后）与清单起点
  uint64_t dataOffset_ = 0;
  uint64_t payloadBase_ = 0;
  uint64_t payloadSize_ = 0;
};

// 从文件读清单（卸载时用：安装目录里的 installer.manifest）
bool loadManifestFile(const std::wstring& path, Manifest& out, std::wstring& error);

}  // namespace AiInstaller
