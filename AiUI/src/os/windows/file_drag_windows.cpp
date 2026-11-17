// 往外拖拽（Windows）
//
// 应用给一份路径清单，这里把它包成 CF_HDROP 的 IDataObject，再用 DoDragDrop
// 交给系统接管鼠标。放下位置、目标程序、是复制还是移动，全部由系统与目标
// 程序协商，应用只提供"拖的是什么"。
//
// 为什么不用 WM_DROPFILES 的反向操作：那个消息只负责"接住别人拖进来的"，
// 往外拖没有对应的消息，必须自己实现 OLE 拖放的两个接口。
#include "backend.h"
#include "windows_backend.h"

#include <objidl.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <vector>

namespace AiUI {
namespace os {
namespace windows {
namespace {

// 拖拽用的数据对象：只提供 CF_HDROP 一种格式。
//
// 目标程序按自己需要的格式来问，资源管理器只认 CF_HDROP（一串绝对路径）。
// 其它格式（CF_UNICODETEXT 等）不提供——多给格式反而会让某些目标程序
// 拿着纯文本路径当内容插入，结果是一段文字而不是文件。
class FileDataObject final : public IDataObject {
 public:
  explicit FileDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

  // ── IUnknown ──
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // ── IDataObject ──
  IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_INVALIDARG;
    if (!isDropFiles(*format)) return DV_E_FORMATETC;

    const HGLOBAL memory = buildDropFiles(paths_);
    if (!memory) return E_OUTOFMEMORY;

    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    // pUnkForRelease 为空：接收方用完自己 ReleaseStgMedium 释放 hGlobal
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  // 不主动提供数据：拖拽过程中目标程序只在自己需要时来要
  IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

  IFACEMETHODIMP QueryGetData(FORMATETC* format) override {
    if (!format) return E_INVALIDARG;
    return isDropFiles(*format) ? S_OK : DV_E_FORMATETC;
  }

  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }

  IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

  // 枚举所有可提供格式：只有一个 CF_HDROP，目标程序靠它判断能不能接
  IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;

    FORMATETC format = dropFilesFormat();
    return ::SHCreateStdEnumFmtEtc(1, &format, out);
  }

  // 拖放期间的通知（放下、悬停等）不需要处理
  IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  static FORMATETC dropFilesFormat() {
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return format;
  }

  static bool isDropFiles(const FORMATETC& format) {
    return format.cfFormat == CF_HDROP && format.dwAspect == DVASPECT_CONTENT &&
           (format.tymed & TYMED_HGLOBAL) != 0 &&
           // lindex 必须是 -1：CF_HDROP 没有"第几份"，别的值说明目标程序问错了
           format.lindex == -1;
  }

  // 按 DROPFILES 结构打包：一段头部 + 一串以 '\0' 分隔、以 '\0\0' 结尾的路径
  static HGLOBAL buildDropFiles(const std::vector<std::wstring>& paths) {
    size_t chars = 0;
    for (const auto& p : paths) chars += p.size() + 1;
    chars += 1;  // 结尾那个额外的 '\0'

    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return nullptr;

    auto* drop = static_cast<DROPFILES*>(::GlobalLock(memory));
    if (!drop) {
      ::GlobalFree(memory);
      return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;  // 宽字符路径：中文与超长路径都不能靠 ANSI 代码页

    auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    for (const auto& p : paths) {
      ::memcpy(cursor, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
      cursor += p.size() + 1;
    }
    *cursor = L'\0';
    ::GlobalUnlock(memory);
    return memory;
  }

  std::vector<std::wstring> paths_;
  LONG refs_ = 1;
};

// 拖拽源：只允许复制。
//
// 内容是发起方随时可能回收的临时副本，被"移动"走会让镜像里少文件；而且
// 从压缩包里往外拖，语义本来就该是"导出到那里"，不是"把包里的内容搬走"。
class FileDropSource final : public IDropSource {
 public:
  FileDropSource() = default;

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *ppv = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // 是否开始拖拽：必须真的把鼠标移开一段，才不算误触（点一下列表项不该
  // 变成拖拽）。系统只在按住左键时调用。
  IFACEMETHODIMP QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
    return S_OK;
  }

  // 光标反馈：允许的操作集里只有"复制"，目标程序据此只显示复制光标
  IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

 private:
  LONG refs_ = 1;
};

}  // namespace

bool WindowsBackend::startFileDrag(const FileDragDesc& desc) {
  if (desc.paths.empty() || !desc.sourceWindow) return false;

  auto* data = new FileDataObject(desc.paths);
  auto* source = new FileDropSource();

  // 只允许复制：拖出来的是临时副本，被目标程序"移动"走会破坏发起方的镜像，
  // 而且从压缩包里拖出的本意就是导出，不是把包内内容搬走。
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT hr = ::DoDragDrop(data, source, DROPEFFECT_COPY, &effect);

  data->Release();
  source->Release();
  // DRAGDROP_S_DROP / DRAGDROP_S_CANCEL 都算正常结束；只有真出错才返回 false
  return hr == DRAGDROP_S_DROP || hr == DRAGDROP_S_CANCEL || SUCCEEDED(hr);
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
