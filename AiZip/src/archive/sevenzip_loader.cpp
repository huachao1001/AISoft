#include "sevenzip_loader.h"

#include <cwctype>

// 7-Zip 静态链接：CreateObject 由 7zstatic 库里的 Z7StaticExports.cpp 直接导出，
// 不再是 dll 的 GetProcAddress。动态链接相关逻辑已全部移除。
extern "C" HRESULT WINAPI CreateObject(const GUID* clsid, const GUID* iid, void** outObject);

namespace z7 {

IInArchive* CreateInArchive(const GUID& clsId) {
  void* out = nullptr;
  if (FAILED(::CreateObject(&clsId, &kIidIInArchive, &out))) return nullptr;
  return static_cast<IInArchive*>(out);
}

IOutArchive* CreateOutArchive(const GUID& clsId) {
  void* out = nullptr;
  if (FAILED(::CreateObject(&clsId, &kIidIOutArchive, &out))) return nullptr;
  return static_cast<IOutArchive*>(out);
}

namespace {
std::wstring LowerExt(const std::wstring& extIn) {
  std::wstring e = extIn;
  for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
  return e;
}
}  // namespace

GUID FormatFromExtension(const std::wstring& extIn) {
  const std::wstring e = LowerExt(extIn);
  if (e == L".zip") return kClsidZip;
  // 这几个后缀本质是 ZIP 容器：先试 ZIP handler，比先落到 7z 再失败重试快
  if (e == L".aar" || e == L".apk" || e == L".jar" || e == L".epub") return kClsidZip;
  if (e == L".7z") return kClsid7z;
  if (e == L".tar") return kClsidTar;
  if (e == L".gz" || e == L".tgz") return kClsidGZip;
  if (e == L".bz2" || e == L".tbz2") return kClsidBZip2;
  if (e == L".xz" || e == L".txz") return kClsidXz;
  if (e == L".cab") return kClsidCab;
  if (e == L".rar") return kClsidRar;
  return kClsid7z;
}

const wchar_t* FormatNameFromExtension(const std::wstring& extIn) {
  const std::wstring e = LowerExt(extIn);
  if (e == L".zip") return L"ZIP";
  // AAR/APK/JAR/EPUB 都是 ZIP 容器，按真实格式显示而不是笼统的 7Z
  if (e == L".aar" || e == L".apk" || e == L".jar" || e == L".epub") return L"ZIP";
  if (e == L".7z") return L"7Z";
  if (e == L".tar") return L"TAR";
  if (e == L".gz" || e == L".tgz") return L"GZ";
  if (e == L".bz2" || e == L".tbz2") return L"BZ2";
  if (e == L".xz" || e == L".txz") return L"XZ";
  if (e == L".cab") return L"CAB";
  if (e == L".rar") return L"RAR";
  if (e == L".iso") return L"ISO";
  if (e == L".wim") return L"WIM";
  // 认不出来就返回空串，让调用方不画标签。
  // 以前这里兜底返回 "7Z"，任何认不出的后缀都会被标成 7z 格式，
  // 界面上等于报了个假消息；宁可空着也不能写错。
  return L"";
}

const wchar_t* FormatNameFromClsid(const GUID& clsId) {
  struct Pair {
    const GUID* clsid;
    const wchar_t* name;
  };
  // 只列能出现在浏览界面的容器格式。7-Zip 还有些 handler 是「分析器」而非
  // 归档格式（ELF、PE、FAT 之类），它们没有用户可读的名字，返回 nullptr
  // 让调用方退回扩展名推断。
  static const Pair kNames[] = {
      {&kClsidZip, L"ZIP"},   {&kClsid7z, L"7Z"},     {&kClsidRar, L"RAR"},
      {&kClsidRar5, L"RAR"},  {&kClsidTar, L"TAR"},   {&kClsidGZip, L"GZ"},
      {&kClsidBZip2, L"BZ2"}, {&kClsidXz, L"XZ"},     {&kClsidZstd, L"ZSTD"},
      {&kClsidLzma, L"LZMA"}, {&kClsidCab, L"CAB"},   {&kClsidIso, L"ISO"},
      {&kClsidUdf, L"UDF"},   {&kClsidWim, L"WIM"},   {&kClsidLzh, L"LZH"},
      {&kClsidArj, L"ARJ"},   {&kClsidZ, L"Z"},       {&kClsidNsis, L"NSIS"},
      {&kClsidChm, L"CHM"},   {&kClsidCpio, L"CPIO"}, {&kClsidRpm, L"RPM"},
      {&kClsidAr, L"AR"},     {&kClsidXar, L"XAR"},   {&kClsidDmg, L"DMG"},
      {&kClsidHfs, L"HFS"},   {&kClsidApfs, L"APFS"}, {&kClsidSquashfs, L"SQUASHFS"},
      {&kClsidVhd, L"VHD"},   {&kClsidVhdx, L"VHDX"}, {&kClsidVdi, L"VDI"},
      {&kClsidVmdk, L"VMDK"}, {&kClsidQcow, L"QCOW"},
  };
  for (const Pair& p : kNames) {
    if (::IsEqualGUID(*p.clsid, clsId)) return p.name;
  }
  return nullptr;
}

// ── 可创建格式表 ──
// 界面上每项各占一个单选，顺序与这里一致。
// 7-Zip 的 gzip handler 只接受单文件，多文件必须先用 tar 包裹，
// 这就是 TAR.GZ 存在的原因。
//
// 表里只放真正能压出来的格式。RAR 不在此列：7-Zip 只有 RAR 解码器、
// 没有编码器，放进来只会让用户点确定后收到「压缩失败」。
namespace {
const FormatInfo kFormats[] = {
    {L"ZIP", L".zip", kClsidZip, false, false, false, 9},
    {L"7Z", L".7z", kClsid7z, false, false, true, 9},
    // TAR：只打包不压缩，没有档位
    {L"TAR", L".tar", kClsidTar, false, false, false, 0},
    // GZ：单文件压缩，多文件由对话框拦下并提示改用 TAR.GZ
    {L"GZ", L".gz", kClsidGZip, true, false, false, 9},
    // TAR.GZ：先打 tar 再压，因此能容纳任意数量文件
    {L"TAR.GZ", L".tar.gz", kClsidGZip, false, true, false, 9},
};
}  // namespace

const FormatInfo* Formats() { return kFormats; }
int FormatCount() { return static_cast<int>(sizeof(kFormats) / sizeof(kFormats[0])); }

int FormatIndexByName(const wchar_t* name) {
  if (!name) return -1;
  for (int i = 0; i < FormatCount(); ++i) {
    if (::CompareStringOrdinal(kFormats[i].name, -1, name, -1, TRUE) == CSTR_EQUAL) return i;
  }
  // 旧名兼容：格式显示名改过之后，用户设置里存的还是旧名。
  // 不做这层映射，升级后打开对话框会退回默认格式，用户会以为自己选错了。
  if (::CompareStringOrdinal(L"GZIP", -1, name, -1, TRUE) == CSTR_EQUAL) {
    return FormatIndexByName(L"GZ");
  }
  return -1;
}

}  // namespace z7
