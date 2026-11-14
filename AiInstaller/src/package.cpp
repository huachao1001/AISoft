// 安装包读取：定位 exe 末尾的 trailer，解析清单，按偏移导出载荷
//
// 全部经 AiUI 的流接口完成，不出现任何系统 API。
#include "AiInstaller/package.h"

#include "internal_util.h"

#include "AiUI/platform.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace AiInstaller {
namespace {

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

// 分块复制的缓冲大小：载荷可能是几十 MB，不能一次读进内存
constexpr int64_t kChunkSize = 1 << 20;

bool copyRange(AiUI::FileStream& source, uint64_t offset, uint64_t size,
               AiUI::FileStream& target) {
  if (!source.seek(static_cast<int64_t>(offset), AiUI::SeekOrigin::kBegin, nullptr)) return false;

  std::vector<uint8_t> buffer(static_cast<size_t>(kChunkSize));
  uint64_t remaining = size;
  while (remaining > 0) {
    const int64_t want = static_cast<int64_t>(remaining < static_cast<uint64_t>(kChunkSize)
                                                  ? remaining
                                                  : static_cast<uint64_t>(kChunkSize));
    const int64_t got = source.read(buffer.data(), want);
    if (got <= 0) return false;
    if (target.write(buffer.data(), got) != got) return false;
    remaining -= static_cast<uint64_t>(got);
  }
  return true;
}

}  // namespace

bool Package::openSelf(std::wstring& error) {
  error.clear();
  packaged_ = false;
  selfPath_ = AiUI::platform()->executablePath();
  if (selfPath_.empty()) {
    error = L"取不到安装器自身路径";
    return false;
  }

  const uint64_t total = AiUI::platform()->fileSize(selfPath_);
  if (total <= kTrailerSize) return false;  // 裸安装器，没有包装过

  auto stream = AiUI::platform()->openFileStream(selfPath_, false);
  if (!stream) {
    error = L"无法读取安装器自身";
    return false;
  }

  uint8_t trailer[kTrailerSize] = {};
  if (!stream->seek(static_cast<int64_t>(total - kTrailerSize), AiUI::SeekOrigin::kBegin, nullptr) ||
      stream->read(trailer, static_cast<int64_t>(kTrailerSize)) !=
          static_cast<int64_t>(kTrailerSize)) {
    // 读不到尾部不算损坏：可能就是个还没打包的安装器
    return false;
  }

  if (std::memcmp(trailer, kPackageMagic, sizeof(kPackageMagic)) != 0) return false;

  const uint32_t format = readU32(trailer + 8);
  if (format != kPackageFormatVersion) {
    error = L"安装包格式版本不支持：" + std::to_wstring(format);
    return false;
  }

  const uint32_t manifestSize = readU32(trailer + 12);
  const uint64_t dataOffset = readU64(trailer + 16);
  const uint64_t dataSize = readU64(trailer + 24);

  // 边界校验：损坏的偏移会导致越界读，宁可报错也不猜
  const uint64_t trailerStart = total - kTrailerSize;
  if (dataOffset > trailerStart || dataSize > trailerStart - dataOffset ||
      manifestSize > dataSize) {
    error = L"安装包数据区损坏";
    return false;
  }

  std::string manifestBytes(manifestSize, '\0');
  if (!manifestBytes.empty()) {
    if (!stream->seek(static_cast<int64_t>(dataOffset), AiUI::SeekOrigin::kBegin, nullptr) ||
        stream->read(manifestBytes.data(), manifestSize) != static_cast<int64_t>(manifestSize)) {
      error = L"无法读取安装包清单";
      return false;
    }
  }

  const std::wstring manifestText = AiUI::platform()->fromUtf8(manifestBytes);
  Manifest manifest;
  std::wstring parseError;
  if (!parseManifest(manifestText, manifest, parseError)) {
    error = L"安装包清单无效：" + parseError;
    return false;
  }

  // 清单里记的偏移相对载荷区起点，这里换算成绝对偏移后统一校验一次，
  // 免得坏包在解压到一半才失败
  const uint64_t base = dataOffset + manifestSize;
  const uint64_t payloadSize = dataSize - manifestSize;
  // 卸载副本是不带载荷的合法安装包：它只要清单，文件条目的偏移仍指向原来
  // 那份载荷，在这里按「必须落在包里」校验必然失败。没有载荷就跳过这层校验，
  // 卸载只用 file.path，用不到偏移。
  if (payloadSize > 0) {
    for (const auto& file : manifest.files) {
      if (file.offset > payloadSize || file.size > payloadSize - file.offset) {
        error = L"安装包内文件越界：" + file.path;
        return false;
      }
    }
  }

  manifest_ = std::move(manifest);
  manifestText_ = manifestText;
  dataOffset_ = dataOffset;
  payloadBase_ = base;
  payloadSize_ = payloadSize;
  packaged_ = true;
  return true;
}

