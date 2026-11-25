// 打包/解包 exe 的实现。
//
// ── 为什么先写临时文件再改名 ──
//
// 中途失败不会留下「看起来像 exe、实际截断」的产物。Windows 上还会挡住
// 直接覆盖正在运行的同名 exe，改名顺带绕开这一点。
//
// ── 为什么要检查 launcher 本身没被打包过 ──
//
// 对一个已经打包过的 exe 再打包会产出双层嵌套，而它仍然能跑（读最内层），
// 症状是「改了脚本但输出没变」——这类静默错误最难查，所以在入口就拦住。
#include "pcc/backend/bundle.h"

#include "pcc/backend/pack.h"
#include "pcc/diagnostics.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace pcc {
namespace {

bool readWholeFile(const std::string& path, std::vector<std::uint8_t>* out) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) return false;
  std::fseek(handle, 0, SEEK_END);
  const long size = std::ftell(handle);
  if (size < 0) {
    std::fclose(handle);
    return false;
  }
  std::fseek(handle, 0, SEEK_SET);
  out->resize(static_cast<std::size_t>(size));
  const std::size_t got =
      size == 0 ? 0 : std::fread(out->data(), 1, out->size(), handle);
  std::fclose(handle);
  return got == out->size();
}

bool fileExists(const std::string& path) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) return false;
  std::fclose(handle);
  return true;
}

void writeU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
  }
}

std::uint64_t readU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

}  // namespace

bool readTrailer(const std::string& path, std::uint64_t* length) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) return false;

  std::fseek(handle, 0, SEEK_END);
  const long size = std::ftell(handle);
  if (size < static_cast<long>(kTrailerSize)) {
    std::fclose(handle);
    return false;
  }

  std::fseek(handle, size - static_cast<long>(kTrailerSize), SEEK_SET);
  std::uint8_t trailer[kTrailerSize] = {0};
  const std::size_t got = std::fread(trailer, 1, kTrailerSize, handle);
  std::fclose(handle);
  if (got != kTrailerSize) return false;
  if (std::memcmp(trailer, kBundleMagic, 8) != 0) return false;

  const std::uint64_t payload = readU64(trailer + 8);
  // 长度越界说明文件被截断过。当作没有索引，让调用方报清晰的错误，
  // 而不是按一个错误的偏移去读。
  if (payload == 0 || payload > static_cast<std::uint64_t>(size) - kTrailerSize) {
    return false;
  }
  *length = payload;
  return true;
}

bool bundleExecutable(const std::string& launcher,
                      const std::string& bytecode,
                      const std::string& output, std::uint64_t* size,
                      std::string* error) {
  if (!fileExists(launcher)) {
    *error = diag::fileNotFound(launcher);
    return false;
  }
  if (!fileExists(bytecode)) {
    *error = diag::fileNotFound(bytecode);
    return false;
  }

  std::uint64_t existing = 0;
  if (readTrailer(launcher, &existing)) {
    *error = diag::alreadyBundled(launcher);
    return false;
  }

  std::vector<std::uint8_t> payload;
  if (!readWholeFile(bytecode, &payload)) {
    *error = diag::fileNotFound(bytecode);
    return false;
  }
  if (payload.empty()) {
    *error = diag::emptyBytecode(bytecode);
    return false;
  }
  if (payload.size() < sizeof(kBytecodeMagic) ||
      std::memcmp(payload.data(), kBytecodeMagic, sizeof(kBytecodeMagic)) != 0) {
    *error = diag::notBytecode(bytecode);
    return false;
  }

  std::vector<std::uint8_t> image;
  if (!readWholeFile(launcher, &image)) {
    *error = diag::fileNotFound(launcher);
    return false;
  }

  image.insert(image.end(), payload.begin(), payload.end());
  image.insert(image.end(), kBundleMagic,
               kBundleMagic + sizeof(kBundleMagic));
  writeU64(&image, static_cast<std::uint64_t>(payload.size()));

  // 临时文件 + 改名。
  const std::string temp = output + ".tmp";
  {
    std::FILE* handle = std::fopen(temp.c_str(), "wb");
    if (handle == nullptr) {
      *error = "cannot open " + temp + " for writing";
      return false;
    }
    const std::size_t written =
        std::fwrite(image.data(), 1, image.size(), handle);
    std::fclose(handle);
    if (written != image.size()) {
      *error = "short write to " + temp;
      return false;
    }
  }
  // MSVC 的 rename 不覆盖已存在的目标，先删掉再改名（与 os.replace 等价）。
  std::remove(output.c_str());
  if (std::rename(temp.c_str(), output.c_str()) != 0) {
    *error = "cannot rename " + temp + " to " + output;
    return false;
  }

  *size = static_cast<std::uint64_t>(image.size());
  return true;
}

bool extractBytecode(const std::string& path, const std::string& output,
                     std::uint64_t* size, std::string* error) {
  std::uint64_t length = 0;
  if (!readTrailer(path, &length)) {
    *error = diag::noEmbeddedScript(path);
    return false;
  }

  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) {
    *error = diag::fileNotFound(path);
    return false;
  }
  std::fseek(handle, 0, SEEK_END);
  const long total = std::ftell(handle);
  const long offset =
      total - static_cast<long>(kTrailerSize) - static_cast<long>(length);
  std::fseek(handle, offset, SEEK_SET);
  std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
  const std::size_t got =
      std::fread(payload.data(), 1, payload.size(), handle);
  std::fclose(handle);
  if (got != payload.size()) {
    *error = "short read from " + path;
    return false;
  }

  std::FILE* out = std::fopen(output.c_str(), "wb");
  if (out == nullptr) {
    *error = "cannot open " + output + " for writing";
    return false;
  }
  const std::size_t written = std::fwrite(payload.data(), 1, payload.size(), out);
  std::fclose(out);
  if (written != payload.size()) {
    *error = "short write to " + output;
    return false;
  }
  *size = static_cast<std::uint64_t>(payload.size());
  return true;
}

}  // namespace pcc
