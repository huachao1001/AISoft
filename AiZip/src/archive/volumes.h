// 分卷读写：把一条连续流映射成 name.001 / name.002 …
//
// 7-Zip 内核不做切卷：官方的 -v 是外壳自己把输出流按边界摊到多个文件。
// 这里补同样的一层——写入端 SplitOutStream 在卷满时换文件，读取端
// MultiVolumeInStream 反向拼回去，两边共用同一套「虚拟偏移 → 卷号」算法，
// 因此不必落地临时合并文件。
#pragma once

#include "sevenzip.h"
#include "sevenzip_loader.h"

#include "AiUI/platform.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arc {

// 分卷命名：basePath 追加 ".001"、".002"……三位起步，超过 999 自然变长
std::wstring VolumePath(const std::wstring& basePath, int index);

// path 是分卷首卷（xxx.001）时返回全部卷路径，否则返回空。
// 序号连续到第一个缺口为止：分卷包缺卷没有意义，多出来的孤立文件不该算进去。
std::vector<std::wstring> CollectVolumes(const std::wstring& path);

// 删掉 basePath 名下所有已存在的卷（basePath.001 起，到第一个缺口为止）。
// 重新压缩前必须先清干净：旧卷会多于新卷，读取端拼包时会把旧数据接在后面。
void RemoveVolumes(const std::wstring& basePath);

// 把输出切到多个卷。总量不可预知，卷在首次写入时才创建。
class SplitOutStream final : public z7::IOutStream {
 public:
  SplitOutStream(std::wstring basePath, uint64_t volumeSize);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE Write(const void* data, uint32_t size, uint32_t* processed) override;
  HRESULT STDMETHODCALLTYPE Seek(int64_t offset, uint32_t origin, uint64_t* newPos) override;
  HRESULT STDMETHODCALLTYPE SetSize(uint64_t newSize) override;

  // 实际用到的卷数；取消或失败时由调用方按这个数量清理
  int volumeCount() const { return static_cast<int>(volumes_.size()); }

 private:
  // 取第 index 卷，不存在就创建；失败返回 nullptr
  AiUI::FileStream* volumeAt(int index);
  // 关掉并删除第 index 卷之后的全部卷，用于 SetSize 回缩
  void dropAfter(int index);

  std::wstring basePath_;
  uint64_t volumeSize_ = 0;
  uint64_t total_ = 0;
  uint64_t pos_ = 0;
  std::vector<std::unique_ptr<AiUI::FileStream>> volumes_;
  std::atomic<ULONG> refs_{1};
};

// 把多个卷文件当成一条连续流读，供 7-Zip 打开分卷包
class MultiVolumeInStream final : public z7::IInStream {
 public:
  explicit MultiVolumeInStream(std::vector<std::unique_ptr<AiUI::FileStream>> volumes);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE Read(void* data, uint32_t size, uint32_t* processed) override;
  HRESULT STDMETHODCALLTYPE Seek(int64_t offset, uint32_t origin, uint64_t* newPos) override;

 private:
  // 定位到包含 pos_ 的卷；返回卷下标，越界返回 -1
  int volumeFor(uint64_t pos) const;

  std::vector<std::unique_ptr<AiUI::FileStream>> volumes_;
  std::vector<uint64_t> starts_;  // 每卷在虚拟空间里的起始偏移
  uint64_t total_ = 0;
  uint64_t pos_ = 0;
  std::atomic<ULONG> refs_{1};
};

}  // namespace arc
