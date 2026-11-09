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
  if (e == L".7z") return L"7Z";
  if (e == L".tar") return L"TAR";
  if (e == L".gz" || e == L".tgz") return L"GZ";
  if (e == L".bz2" || e == L".tbz2") return L"BZ2";
  if (e == L".xz" || e == L".txz") return L"XZ";
  if (e == L".cab") return L"CAB";
  if (e == L".rar") return L"RAR";
  if (e == L".iso") return L"ISO";
  if (e == L".wim") return L"WIM";
  return L"7Z";
}

// ── 可创建格式表 ──
// 格式顺序：ZIP / 7Z 单独单选，其余按下表顺序进下拉。
// 7-Zip 的 gzip/bzip2/xz handler 只接受单文件，多文件必须走 tar 包裹，
// 这就是 TAR.GZ / TAR.BZ2 / TAR.XZ 三个组合格式存在的原因。
namespace {
const FormatInfo kFormats[] = {
    {L"ZIP", L".zip", kClsidZip, false, false, false, 9},
    {L"7Z", L".7z", kClsid7z, false, false, true, 9},
    {L"TAR.GZ", L".tar.gz", kClsidGZip, true, true, false, 9},
    {L"TAR.BZ2", L".tar.bz2", kClsidBZip2, true, true, false, 9},
    {L"TAR.XZ", L".tar.xz", kClsidXz, true, true, false, 9},
    {L"TAR", L".tar", kClsidTar, false, false, false, 0},
    {L"WIM", L".wim", kClsidWim, false, false, false, 0},
    {L"GZIP", L".gz", kClsidGZip, true, false, false, 9},
    {L"BZIP2", L".bz2", kClsidBZip2, true, false, false, 9},
    {L"XZ", L".xz", kClsidXz, true, false, false, 9},
};
}  // namespace

const FormatInfo* Formats() { return kFormats; }
int FormatCount() { return static_cast<int>(sizeof(kFormats) / sizeof(kFormats[0])); }

int FormatIndexByName(const wchar_t* name) {
  if (!name) return -1;
  for (int i = 0; i < FormatCount(); ++i) {
    if (::CompareStringOrdinal(kFormats[i].name, -1, name, -1, TRUE) == CSTR_EQUAL) return i;
  }
  return -1;
}

}  // namespace z7