std::wstring Package::extractFile(const ManifestFile& file, const std::wstring& targetPath) const {
  if (!packaged_) return L"安装包未打开";

  // 先建父目录：清单允许 "config\x.dll" 这类带子目录的路径
  const std::wstring parent = detail::directoryOf(targetPath);
  if (!parent.empty() && !detail::ensureDirectoryTree(parent)) {
    return L"无法创建目录：" + parent;
  }

  auto source = AiUI::platform()->openFileStream(selfPath_, false);
  if (!source) return L"无法读取安装包";

  auto target = AiUI::platform()->openFileStream(targetPath, true);
  if (!target) return L"无法写入文件：" + targetPath;

  if (!copyRange(*source, payloadBase_ + file.offset, file.size, *target)) {
    return L"解压失败：" + file.path;
  }
  return {};
}

bool Package::readBlob(const std::wstring& path, std::vector<uint8_t>& out) const {
  out.clear();
  if (!packaged_) return false;

  for (const auto& file : manifest_.files) {
    if (AiUI::platform()->compareText(file.path, path) != 0) continue;
    if (file.size > (16u << 20)) return false;  // 图标这类小文件才走内存

    auto source = AiUI::platform()->openFileStream(selfPath_, false);
    if (!source) return false;
    if (!source->seek(static_cast<int64_t>(payloadBase_ + file.offset),
                      AiUI::SeekOrigin::kBegin, nullptr)) {
      return false;
    }

    out.resize(static_cast<size_t>(file.size));
    if (out.empty()) return true;
    return source->read(out.data(), static_cast<int64_t>(out.size())) ==
           static_cast<int64_t>(out.size());
  }
  return false;
}

std::wstring Package::copyInstallerTo(const std::wstring& targetPath) const {
  const std::wstring parent = detail::directoryOf(targetPath);
  if (!parent.empty() && !detail::ensureDirectoryTree(parent)) {
    return L"无法创建目录：" + parent;
  }

  auto source = AiUI::platform()->openFileStream(selfPath_, false);
  if (!source) return L"无法读取安装器自身";

  auto target = AiUI::platform()->openFileStream(targetPath, true);
  if (!target) return L"无法写入文件：" + targetPath;

  // 只复制到清单起点为止：后面是载荷，安装目录里不需要第二份
  const uint64_t keep = packaged_ ? dataOffset_ : AiUI::platform()->fileSize(selfPath_);
  if (!copyRange(*source, 0, keep, *target)) return L"复制安装器失败：" + targetPath;
  return {};
}

