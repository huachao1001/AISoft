// 分卷流实现
#include "volumes.h"

#include "util.h"

#include <algorithm>

namespace arc {
namespace {

// STREAM_SEEK_* 的取值，避免在应用侧引入 windows.h
constexpr uint32_t kSeekBegin = 0;
constexpr uint32_t kSeekCurrent = 1;
constexpr uint32_t kSeekEnd = 2;

}  // namespace

std::wstring VolumePath(const std::wstring& basePath, int index) {
  // 三位起步，超过 999 自动加宽；补零是为了资源管理器按名字排序即卷序
  std::wstring digits = std::to_wstring(index);
  while (digits.size() < 3) digits.insert(digits.begin(), L'0');
  return basePath + L"." + digits;
}

std::vector<std::wstring> CollectVolumes(const std::wstring& path) {
  std::vector<std::wstring> out;
  // 首卷必须是 .001：从 .002 起打开没有意义，7-Zip 也要求从首卷开始
  const std::wstring first = path;
  if (!util::PathExists(first)) return out;
  if (util::FileExtension(first) != L".001") return out;

  const std::wstring base = first.substr(0, first.size() - 4);
  out.push_back(first);
  for (int i = 2; i < 100000; ++i) {
    const std::wstring next = VolumePath(base, i);
    if (!util::PathExists(next)) break;
    out.push_back(next);
  }
  return out;
}

void RemoveVolumes(const std::wstring& basePath) {
  // 不用 CollectVolumes：它要求首卷扩展名恰好是 .001，
  // 而这里只需要清掉「接着序号存在的那些」
  for (int i = 1; i < 100000; ++i) {
    const std::wstring path = VolumePath(basePath, i);
    if (!util::PathExists(path)) break;
    AiUI::platform()->deleteFile(path);
  }
}

// ───────────────────────── SplitOutStream ─────────────────────────

SplitOutStream::SplitOutStream(std::wstring basePath, uint64_t volumeSize)
    : basePath_(std::move(basePath)), volumeSize_(volumeSize ? volumeSize : 1) {}

HRESULT STDMETHODCALLTYPE SplitOutStream::QueryInterface(REFIID riid, void** ppv) {
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

ULONG STDMETHODCALLTYPE SplitOutStream::AddRef() { return ++refs_; }

ULONG STDMETHODCALLTYPE SplitOutStream::Release() {
  const ULONG n = --refs_;
  if (n == 0) delete this;
  return n;
}

AiUI::FileStream* SplitOutStream::volumeAt(int index) {
  if (index < 0) return nullptr;
  while (static_cast<int>(volumes_.size()) <= index) {
    const int next = static_cast<int>(volumes_.size());
    auto stream = AiUI::platform()->openFileStream(VolumePath(basePath_, next + 1), true);
    if (!stream) return nullptr;
    volumes_.push_back(std::move(stream));
  }
  return volumes_[static_cast<size_t>(index)].get();
}

void SplitOutStream::dropAfter(int index) {
  // 从尾部删：先关句柄再删文件，Windows 上句柄没关会被占用
  while (static_cast<int>(volumes_.size()) > index + 1) {
    volumes_.pop_back();
    AiUI::platform()->deleteFile(VolumePath(basePath_, static_cast<int>(volumes_.size()) + 1));
  }
}

HRESULT STDMETHODCALLTYPE SplitOutStream::Write(const void* data, uint32_t size,
                                                uint32_t* processed) {
  if (processed) *processed = 0;
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t written = 0;

  while (written < size) {
    const int index = static_cast<int>(pos_ / volumeSize_);
    AiUI::FileStream* stream = volumeAt(index);
    if (!stream) return E_FAIL;

    // 单次写入不跨卷：跨卷时只写满当前卷剩余部分，下一轮换卷
    const uint64_t offsetInVolume = pos_ % volumeSize_;
    const uint64_t room = volumeSize_ - offsetInVolume;
    const uint64_t chunk = (std::min)(room, static_cast<uint64_t>(size - written));
    if (!stream->seek(static_cast<int64_t>(offsetInVolume), AiUI::SeekOrigin::kBegin, nullptr)) {
      return E_FAIL;
    }
    const int64_t put = stream->write(bytes + written, static_cast<int64_t>(chunk));
    if (put <= 0) return E_FAIL;

    written += static_cast<uint32_t>(put);
    pos_ += static_cast<uint64_t>(put);
    total_ = (std::max)(total_, pos_);
  }

  if (processed) *processed = written;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE SplitOutStream::Seek(int64_t offset, uint32_t origin,
                                               uint64_t* newPos) {
  uint64_t target = 0;
  if (origin == kSeekBegin) {
    if (offset < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(offset);
  } else if (origin == kSeekCurrent) {
    const int64_t next = static_cast<int64_t>(pos_) + offset;
    if (next < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(next);
  } else if (origin == kSeekEnd) {
    const int64_t next = static_cast<int64_t>(total_) + offset;
    if (next < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(next);
  } else {
    return E_INVALIDARG;
  }

  // 7-Zip 会先跳到很靠后的位置再回来填内容（zip 的中央目录），
  // 这里只移动逻辑位置，真正落盘仍由 Write 触发并补齐中间卷
  pos_ = target;
  total_ = (std::max)(total_, target);
  if (newPos) *newPos = pos_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE SplitOutStream::SetSize(uint64_t newSize) {
  total_ = newSize;
  if (pos_ > newSize) pos_ = newSize;

  const int keep = static_cast<int>((newSize + volumeSize_ - 1) / volumeSize_);
  dropAfter(keep - 1);
  if (keep <= 0) return S_OK;

  AiUI::FileStream* last = volumeAt(keep - 1);
  if (!last) return E_FAIL;
  // 末卷长度由虚拟总长决定，多出来的部分要截掉
  const uint64_t lastSize = newSize - static_cast<uint64_t>(keep - 1) * volumeSize_;
  return last->setSize(lastSize) ? S_OK : E_FAIL;
}

// ───────────────────────── MultiVolumeInStream ─────────────────────────

MultiVolumeInStream::MultiVolumeInStream(std::vector<std::unique_ptr<AiUI::FileStream>> volumes)
    : volumes_(std::move(volumes)) {
  starts_.reserve(volumes_.size());
  uint64_t offset = 0;
  for (const auto& stream : volumes_) {
    starts_.push_back(offset);
    // 卷大小靠 seek 到尾部取得，FileStream 没有直接的 size()
    uint64_t end = 0;
    if (stream->seek(0, AiUI::SeekOrigin::kEnd, &end)) offset += end;
  }
  total_ = offset;
  // 卷内偏移从各自开头算，重置一遍，避免留着上次 seek 的位置
  for (auto& stream : volumes_) stream->seek(0, AiUI::SeekOrigin::kBegin, nullptr);
}

HRESULT STDMETHODCALLTYPE MultiVolumeInStream::QueryInterface(REFIID riid, void** ppv) {
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

ULONG STDMETHODCALLTYPE MultiVolumeInStream::AddRef() { return ++refs_; }

ULONG STDMETHODCALLTYPE MultiVolumeInStream::Release() {
  const ULONG n = --refs_;
  if (n == 0) delete this;
  return n;
}

int MultiVolumeInStream::volumeFor(uint64_t pos) const {
  // 卷数很少（几十个量级），线性扫比二分更省心
  for (int i = static_cast<int>(starts_.size()) - 1; i >= 0; --i) {
    if (pos >= starts_[static_cast<size_t>(i)]) return i;
  }
  return -1;
}

HRESULT STDMETHODCALLTYPE MultiVolumeInStream::Read(void* data, uint32_t size,
                                                   uint32_t* processed) {
  if (processed) *processed = 0;
  if (pos_ >= total_) return S_OK;

  auto* out = static_cast<uint8_t*>(data);
  uint32_t done = 0;
  while (done < size && pos_ < total_) {
    const int index = volumeFor(pos_);
    if (index < 0) break;
    AiUI::FileStream& stream = *volumes_[static_cast<size_t>(index)];

    const uint64_t offsetInVolume = pos_ - starts_[static_cast<size_t>(index)];
    const uint64_t volumeEnd = (index + 1 < static_cast<int>(starts_.size()))
                                   ? starts_[static_cast<size_t>(index) + 1]
                                   : total_;
    const uint64_t room = volumeEnd - pos_;
    const uint64_t chunk = (std::min)(room, static_cast<uint64_t>(size - done));

    if (!stream.seek(static_cast<int64_t>(offsetInVolume), AiUI::SeekOrigin::kBegin, nullptr)) {
      return E_FAIL;
    }
    const int64_t got = stream.read(out + done, static_cast<int64_t>(chunk));
    if (got < 0) return E_FAIL;
    if (got == 0) break;  // 卷被截短：交给上层按总长判断

    done += static_cast<uint32_t>(got);
    pos_ += static_cast<uint64_t>(got);
  }

  if (processed) *processed = done;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MultiVolumeInStream::Seek(int64_t offset, uint32_t origin,
                                                    uint64_t* newPos) {
  uint64_t target = 0;
  if (origin == kSeekBegin) {
    if (offset < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(offset);
  } else if (origin == kSeekCurrent) {
    const int64_t next = static_cast<int64_t>(pos_) + offset;
    if (next < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(next);
  } else if (origin == kSeekEnd) {
    const int64_t next = static_cast<int64_t>(total_) + offset;
    if (next < 0) return E_INVALIDARG;
    target = static_cast<uint64_t>(next);
  } else {
    return E_INVALIDARG;
  }

  pos_ = target;
  if (newPos) *newPos = pos_;
  return S_OK;
}

}  // namespace arc
