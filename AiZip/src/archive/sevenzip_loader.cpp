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
    // GZIP：单文件压缩，多文件由对话框拦下并提示改用 TAR.GZ
    {L"GZIP", L".gz", kClsidGZip, true, false, false, 9},
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
  return -1;
}

}  // namespace z7
