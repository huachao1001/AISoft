// 7-Zip 内核的 GUID 与入口
#pragma once

#include "sevenzip.h"
#include <string>

namespace z7 {

// 接口 GUID 布局：{23170F69-40C1-278A-0000-00<group>00<sub>0000}
constexpr GUID MakeIfaceGuid(unsigned group, unsigned sub) {
  return GUID{0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, static_cast<BYTE>(group),
                                           0x00, static_cast<BYTE>(sub), 0x00, 0x00}};
}

// 格式 handler GUID 布局：{23170F69-40C1-278A-1000-0001 10 <sub> 0000}
constexpr GUID MakeHandlerGuid(unsigned sub) {
  return GUID{0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01,
                                           0x10, static_cast<BYTE>(sub), 0x00, 0x00}};
}

constexpr GUID kClsidZip = MakeHandlerGuid(0x01);
constexpr GUID kClsidBZip2 = MakeHandlerGuid(0x02);
constexpr GUID kClsidRar = MakeHandlerGuid(0x03);
constexpr GUID kClsidArj = MakeHandlerGuid(0x04);
constexpr GUID kClsidZ = MakeHandlerGuid(0x05);
constexpr GUID kClsidLzh = MakeHandlerGuid(0x06);
constexpr GUID kClsid7z = MakeHandlerGuid(0x07);
constexpr GUID kClsidCab = MakeHandlerGuid(0x08);
constexpr GUID kClsidNsis = MakeHandlerGuid(0x09);
constexpr GUID kClsidLzma = MakeHandlerGuid(0x0A);
constexpr GUID kClsidXz = MakeHandlerGuid(0x0C);
constexpr GUID kClsidPpmd = MakeHandlerGuid(0x0D);
constexpr GUID kClsidZstd = MakeHandlerGuid(0x0E);
constexpr GUID kClsidApfs = MakeHandlerGuid(0xC3);
constexpr GUID kClsidVhdx = MakeHandlerGuid(0xC4);
constexpr GUID kClsidBase64 = MakeHandlerGuid(0xC5);
constexpr GUID kClsidExt = MakeHandlerGuid(0xC7);
constexpr GUID kClsidVmdk = MakeHandlerGuid(0xC8);
constexpr GUID kClsidVdi = MakeHandlerGuid(0xC9);
constexpr GUID kClsidQcow = MakeHandlerGuid(0xCA);
constexpr GUID kClsidGpt = MakeHandlerGuid(0xCB);
constexpr GUID kClsidRar5 = MakeHandlerGuid(0xCC);
constexpr GUID kClsidHxs = MakeHandlerGuid(0xCE);
constexpr GUID kClsidPe = MakeHandlerGuid(0xCF);
constexpr GUID kClsidUefiF = MakeHandlerGuid(0xD1);
constexpr GUID kClsidSquashfs = MakeHandlerGuid(0xD2);
constexpr GUID kClsidCramfs = MakeHandlerGuid(0xD3);
constexpr GUID kClsidApm = MakeHandlerGuid(0xD4);
constexpr GUID kClsidMslz = MakeHandlerGuid(0xD5);
constexpr GUID kClsidFlv = MakeHandlerGuid(0xD6);
constexpr GUID kClsidSwf = MakeHandlerGuid(0xD7);
constexpr GUID kClsidNtfs = MakeHandlerGuid(0xD9);
constexpr GUID kClsidFat = MakeHandlerGuid(0xDA);
constexpr GUID kClsidVhd = MakeHandlerGuid(0xDC);
constexpr GUID kClsidElf = MakeHandlerGuid(0xDE);
constexpr GUID kClsidMacho = MakeHandlerGuid(0xDF);
constexpr GUID kClsidUdf = MakeHandlerGuid(0xE0);
constexpr GUID kClsidXar = MakeHandlerGuid(0xE1);
constexpr GUID kClsidMub = MakeHandlerGuid(0xE2);
constexpr GUID kClsidHfs = MakeHandlerGuid(0xE3);
constexpr GUID kClsidDmg = MakeHandlerGuid(0xE4);
constexpr GUID kClsidCom = MakeHandlerGuid(0xE5);
constexpr GUID kClsidMbr = MakeHandlerGuid(0xDB);
constexpr GUID kClsidWim = MakeHandlerGuid(0xE6);
constexpr GUID kClsidIso = MakeHandlerGuid(0xE7);
constexpr GUID kClsidChm = MakeHandlerGuid(0xE9);
constexpr GUID kClsidSplit = MakeHandlerGuid(0xEA);
constexpr GUID kClsidRpm = MakeHandlerGuid(0xEB);
constexpr GUID kClsidAr = MakeHandlerGuid(0xEC);
constexpr GUID kClsidCpio = MakeHandlerGuid(0xED);
constexpr GUID kClsidTar = MakeHandlerGuid(0xEE);
constexpr GUID kClsidGZip = MakeHandlerGuid(0xEF);

