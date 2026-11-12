#include "archive.h"

#include "sevenzip_loader.h"
#include "util.h"
#include "volumes.h"
#include "AiUI/platform.h"

#include <atomic>

namespace arc {
namespace {

// FILETIME 自 1601-01-01 起算，每单位 100ns；统一换算成 Unix 纪元毫秒
int64_t fileTimeToUnixMillis(const FILETIME& ft) {
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  constexpr uint64_t kEpochDiff = 116444736000000000ULL;
  if (value.QuadPart <= kEpochDiff) return 0;
  return static_cast<int64_t>((value.QuadPart - kEpochDiff) / 10000ULL);
}

// Unix 纪元毫秒 → FILETIME（1601 起的 100ns）；0 表示未知，返回全零
FILETIME unixMillisToFileTime(int64_t millis) {
  if (millis <= 0) return FILETIME{};
  constexpr uint64_t kEpochDiff = 116444736000000000ULL;
  ULARGE_INTEGER value{};
  value.QuadPart = static_cast<uint64_t>(millis) * 10000ULL + kEpochDiff;
  FILETIME ft{};
  ft.dwLowDateTime = value.LowPart;
  ft.dwHighDateTime = value.HighPart;
  return ft;
}

// ───────────────────────── 口令提供者 ─────────────────────────

// 密码状态：延迟询问一次，两个接口共用同一份凭据
struct PasswordState {
  std::wstring password;
  PasswordFn ask;
  bool asked = false;

  const std::wstring& Get() {
    if (password.empty() && !asked && ask) {
      asked = true;
      ask(password);
    }
    return password;
  }
};

// ICryptoGetTextPassword2 (5,0x11)：解压阶段用
class PasswordProvider final : public z7::ICryptoGetTextPassword2 {
 public:
  explicit PasswordProvider(PasswordState* src) : src_(src) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(5, 0x11)) {
      *ppv = static_cast<z7::ICryptoGetTextPassword2*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(int32_t* isDefined, BSTR* password) override {
    if (!password) return E_POINTER;
    *password = nullptr;
    const std::wstring& pwd = src_->Get();
    if (pwd.empty()) {
      if (isDefined) *isDefined = 0;
      return S_OK;
    }
    if (isDefined) *isDefined = 1;
    *password = ::SysAllocString(pwd.c_str());
    return S_OK;
  }

 private:
  PasswordState* src_;
  std::atomic<ULONG> refs_{1};
};

// ICryptoGetTextPassword (5,0x10)：7z 打开阶段只认这个老接口。
// 签名与 2 版不同（少一个 isDefined 参数），且占用同一个 vtable 槽位，
// 不能和上面的类共用对象，必须独立实现。
class PasswordProvider1 final : public IUnknown {
 public:
  explicit PasswordProvider1(PasswordState* src) : src_(src) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(5, 0x10)) {
      *ppv = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  // 与 ICryptoGetTextPassword2 的槽位对齐：CryptoGetTextPassword(BSTR*)
  virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) {
    if (!password) return E_POINTER;
    *password = nullptr;
    const std::wstring& pwd = src_->Get();
    if (pwd.empty()) return S_OK;
    *password = ::SysAllocString(pwd.c_str());
    return S_OK;
  }

 private:
  PasswordState* src_;
  std::atomic<ULONG> refs_{1};
};

// 持有状态与两个适配器；析构顺序保证适配器先于状态释放
class PasswordSource {
 public:
  PasswordSource(std::wstring initial, PasswordFn ask)
      : pwd2_(new PasswordProvider(&state_)), pwd1_(new PasswordProvider1(&state_)) {
    state_.password = std::move(initial);
    state_.ask = std::move(ask);
  }
  ~PasswordSource() {
    pwd2_->Release();
    pwd1_->Release();
  }
  PasswordSource(const PasswordSource&) = delete;
  PasswordSource& operator=(const PasswordSource&) = delete;

  // riid 命中密码接口时转发并返回 true
  bool QueryPasswordInterface(REFIID riid, void** ppv) {
    if (riid == z7::MakeIfaceGuid(5, 0x11)) {
      *ppv = static_cast<z7::ICryptoGetTextPassword2*>(pwd2_);
      pwd2_->AddRef();
      return true;
    }
    if (riid == z7::MakeIfaceGuid(5, 0x10)) {
      *ppv = static_cast<IUnknown*>(pwd1_);
      pwd1_->AddRef();
      return true;
    }
    return false;
  }