std::wstring Package::embedManifest(const std::wstring& exePath,
                                    const std::wstring& manifestText) const {
  if (exePath.empty()) return L"卸载程序路径为空";

  // 统一按 UTF-8 存：清单里的中文标题不能依赖系统代码页
  const std::string manifest = AiUI::platform()->toUtf8(manifestText);

  // 必须用「读写打开、不清空」：openFileStream(path, true) 是新建/覆盖，
  // 一打开就把安装器副本清空，后面既读不到正文长度也写不进清单。
  auto target = AiUI::platform()->openFileStreamForUpdate(exePath);
  if (!target) return L"无法打开卸载程序：" + exePath;

  // 先截到「安装器本体」的长度：调用方传进来的可能是刚复制出来的纯净副本，
  // 但重装时可能是个已经带过 trailer 的文件，不截断就会越叠越长。
  const uint64_t bodySize = AiUI::platform()->fileSize(exePath);
  if (bodySize <= kTrailerSize) return L"卸载程序过小，不是有效的安装器副本";

  // 定位本体的真实结尾：若已有 trailer 就退回它的 dataOffset，避免把旧清单
  // 与旧 trailer 留在新清单前面。覆盖安装时传进来的可能就是这样的文件。
  uint64_t bodyEnd = bodySize;
  {
    uint8_t trailer[kTrailerSize] = {};
    if (target->seek(static_cast<int64_t>(bodySize - kTrailerSize), AiUI::SeekOrigin::kBegin,
                     nullptr) &&
        target->read(trailer, static_cast<int64_t>(kTrailerSize)) ==
            static_cast<int64_t>(kTrailerSize) &&
        std::memcmp(trailer, kPackageMagic, sizeof(kPackageMagic)) == 0) {
      const uint64_t previous = readU64(trailer + 16);
      if (previous > 0 && previous < bodySize) bodyEnd = previous;
    }
  }

  // 先截断再写：不截断的话旧 trailer 会落在新清单与载荷区之间，
  // 后面的写入位置对不上，读出来就是错的数据。
  if (!target->setSize(bodyEnd)) return L"无法截断卸载程序";
  if (!target->seek(static_cast<int64_t>(bodyEnd), AiUI::SeekOrigin::kBegin, nullptr)) {
    return L"无法定位卸载程序末尾";
  }
  if (target->write(manifest.data(), static_cast<int64_t>(manifest.size())) !=
      static_cast<int64_t>(manifest.size())) {
    return L"无法写入卸载程序清单";
  }

  // trailer 与打包器完全同构：dataOffset 指向清单起点，dataSize 只含清单
  // （载荷为 0），这样卸载副本和正常安装包走同一套 openSelf 读取路径。
  uint8_t trailer[kTrailerSize] = {};
  std::memcpy(trailer, kPackageMagic, sizeof(kPackageMagic));
  auto putU32 = [&trailer](size_t offset, uint32_t value) {
    trailer[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    trailer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    trailer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    trailer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
  };
  auto putU64 = [&trailer](size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      trailer[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
  };
  putU32(8, kPackageFormatVersion);
  putU32(12, static_cast<uint32_t>(manifest.size()));
  putU64(16, bodyEnd);
  // dataSize 只算「清单 + 载荷」，与打包器一致：这里的副本不带载荷，
  // 所以就等于清单长度。多加 trailer 会让 payloadSize_ 算出一个假的 32 字节，
  // 卸载时读 package 就会以为还有载荷。
  putU64(24, static_cast<uint64_t>(manifest.size()));

  if (target->write(trailer, static_cast<int64_t>(kTrailerSize)) !=
      static_cast<int64_t>(kTrailerSize)) {
    return L"无法写入卸载程序尾部";
  }
  return {};
}

bool loadManifestFile(const std::wstring& path, Manifest& out, std::wstring& error) {
  error.clear();
  if (!AiUI::platform()->pathExists(path)) {
    error = L"找不到安装清单：" + path;
    return false;
  }

  // 清单不大，几百 KB 上限足够；读不到就按损坏处理
  const std::string bytes = AiUI::platform()->readFile(path, 1u << 20);
  if (bytes.empty()) {
    error = L"安装清单为空：" + path;
    return false;
  }

  std::wstring parseError;
  if (!parseManifest(AiUI::platform()->fromUtf8(bytes), out, parseError)) {
    error = L"安装清单无效：" + parseError;
    return false;
  }
  return true;
}

}  // namespace AiInstaller