// 打开压缩包时依次尝试的 handler。顺序即猜测优先级：常见容器在前，
// 磁盘映像/可执行体在后。扩展名不可靠（改名的 zip、.001 分卷），必须逐个试。
constexpr GUID kAllHandlers[] = {
    kClsid7z,    kClsidZip,    kClsidRar,  kClsidRar5, kClsidTar,   kClsidGZip,
    kClsidBZip2, kClsidXz,     kClsidZstd, kClsidLzma, kClsidPpmd,  kClsidCab,
    kClsidIso,   kClsidUdf,    kClsidWim,  kClsidMbr,  kClsidLzh,  kClsidArj,   kClsidZ,
    kClsidNsis,  kClsidChm,    kClsidHxs,  kClsidCpio, kClsidRpm,   kClsidAr,
    kClsidXar,   kClsidCom,    kClsidDmg,  kClsidHfs,  kClsidApfs,  kClsidSquashfs,
    kClsidCramfs, kClsidExt,   kClsidNtfs, kClsidFat,  kClsidGpt,   kClsidApm,
    kClsidBase64, kClsidSplit, kClsidVhd,  kClsidVhdx, kClsidVdi,   kClsidVmdk,
    kClsidQcow,  kClsidMslz,   kClsidMub,  kClsidElf,  kClsidMacho, kClsidPe,
    kClsidUefiF, kClsidSwf,    kClsidFlv,
};

constexpr GUID kIidIInArchive = MakeIfaceGuid(6, 0x60);
constexpr GUID kIidIOutArchive = MakeIfaceGuid(6, 0xA0);
constexpr GUID kIidISetProperties = MakeIfaceGuid(6, 0x03);

// 7-Zip 内核（静态链接，无需 dll，也不需要显式初始化）
IInArchive* CreateInArchive(const GUID& clsId);
IOutArchive* CreateOutArchive(const GUID& clsId);

// 按扩展名推断容器格式
GUID FormatFromExtension(const std::wstring& ext);
const wchar_t* FormatNameFromExtension(const std::wstring& ext);
// 按「真正打开它的 handler」取格式显示名，未收录的返回 nullptr。
// 扩展名会骗人（临时副本、无后缀文件、扩展名被改过的包都见过），
// 能把包打开的那个 handler 才是事实。
const wchar_t* FormatNameFromClsid(const GUID& clsId);

// ── 可创建的压缩格式 ──
// 格式列表顺序：ZIP/7Z 单列单选，其余进下拉。
// 每种格式声明扩展名、显示名、是否单文件容器、支持的档位。
struct FormatInfo {
  const wchar_t* name;      // 下拉显示名，如 L"TAR.GZ"
  const wchar_t* ext;       // 主扩展名，含点
  GUID clsId;
  bool singleFile;          // 只能容纳一个文件（gzip/bzip2/xz）
  bool tarWrapped;          // 先打 tar 再压缩（TAR.GZ 等组合格式）
  bool solidCapable;        // 支持固实
  int maxLevel;             // 最高档位（0 表示只能用存储档）
};

const FormatInfo* Formats();
int FormatCount();
// 按显示名查格式索引，未找到返回 -1
int FormatIndexByName(const wchar_t* name);

}  // namespace z7
