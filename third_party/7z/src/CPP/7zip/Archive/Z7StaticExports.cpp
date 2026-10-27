// 7-Zip 静态链接入口。
//
// 原版通过 7z.dll 导出 CreateObject 供外部调用，这里改为在 exe 内直接暴露同名函数，
// 使上层 sevenzip_loader.cpp 的调用代码完全不用改。
//
// 与 DllExports2.cpp 的差异：
//   - 没有 DllMain / DLL_PROCESS_ATTACH
//   - 不启用 Z7_EXTERNAL_CODECS：静态链接下没有外部 codec 宿主，纯内部 codec
//   - Z7_ST 单线程？否，LZMA 多线程是压缩性能关键，保持默认
#include "StdAfx.h"

#include "../../Common/MyWindows.h"
#include "../../Common/MyInitGuid.h"

#include "../../Common/ComTry.h"

#include "../../Windows/PropVariant.h"

#include "../ICoder.h"
#include "../IPassword.h"

#include "../Common/CreateCoder.h"

#include "IArchive.h"

Z7_DEFINE_GUID(CLSID_CArchiveHandler,
    k_7zip_GUID_Data1,
    k_7zip_GUID_Data2,
    k_7zip_GUID_Data3_Common,
    0x10, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00);

STDAPI CreateCoder(const GUID* clsid, const GUID* iid, void** outObject);
STDAPI CreateHasher(const GUID* clsid, IHasher** hasher);
STDAPI CreateArchiver(const GUID* clsid, const GUID* iid, void** outObject);

// 与 7z.dll 导出的 CreateObject 签名一致，sevenzip_loader.cpp 可直接调用
STDAPI CreateObject(const GUID* clsid, const GUID* iid, void** outObject) {
  *outObject = NULL;
  if (*iid == IID_ICompressCoder || *iid == IID_ICompressCoder2 ||
      *iid == IID_ICompressFilter) {
    return CreateCoder(clsid, iid, outObject);
  }
  if (*iid == IID_IHasher) {
    return CreateHasher(clsid, reinterpret_cast<IHasher**>(outObject));
  }
  return CreateArchiver(clsid, iid, outObject);
}

STDAPI SetLargePageMode() { return S_OK; }

// g_CaseSensitive 已在 CPP/Common/Wildcard.cpp 里定义，这里只声明
extern bool g_CaseSensitive;

STDAPI SetCaseSensitive(Int32 caseSensitive);
STDAPI SetCaseSensitive(Int32 caseSensitive) {
  g_CaseSensitive = (caseSensitive != 0);
  return S_OK;
}

STDAPI SetCodecs(ICompressCodecsInfo*);
STDAPI SetCodecs(ICompressCodecsInfo*) { return S_OK; }