 private:
  PasswordState state_;
  PasswordProvider* pwd2_;
  PasswordProvider1* pwd1_;
};

// ───────────────────────── 文件流 ─────────────────────────
//
// 流对象包一层平台 FileStream：7z 内核要的是自己的 COM 接口，而打开文件
// 的方式（共享模式、顺序扫描提示）各平台不同，统一交给平台层。

// 7z 的 Seek 起点常量（STREAM_SEEK_SET / CUR / END）与平台枚举一一对应
AiUI::SeekOrigin seekOriginFrom(uint32_t origin) {
  switch (origin) {
    case 1:
      return AiUI::SeekOrigin::kCurrent;
    case 2:
      return AiUI::SeekOrigin::kEnd;
    default:
      return AiUI::SeekOrigin::kBegin;
  }
}

class FileInStream final : public z7::IInStream {
 public:
  explicit FileInStream(std::unique_ptr<AiUI::FileStream> stream) : stream_(std::move(stream)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(3, 0x03)) {
      *ppv = static_cast<z7::IInStream*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == z7::MakeIfaceGuid(3, 0x01)) {
      *ppv = static_cast<z7::ISequentialInStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  // 底层文件句柄的显式收口：7-Zip 用完输入流不一定 Release 到 0，
  // 析构可能迟迟不发生，而 Windows 上只要句柄还在就删不掉文件。
  void CloseHandleNow() { stream_.reset(); }
  HRESULT STDMETHODCALLTYPE Read(void* data, uint32_t size, uint32_t* processed) override {
    if (!stream_) {
      if (processed) *processed = 0;
      return E_FAIL;
    }
    const int64_t got = stream_->read(data, static_cast<int64_t>(size));
    if (got < 0) {
      if (processed) *processed = 0;
      return E_FAIL;
    }
    if (processed) *processed = static_cast<uint32_t>(got);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Seek(int64_t offset, uint32_t origin, uint64_t* newPos) override {
    if (!stream_) return E_FAIL;
    uint64_t position = 0;
    if (!stream_->seek(offset, seekOriginFrom(origin), &position)) return E_FAIL;
    if (newPos) *newPos = position;
    return S_OK;
  }

 private:
  std::unique_ptr<AiUI::FileStream> stream_;
  std::atomic<ULONG> refs_{1};
};

class FileOutStream final : public z7::IOutStream {
 public:
  explicit FileOutStream(std::unique_ptr<AiUI::FileStream> stream) : stream_(std::move(stream)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(3, 0x04)) {
      *ppv = static_cast<z7::IOutStream*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == z7::MakeIfaceGuid(3, 0x02)) {
      *ppv = static_cast<z7::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Write(const void* data, uint32_t size, uint32_t* processed) override {
    if (!stream_) {
      if (processed) *processed = 0;
      return E_FAIL;
    }
    const int64_t put = stream_->write(data, static_cast<int64_t>(size));
    if (put < 0) {
      if (processed) *processed = 0;
      return E_FAIL;
    }
    if (processed) *processed = static_cast<uint32_t>(put);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Seek(int64_t offset, uint32_t origin, uint64_t* newPos) override {
    if (!stream_) return E_FAIL;
    uint64_t position = 0;
    if (!stream_->seek(offset, seekOriginFrom(origin), &position)) return E_FAIL;
    if (newPos) *newPos = position;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(uint64_t newSize) override {
    return (stream_ && stream_->setSize(newSize)) ? S_OK : E_FAIL;
  }

 private:
  std::unique_ptr<AiUI::FileStream> stream_;
  std::atomic<ULONG> refs_{1};
};

// ───────────────────────── 属性读取 ─────────────────────────

std::wstring GetStringProp(z7::IInArchive* a, uint32_t idx, z7::PropId id) {
  PROPVARIANT v;
  ::PropVariantInit(&v);
  std::wstring out;
  if (a->GetProperty(idx, id, &v) == S_OK && v.vt == VT_BSTR && v.bstrVal) {
    out.assign(v.bstrVal, ::SysStringLen(v.bstrVal));
  }
  ::PropVariantClear(&v);
  return out;
}

bool GetUInt64Prop(z7::IInArchive* a, uint32_t idx, z7::PropId id, uint64_t& out) {
  PROPVARIANT v;
  ::PropVariantInit(&v);
  bool ok = false;
  if (a->GetProperty(idx, id, &v) == S_OK) {
    if (v.vt == VT_UI8) {
      out = v.uhVal.QuadPart;
      ok = true;
    } else if (v.vt == VT_UI4) {
      out = v.ulVal;
      ok = true;
    }
  }
  ::PropVariantClear(&v);
  return ok;
}

bool GetBoolProp(z7::IInArchive* a, uint32_t idx, z7::PropId id) {
  PROPVARIANT v;
  ::PropVariantInit(&v);
  bool out = false;
  if (a->GetProperty(idx, id, &v) == S_OK) {
    if (v.vt == VT_BOOL) out = v.boolVal != VARIANT_FALSE;
    else if (v.vt == VT_UI4) out = v.ulVal != 0;
  }
  ::PropVariantClear(&v);
  return out;
}

bool GetFileTimeProp(z7::IInArchive* a, uint32_t idx, z7::PropId id, int64_t& outMillis) {
  PROPVARIANT v;
  ::PropVariantInit(&v);
  bool ok = false;
  if (a->GetProperty(idx, id, &v) == S_OK && v.vt == VT_FILETIME) {
    outMillis = fileTimeToUnixMillis(v.filetime);
    ok = true;
  }
  ::PropVariantClear(&v);
  return ok;
}

// ───────────────────────── 打开回调 ─────────────────────────

class OpenCallback final : public z7::IArchiveOpenCallback {
 public:
  explicit OpenCallback(PasswordSource* pwd) : pwd_(pwd) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(6, 0x10)) {
      *ppv = static_cast<z7::IArchiveOpenCallback*>(this);
      AddRef();
      return S_OK;
    }
    if (pwd_ && pwd_->QueryPasswordInterface(riid, ppv)) return S_OK;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE SetTotal(const uint64_t*, const uint64_t*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t*, const uint64_t*) override { return S_OK; }

 private:
  PasswordSource* pwd_;
  std::atomic<ULONG> refs_{1};
};

// ───────────────────────── 解压回调 ─────────────────────────

class ExtractCallback final : public z7::IArchiveExtractCallback {
 public:
  ExtractCallback(const std::vector<Entry>& entries, const ExtractOptions& opts,
                  ProgressFn progress, OverwriteFn askOverwrite, PasswordSource* pwd)
      : entries_(entries),
        opts_(opts),
        progress_(std::move(progress)),
        askOverwrite_(std::move(askOverwrite)),
        pwd_(pwd) {}

  ~ExtractCallback() {
    if (outStream_) outStream_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(6, 0x20) ||
        riid == z7::MakeIfaceGuid(0, 0x05)) {
      *ppv = static_cast<z7::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    if (pwd_ && pwd_->QueryPasswordInterface(riid, ppv)) return S_OK;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE SetTotal(uint64_t total) override {
    if (total > 0) total_ = total;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t* complete) override {
    if (canceled_) return E_ABORT;
    if (progress_ && complete && !progress_(*complete, total_, currentPath_)) {
      canceled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetStream(uint32_t index, z7::ISequentialOutStream** outStream,
                                      int32_t askMode) override {
    if (canceled_) return E_ABORT;
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askMode != 0) return S_OK;

    if (index >= entries_.size()) return E_FAIL;
    const Entry& e = entries_[index];

    // 包内路径转落地相对路径，拒绝穿越
    std::wstring rel = e.path;
    for (auto& c : rel) {
      if (c == L'/') c = L'\\';
    }
    while (!rel.empty() && rel.front() == L'\\') rel.erase(rel.begin());
    if (rel.empty() || rel.find(L"..") != std::wstring::npos) return E_FAIL;
    currentPath_ = e.path;

    std::wstring target = util::JoinPath(opts_.targetDir, rel);

    if (e.isDir) {
      util::EnsureDirectory(target);
      currentSkip_ = true;
      currentEntry_ = nullptr;
      return S_OK;
    }

    util::EnsureDirectory(util::DirName(target));

    const bool exists = util::PathExists(target);
    if (exists && opts_.update == ExtractOptions::Update::kNewerOnly && e.hasMtime) {
      // 归档里的时间已经是 Unix 毫秒，直接和现有文件的修改时间比
      const int64_t existing = AiUI::platform()->modifiedTime(target);
      if (existing > 0 && existing >= e.modifiedMillis) {
        currentSkip_ = true;
        currentEntry_ = nullptr;
        return S_OK;
      }
    }

    if (exists) {
      int action;
      if (overwriteAll_ != 0) {
        action = overwriteAll_ > 0 ? 1 : 0;
      } else {
        switch (opts_.overwrite) {
          case ExtractOptions::Overwrite::kSkip:
            action = 0;
            break;
          case ExtractOptions::Overwrite::kOverwrite:
            action = 1;
            break;
          case ExtractOptions::Overwrite::kRename:
            action = 2;
            break;
          default:
            action = askOverwrite_ ? askOverwrite_(target) : 1;
            if (action == 3) {
              overwriteAll_ = 1;
              action = 1;
            } else if (action == 4) {
              overwriteAll_ = -1;
              action = 0;
            }
            break;
        }
      }
      if (action == 0) {
        currentSkip_ = true;
        currentEntry_ = nullptr;
        return S_OK;
      }
      if (action == 2) target = util::MakeUniquePath(target);
    }

    auto stream = AiUI::platform()->openFileStream(target, true);
    if (!stream) {
      currentSkip_ = true;
      currentEntry_ = nullptr;
      return S_OK;
    }
    if (outStream_) {
      outStream_->Release();
      outStream_ = nullptr;
    }
    outStream_ = new FileOutStream(std::move(stream));
    *outStream = static_cast<z7::ISequentialOutStream*>(outStream_);
    outStream_->AddRef();
    currentSkip_ = false;
    currentTarget_ = target;
    currentEntry_ = &e;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE PrepareOperation(int32_t) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE SetOperationResult(int32_t result) override {
    // 每个条目结束立刻关掉写入流，否则最后一个文件的句柄会一直被占着
    if (outStream_) {
      outStream_->Release();
      outStream_ = nullptr;
    }

    if (result == 0 && !currentSkip_ && currentEntry_ && currentEntry_->hasMtime &&
        !currentTarget_.empty()) {
      AiUI::platform()->setModifiedTime(currentTarget_, currentEntry_->modifiedMillis);
    }
    if (result != 0) errorCount_++;
    currentTarget_.clear();
    currentPath_.clear();
    currentEntry_ = nullptr;
    return S_OK;
  }

  int ErrorCount() const { return errorCount_; }
  bool Canceled() const { return canceled_; }

 private:
  const std::vector<Entry>& entries_;
  ExtractOptions opts_;
  ProgressFn progress_;
  OverwriteFn askOverwrite_;
  PasswordSource* pwd_;
  uint64_t total_ = 0;
  std::atomic<ULONG> refs_{1};
  z7::IOutStream* outStream_ = nullptr;
  std::wstring currentPath_;
  std::wstring currentTarget_;
  const Entry* currentEntry_ = nullptr;
  bool currentSkip_ = false;
  bool canceled_ = false;
  int overwriteAll_ = 0;
  int errorCount_ = 0;
};

// ───────────────────────── 压缩回调 ─────────────────────────

class UpdateCallback final : public z7::IArchiveUpdateCallback {
 public:
  UpdateCallback(const std::vector<util::SourceItem>& items, ProgressFn progress,
                 PasswordSource* pwd)
      : items_(items), progress_(std::move(progress)), pwd_(pwd) {
    for (const auto& it : items_) total_ += it.size;
  }

  ~UpdateCallback() {
    if (inStream_) inStream_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == z7::MakeIfaceGuid(6, 0x80) ||
        riid == z7::MakeIfaceGuid(0, 0x05)) {
      *ppv = static_cast<z7::IArchiveUpdateCallback*>(this);
      AddRef();
      return S_OK;
    }
    if (pwd_ && pwd_->QueryPasswordInterface(riid, ppv)) return S_OK;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE SetTotal(uint64_t total) override {
    if (total > 0) total_ = total;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t* complete) override {
    if (canceled_) return E_ABORT;
    if (progress_ && complete && !progress_(*complete, total_, currentPath_)) {
      canceled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(uint32_t index, int32_t* newData, int32_t* newProps,
                                              uint32_t* indexInArchive) override {
    if (index >= items_.size()) return E_INVALIDARG;
    if (newData) *newData = 1;
    if (newProps) *newProps = 1;
    if (indexInArchive) *indexInArchive = 0xFFFFFFFF;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetProperty(uint32_t index, z7::PropId propId,
                                        PROPVARIANT* value) override {
    if (!value) return E_POINTER;
    ::PropVariantInit(value);
    if (index >= items_.size()) return E_INVALIDARG;
    const util::SourceItem& it = items_[index];

    switch (propId) {
      case z7::kpidPath:
        value->vt = VT_BSTR;
        value->bstrVal = ::SysAllocString(it.relPath.c_str());
        break;
      case z7::kpidIsDir:
        value->vt = VT_BOOL;
        value->boolVal = it.isDir ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case z7::kpidSize:
        value->vt = VT_UI8;
        value->uhVal.QuadPart = it.size;
        break;
      case z7::kpidAttrib:
        value->vt = VT_UI4;
        value->ulVal = it.attributes;
        break;
      case z7::kpidMTime:
        value->vt = VT_FILETIME;
        value->filetime = unixMillisToFileTime(it.modifiedMillis);
        break;
      case z7::kpidIsAnti:
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_FALSE;
        break;
      default:
        break;  // 留作 VT_EMPTY
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetStream(uint32_t index, z7::ISequentialInStream** inStream) override {
    if (!inStream) return E_POINTER;
    *inStream = nullptr;
    if (index >= items_.size()) return E_INVALIDARG;
    const util::SourceItem& it = items_[index];
    if (it.isDir) return S_OK;

    auto stream = AiUI::platform()->openFileStream(it.fullPath, false);
    if (!stream) return E_FAIL;

    if (inStream_) {
      inStream_->Release();
      inStream_ = nullptr;
    }
    inStream_ = new FileInStream(std::move(stream));
    *inStream = static_cast<z7::ISequentialInStream*>(inStream_);
    inStream_->AddRef();
    currentPath_ = it.relPath;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetOperationResult(int32_t result) override {
    if (result != 0) errorCount_++;
    return S_OK;
  }

  int ErrorCount() const { return errorCount_; }
  bool Canceled() const { return canceled_; }

  // 7-Zip 用完 GetStream 给出的输入流未必 Release 到 0，句柄可能一直留着，
  // 导致源文件（尤其是两阶段压缩里的临时 tar）删不掉。压缩结束后显式关句柄。
  // inStream_ 只会由本类在 GetStream 里创建，静态转换安全。
  void CloseInputHandles() {
    if (inStream_) static_cast<FileInStream*>(inStream_)->CloseHandleNow();
  }

 private:
  const std::vector<util::SourceItem>& items_;
  ProgressFn progress_;
  PasswordSource* pwd_;
  uint64_t total_ = 0;
  std::atomic<ULONG> refs_{1};
  z7::IInStream* inStream_ = nullptr;
  std::wstring currentPath_;
  bool canceled_ = false;
  int errorCount_ = 0;
};

}  // namespace

// ───────────────────────── Archive ─────────────────────────

Archive::Archive() = default;

Archive::~Archive() { Close(); }

void Archive::Close() {
  if (archive_) {
    archive_->Close();
    archive_->Release();
    archive_ = nullptr;
  }
  if (stream_) {
    static_cast<z7::IInStream*>(stream_)->Release();
    stream_ = nullptr;
  }
  entries_.clear();
}

OpenResult Archive::Open(const std::wstring& filePath, const std::wstring& password) {
  OpenResult r;
  Close();

  auto* pwd = new PasswordSource(password, nullptr);
  auto* cb = new OpenCallback(pwd);

  // 分卷包（xxx.001）要把各卷拼成一条连续流再交给内核；
  // 卷路径不连续时只认已存在的连续段，缺卷会在解压时报校验错。
  z7::IInStream* inStream = nullptr;
  const std::vector<std::wstring> volumePaths = CollectVolumes(filePath);
  if (volumePaths.size() > 1) {
    std::vector<std::unique_ptr<AiUI::FileStream>> streams;
    streams.reserve(volumePaths.size());
    for (const std::wstring& path : volumePaths) {
      auto stream = AiUI::platform()->openFileStream(path, false);
      if (!stream) {
        cb->Release();
        delete pwd;
        r.error = L"无法打开分卷文件";
        return r;
      }
      streams.push_back(std::move(stream));
    }
    inStream = static_cast<z7::IInStream*>(new MultiVolumeInStream(std::move(streams)));
  } else {
    auto fileStream = AiUI::platform()->openFileStream(filePath, false);
    if (!fileStream) {
      cb->Release();
      delete pwd;
      r.error = L"无法打开文件";
      return r;
    }
    inStream = static_cast<z7::IInStream*>(new FileInStream(std::move(fileStream)));
  }

  const std::wstring ext = util::FileExtension(filePath);
  // 按扩展名猜的 handler 打头，其余 handler 依次兜底
  std::vector<GUID> candidates{z7::FormatFromExtension(ext)};
  for (const GUID& g : z7::kAllHandlers) {
    if (!::IsEqualGUID(g, candidates[0])) candidates.push_back(g);
  }

  // 分卷的总长由拼流自己算，不能只看首卷大小
  uint64_t size = AiUI::platform()->fileSize(filePath);
  if (volumePaths.size() > 1) {
    size = 0;
    for (const std::wstring& path : volumePaths) size += AiUI::platform()->fileSize(path);
  }
  const uint64_t maxCheck = size > 0 ? size : (1ull << 23);

  z7::IInArchive* opened = nullptr;
  for (const GUID& cls : candidates) {
    // 上一个 handler 可能已经把流指针读走，必须回到开头再试
    inStream->Seek(0, STREAM_SEEK_SET, nullptr);
    z7::IInArchive* a = z7::CreateInArchive(cls);
    if (!a) continue;
    if (a->Open(inStream, &maxCheck, cb) == S_OK) {
      opened = a;
      break;
    }
    a->Release();
  }
  cb->Release();
  delete pwd;

  if (!opened) {
    inStream->Release();
    r.error = L"不支持的压缩格式，或文件已损坏";
    return r;
  }

  archive_ = opened;
  stream_ = inStream;

  uint32_t count = 0;
  if (opened->GetNumberOfItems(&count) != S_OK) {
    Close();
    r.error = L"读取压缩包目录失败";
    return r;
  }

  entries_.clear();
  entries_.reserve(count);
  // 有些格式（bzip2、xz、lzma）本身不保存文件名，条目属性里没有 kpidPath。
  // 官方 7-Zip 用「压缩包主名去掉扩展名」当文件名，这里保持一致。
  const std::wstring fallbackName = util::FileStem(util::BaseName(filePath));
  for (uint32_t i = 0; i < count; ++i) {
    Entry e;
    e.path = GetStringProp(opened, i, z7::kpidPath);
    if (e.path.empty()) e.path = GetStringProp(opened, i, z7::kpidName);
    if (e.path.empty() && !fallbackName.empty()) e.path = fallbackName;
    if (e.path.empty()) continue;
    e.isDir = GetBoolProp(opened, i, z7::kpidIsDir);
    e.encrypted = GetBoolProp(opened, i, z7::kpidEncrypted);
    GetUInt64Prop(opened, i, z7::kpidSize, e.size);
    GetUInt64Prop(opened, i, z7::kpidPackSize, e.packSize);
    e.hasMtime = GetFileTimeProp(opened, i, z7::kpidMTime, e.modifiedMillis);
    e.method = GetStringProp(opened, i, z7::kpidMethod);

    uint64_t crc = 0;
    if (GetUInt64Prop(opened, i, z7::kpidCRC, crc)) {
      e.crc = static_cast<uint32_t>(crc);
      e.hasCrc = true;
    }
    entries_.push_back(std::move(e));
  }

  r.ok = true;
  return r;
}

bool Archive::Extract(const std::vector<uint32_t>& indices, const ExtractOptions& opts,
                      const ProgressFn& progress, const OverwriteFn& askOverwrite,
                      const PasswordFn& askPassword, std::wstring& error) {
  if (!archive_) {
    error = L"压缩包未打开";
    return false;
  }
  if (!util::EnsureDirectory(opts.targetDir)) {
    error = L"无法创建目标目录";
    return false;
  }

  auto* pwd = new PasswordSource(opts.password, askPassword);
  auto* cb = new ExtractCallback(entries_, opts, progress, askOverwrite, pwd);
  cb->AddRef();

  HRESULT hr;
  if (indices.empty()) {
    hr = archive_->Extract(nullptr, 0xFFFFFFFF, 0, cb);
  } else {
    std::vector<uint32_t> idx = indices;
    hr = archive_->Extract(idx.data(), static_cast<uint32_t>(idx.size()), 0, cb);
  }

  const int errors = cb->ErrorCount();
  const bool canceled = cb->Canceled();
  cb->Release();
  delete pwd;

  if (canceled) {
    error = L"操作已取消";
    return false;
  }
  if (hr != S_OK) {
    error = errors > 0 ? L"解压过程中出现错误" : L"解压失败（密码错误或数据损坏）";
    return false;
  }
  if (errors > 0) {
    error = L"部分文件解压失败";
    return false;
  }
  return true;
}

bool Archive::IsArchiveFile(const std::wstring& path) {
  const std::wstring ext = util::FileExtension(path);
  static const wchar_t* kExts[] = {L".zip",  L".7z",   L".rar",  L".tar", L".gz",
                                   L".tgz",  L".bz2",  L".tbz2", L".xz",  L".txz",
                                   L".cab",  L".iso",  L".wim",  L".jar", L".apk",
                                   L".epub", L".lzh",  L".arj",  L".z",   L".001"};
  for (const wchar_t* e : kExts) {
    if (::CompareStringOrdinal(ext.c_str(), -1, e, -1, TRUE) == CSTR_EQUAL) return true;
  }
  return false;
}

// ───────────────────────── 压缩 ─────────────────────────

// 单次压缩：按 clsId 生成一个容器，来源已是最终待打包列表
static bool CompressOnce(const std::vector<util::SourceItem>& items, const GUID& cls,
                         const std::wstring& outPath, int level, bool solid,
                         const std::wstring& password, uint64_t volumeSize,
                         const ProgressFn& progress, std::wstring& error) {
  z7::IOutArchive* out = z7::CreateOutArchive(cls);
  if (!out) {
    error = L"无法创建该格式的压缩器";
    return false;
  }

  // 算法参数
  z7::ISetProperties* sp = nullptr;
  if (out->QueryInterface(z7::kIidISetProperties, reinterpret_cast<void**>(&sp)) != S_OK) {
    sp = nullptr;
  }

  std::vector<std::wstring> nameStore;
  std::vector<PROPVARIANT> valStore;
  if (sp) {
    auto pushStr = [&](const wchar_t* name, const std::wstring& v) {
      nameStore.push_back(name);
      PROPVARIANT pv;
      ::PropVariantInit(&pv);
      pv.vt = VT_BSTR;
      pv.bstrVal = ::SysAllocString(v.c_str());
      valStore.push_back(pv);
    };
    auto pushU32 = [&](const wchar_t* name, uint32_t v) {
      nameStore.push_back(name);
      PROPVARIANT pv;
      ::PropVariantInit(&pv);
      pv.vt = VT_UI4;
      pv.ulVal = v;
      valStore.push_back(pv);
    };
    auto pushBool = [&](const wchar_t* name, bool v) {
      nameStore.push_back(name);
      PROPVARIANT pv;
      ::PropVariantInit(&pv);
      pv.vt = VT_BOOL;
      pv.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
      valStore.push_back(pv);
    };

    pushU32(L"x", static_cast<uint32_t>(level));
    if (::IsEqualGUID(cls, z7::kClsid7z)) pushBool(L"s", solid);
    if (!password.empty()) pushStr(L"p", password);

    std::vector<const wchar_t*> names;
    names.reserve(nameStore.size());
    for (const auto& n : nameStore) names.push_back(n.c_str());

    sp->SetProperties(names.data(), valStore.data(), static_cast<uint32_t>(nameStore.size()));
  }

  // 分卷时把同一条输出流摊到多个文件；不分卷仍走单文件，行为不变
  SplitOutStream* split = nullptr;
  z7::ISequentialOutStream* outStream = nullptr;
  if (volumeSize > 0) {
    // 上次可能是不分卷的同名包：主名文件留着会让打开时读到旧内容
    AiUI::platform()->deleteFile(outPath);
    // 旧卷可能比新卷多，先全清，否则尾部旧卷会被拼进新包
    RemoveVolumes(outPath);
    split = new SplitOutStream(outPath, volumeSize);
    // 两个引用：一个交给自己持有到函数末尾，一个交给 7-Zip
    split->AddRef();
    outStream = static_cast<z7::ISequentialOutStream*>(split);
  } else {
    // 上次分卷留下的 .001 起始卷要清掉，否则读取端会拼进旧数据
    RemoveVolumes(outPath);
    auto fileStream = AiUI::platform()->openFileStream(outPath, true);
    if (!fileStream) {
      for (auto& pv : valStore) ::PropVariantClear(&pv);
      if (sp) sp->Release();
      out->Release();
      error = L"无法创建压缩包文件";
      return false;
    }
    outStream = static_cast<z7::ISequentialOutStream*>(new FileOutStream(std::move(fileStream)));
  }

  auto* pwd = new PasswordSource(password, nullptr);
  auto* cb = new UpdateCallback(items, progress, pwd);
  cb->AddRef();

  const HRESULT hr = out->UpdateItems(outStream, static_cast<uint32_t>(items.size()), cb);

  const int errors = cb->ErrorCount();
  const bool canceled = cb->Canceled();
  // 关掉源文件句柄再 Release：7-Zip 可能没把输入流引用还回来，
  // 句柄留着会让后续 deleteFile（临时 tar）报 ERROR_SHARING_VIOLATION
  cb->CloseInputHandles();
  cb->Release();
  delete pwd;
  outStream->Release();
  // 卷数必须在 Release 之前取：引用归零会销毁对象
  const int volumes = split ? split->volumeCount() : 0;
  if (split) split->Release();
  if (sp) sp->Release();
  out->Release();
  for (auto& pv : valStore) ::PropVariantClear(&pv);

  // 失败时清理已写出的内容：分卷要删全部卷，单文件只删一个
  auto discard = [&] {
    if (split) {
      for (int i = 1; i <= volumes; ++i) AiUI::platform()->deleteFile(VolumePath(outPath, i));
    } else {
      AiUI::platform()->deleteFile(outPath);
    }
  };

  if (canceled) {
    discard();
    error = L"操作已取消";
    return false;
  }
  if (hr != S_OK || errors > 0) {
    discard();
    error = L"压缩失败";
    return false;
  }
  return true;
}

// 把一组来源包成 tar，落在临时文件里，供 TAR.GZ 之类的两阶段压缩使用
static bool MakeTarStaging(const std::vector<util::SourceItem>& items, const std::wstring& tarPath,
                           std::wstring& error) {
  return CompressOnce(items, z7::kClsidTar, tarPath, 0, false, L"", 0, nullptr, error);
}

bool Compress(const std::vector<std::wstring>& sources, const CompressOptions& opts,
              const ProgressFn& progress, std::wstring& error) {
  std::vector<util::SourceItem> items;
  util::CollectSources(sources, items);
  if (items.empty()) {
    error = L"没有可压缩的文件";
    return false;
  }

  // 让调用方指定的格式决定容器，而不是靠扩展名猜
  const z7::FormatInfo* fi = nullptr;
  if (!opts.formatName.empty()) {
    const int idx = z7::FormatIndexByName(opts.formatName.c_str());
    if (idx >= 0) fi = &z7::Formats()[idx];
  }
  const GUID cls = fi ? fi->clsId : z7::FormatFromExtension(util::FileExtension(opts.archivePath));

  // TAR.GZ / TAR.BZ2：先打 tar，再整体压一层
  if (fi && fi->tarWrapped) {
    // 进程号 + 单调时钟 + 自增序号：时钟是 GetTickCount64（约 15ms 粒度），
    // 同一进程连着压两个 tar 组合格式会撞在同一毫秒上，只靠时钟不够。
    static std::atomic<uint32_t> stagingSeq{0};
    const std::wstring tarPath =
        util::JoinPath(AiUI::platform()->tempDir(),
                       L"aizip-" + std::to_wstring(AiUI::platform()->processId()) + L"-" +
                           std::to_wstring(AiUI::platform()->monotonicMillis()) + L"-" +
                           std::to_wstring(stagingSeq.fetch_add(1)) + L".tar");
    bool ok = MakeTarStaging(items, tarPath, error);
    if (ok) {
      std::vector<util::SourceItem> one;
      util::SourceItem it;
      it.fullPath = tarPath;
      // 包内名用归档主名，解压后得到 xxx.tar
      std::wstring innerName = util::BaseName(opts.archivePath);
      const size_t dot = innerName.find(L".tar.");
      it.relPath = dot == std::wstring::npos ? innerName : innerName.substr(0, dot) + L".tar";
      // 中间 tar 的元数据走平台接口取
      it.size = AiUI::platform()->fileSize(tarPath);
      it.modifiedMillis = AiUI::platform()->modifiedTime(tarPath);
      one.push_back(it);
      ok = CompressOnce(one, cls, opts.archivePath, opts.level, false, opts.password,
                        opts.volumeSize, progress, error);
    }
    // 删除可能失败（句柄还没关，或杀软短暂占用）：重试几次，别把 GB 级临时文件留在 temp
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (AiUI::platform()->deleteFile(tarPath)) break;
      AiUI::platform()->sleepMillis(20);
    }
    return ok;
  }

  // GZIP / BZIP2 / XZ 的 handler 只接受单文件，多文件或多出目录会直接失败。
  // 这里提前挡掉，给出可读的提示，而不是让内核返回“压缩失败”。
  if (fi && fi->singleFile && items.size() != 1) {
    error = std::wstring(fi->name) +
            L" 格式一次只能压缩一个文件；多个文件请改用 TAR." +
            (::IsEqualGUID(cls, z7::kClsidGZip) ? L"GZ" : (::IsEqualGUID(cls, z7::kClsidBZip2) ? L"BZ2" : L"XZ"));
    return false;
  }

  const int level = (fi && fi->maxLevel == 0) ? 0 : opts.level;
  return CompressOnce(items, cls, opts.archivePath, level, opts.solid, opts.password,
                      opts.volumeSize, progress, error);
}

}  // namespace arc
