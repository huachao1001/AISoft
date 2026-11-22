// 文件类型关联：把后缀交给本程序打开，并取「关联后」的默认图标
//
// 两件事必须由平台回答，不能由库自绘：
//
// 1. **登记**。Windows 写 ProgID + HKCU/HKLM 合并视图，macOS 走
//    LaunchServices，Linux 写 .desktop 的 MimeType。三者完全不同。
//
// 2. **关联后的默认图标**。用户要在文件列表里认出**那个文件**，所以图标
//    必须来自系统（Windows 的 ProgID\DefaultIcon、macOS 的文档图标、
//    Linux 的 MimeType 反查）。自绘一个「压缩包」图标是错的：所有 zip
//    会长得一样，用户反而分不出是哪个包。
//
// 现代系统不允许静默抢占默认程序（Win10+ 的 UserChoice 带用户意图哈希），
// 所以登记只能做到「出现在打开方式里等用户确认」。这一点要在文档里说清，
// 不要让调用方以为「登记了就是默认」。
#pragma once

#include "pyui/painter.h"
#include "pyui/types.h"

#include <string>
#include <vector>

namespace pyui {

enum class InstallScope {
  kCurrentUser,
  kAllUsers,
};

struct FileAssociation {
  // 处理器标识，如 "PyUI.zip"。同一程序内唯一；注册后不可改，
  // 否则已安装用户的关联会失效。
  std::string progId;
  // 系统「打开方式」列表里显示的描述，如 "PyUI 压缩包"
  std::string description;
  // 生效后缀，含点，如 {".zip", ".7z"}
  std::vector<std::string> extensions;
  // 打开这些后缀的程序路径。留空表示当前进程的可执行文件。
  // 必须能显式指定：安装程序是替别人注册，用当前进程路径会把登记指向工具自己。
  std::string hostExePath;
  // 安装范围。留默认值时用当前进程的实际范围。
  InstallScope scope = InstallScope::kCurrentUser;
  bool scopeExplicit = false;
  // 关联文件的默认图标来源：本可执行文件里的图标资源 id。
  // 0 表示由平台决定（通常取 exe 的主图标）。
  int iconResourceId = 0;
};

// 关联文件的图标取用方式：给平台一个提示，让它知道该去哪找图标。
struct AssociationIconRequest {
  // 具体文件路径。给 path 时优先按「这个文件的关联」取图标，
  // 这样用户自定义过图标的情况下也能取对。
  std::string path;
  // 没有 path 或用不上时，按后缀取。含点，如 ".zip"。
  std::string extension;
  // 要画出来的**设备像素**边长，不是设计值。调用方自己乘 DPI。
  // 系统图标有多档（16/20/24/32/40/48/64/96/128/256），按需要的边长取最接近
  // 的一档：宁可略大让它缩下来，也不要小了再拉伸——缩小是抗锯齿重采样，
  // 放大只能插值猜像素。
  int pixelSize = 0;
};

class FileAssociationHost {
 public:
  virtual ~FileAssociationHost() = default;

  // 登记为这些后缀的候选打开方式。成功返回空串，失败返回可读原因。
  virtual std::string registerFileAssociations(const FileAssociation& association) = 0;
  // 撤销登记
  virtual std::string unregisterFileAssociations(const FileAssociation& association) = 0;
  // 是否已登记且仍指向当前可执行文件
  virtual bool areFileAssociationsRegistered(const FileAssociation& association) = 0;
  // 探测本程序实际登记在哪个范围（HKCU / HKLM）。两个根键都没有时返回 kCurrentUser。
  //
  // 为什么需要：安装时选了「所有用户」的程序，每次启动都要按同一范围校对，
  // 不能想当然用默认值，否则会往另一个根键再写一份，出现「逐项查都对、双击却
  // 打开了另一份程序」的故障。
  virtual InstallScope detectScope(const FileAssociation& association) = 0;

  // 取关联文件的默认图标。返回无效 Image 表示取不到，调用方退回内置图标。
  // 句柄生命周期由后端缓存持有，调用方不要释放。
  virtual Image associationIcon(const AssociationIconRequest& request) = 0;

  // 释放全部缓存的关联图标；宿主退出前调用一次。
  virtual void releaseAssociationIcons() = 0;
};

}  // namespace pyui
