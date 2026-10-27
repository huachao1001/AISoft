// 7-Zip 内核的最小 COM 接口声明（原 dll 导出，现静态链接）。
// 方法顺序严格对应 7-Zip 26.05 SDK 的 Z7_IFACEM_* 宏展开，
// 只声明本项目用到的部分，避免引入整套 SDK 头文件。
#pragma once

#include <windows.h>
#include <propidl.h>
#include <cstdint>

namespace z7 {

using UInt32 = uint32_t;
using Int32 = int32_t;
using UInt64 = uint64_t;
using Int64 = int64_t;

// 属性 ID，取自 SDK PropID.h 的枚举顺序
enum PropId : UInt32 {
  kpidPath = 3,
  kpidName = 4,
  kpidIsDir = 6,
  kpidSize = 7,
  kpidPackSize = 8,
  kpidAttrib = 9,
  kpidMTime = 12,
  kpidEncrypted = 15,
  kpidCRC = 19,
  kpidIsAnti = 21,
  kpidMethod = 22,
  kpidComment = 28,
  kpidNumSubFiles = 32,
};

struct IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;

 protected:
  ~IUnknown7() = default;
};

struct IProgress7 : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* complete) = 0;
};

struct ISequentialInStream : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) = 0;
};

struct ISequentialOutStream : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* processed) = 0;
};

struct IInStream : ISequentialInStream {
  virtual HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) = 0;
};

struct IOutStream : ISequentialOutStream {
  virtual HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetSize(UInt64 newSize) = 0;
};

struct ICryptoGetTextPassword2 : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32* passwordIsDefined,
                                                           BSTR* password) = 0;
};

struct IArchiveOpenCallback : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE SetTotal(const UInt64* files, const UInt64* bytes) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* files, const UInt64* bytes) = 0;
};

struct IArchiveExtractCallback : IProgress7 {
  virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream** outStream,
                                              Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 opRes) = 0;
};

struct IInArchive : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE Open(IInStream* stream, const UInt64* maxCheckStart,
                                        IArchiveOpenCallback* cb) = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32* numItems) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PropId propId,
                                                PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE Extract(const UInt32* indices, UInt32 numItems, Int32 testMode,
                                            IArchiveExtractCallback* cb) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PropId propId, PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32 index, BSTR* name, PropId* propId,
                                                    VARTYPE* varType) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32 index, BSTR* name, PropId* propId,
                                                           VARTYPE* varType) = 0;
};

struct IArchiveUpdateCallback : IProgress7 {
  virtual HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 index, Int32* newData, Int32* newProps,
                                                      UInt32* indexInArchive) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PropId propId,
                                                PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialInStream** inStream) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 opRes) = 0;
};

struct IOutArchive : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE UpdateItems(ISequentialOutStream* outStream, UInt32 numItems,
                                                IArchiveUpdateCallback* cb) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFileTimeType(UInt32* type) = 0;
};

struct ISetProperties : IUnknown7 {
  virtual HRESULT STDMETHODCALLTYPE SetProperties(const wchar_t* const* names,
                                                  const PROPVARIANT* values, UInt32 numProps) = 0;
};

using CreateObjectFn = HRESULT(WINAPI*)(const GUID* clsid, const GUID* iid, void** outObject);

}  // namespace z7
