// 用 7-Zip 的 deflate 解码器解一段 raw deflate 流。
//
// ── 为什么单独一个文件 ──
//
// 只有这里会 include 7z 的头。把「一个第三方库的接入点」收在一个编译单元，
// 换实现（内嵌别的 inflate）时只改这一个文件，不动 ZIP 解析。
//
// ── 为什么 #undef WIN32_LEAN_AND_MEAN ──
//
// 顶层 CMakeLists 全局加了它（为了减小核心库体积）。7-Zip 的 COM 声明依赖
// <windows.h> 里的 OLE 部分，被裁掉后是一屏「STDMETHOD 缺少返回类型」。
// 这里在自己的 TU 里先关掉：比在命令行加 /U 干净（后者与 /D 冲突并报
// D9025），也没有副作用——本文件不属于体积敏感的交付件。
#undef WIN32_LEAN_AND_MEAN

// INITGUID 必须恰好在一个编译单元里定义：7z 用 Z7_DEFINE_GUID 声明接口
// IID，不定义它得到的是外部引用，链接期找不到符号。
#define INITGUID

#include "pcc/backend/zip.h"

#include "7zip/Common/InBuffer.h"
#include "7zip/Compress/DeflateDecoder.h"

#include <cstring>

namespace pcc {
namespace {

// 7z 的流接口是 COM 形状的。这两个适配器把「一段内存」包成 7z 认识的流。
// 输出不按块回调而是攒起来：`.pylib` 的条目都是小文件（静态库、字节码）。
class MemoryInStream : public ISequentialInStream, public CMyUnknownImp {
 public:
  MemoryInStream(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}

  Z7_COM_QI_BEGIN2(ISequentialInStream)
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  HRESULT STDMETHODCALLTYPE Read(void* buffer, UInt32 size,
                                 UInt32* processed) throw() override {
    if (processed != nullptr) *processed = 0;
    if (position_ >= size_) return S_OK;
    std::size_t take = size_ - position_;
    if (take > size) take = size;
    std::memcpy(buffer, data_ + position_, take);
    position_ += take;
    if (processed != nullptr) *processed = static_cast<UInt32>(take);
    return S_OK;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

class VectorOutStream : public ISequentialOutStream, public CMyUnknownImp {
 public:
  explicit VectorOutStream(std::vector<std::uint8_t>* out) : out_(out) {}

  Z7_COM_QI_BEGIN2(ISequentialOutStream)
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  HRESULT STDMETHODCALLTYPE Write(const void* buffer, UInt32 size,
                                  UInt32* processed) throw() override {
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(buffer);
    out_->insert(out_->end(), bytes, bytes + size);
    if (processed != nullptr) *processed = size;
    return S_OK;
  }

 private:
  std::vector<std::uint8_t>* out_;
};

}  // namespace

bool inflateRaw(const std::uint8_t* data, std::size_t size,
                std::vector<std::uint8_t>* out, std::string* error) {
  if (size == 0) {
    *error = "empty deflate stream";
    return false;
  }

  out->clear();

  // CCOMCoder 是 7z 导出的 deflate 解码器（非 deflate64）。
  NCompress::NDeflate::NDecoder::CCOMCoder* coder =
      new NCompress::NDeflate::NDecoder::CCOMCoder();
  CMyComPtr<ICompressCoder> comCoder = coder;

  CMyComPtr<ISequentialInStream> in(new MemoryInStream(data, size));
  CMyComPtr<ISequentialOutStream> outStream(new VectorOutStream(out));

  const HRESULT result = comCoder->Code(in, outStream, nullptr, nullptr, nullptr);
  if (result != S_OK) {
    char text[64];
    std::snprintf(text, sizeof(text), "deflate error 0x%08x",
                  static_cast<unsigned>(result));
    *error = text;
    return false;
  }
  return true;
}

}  // namespace pcc
