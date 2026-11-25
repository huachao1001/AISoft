// 把字节码接到 launcher 尾部，产出可直接运行的 exe。
//
// ── 为什么是「追加」而不是资源段 ──
//
// Windows 加载器只按节表映射镜像，尾部多余字节不参与加载。所以打包就是
// 「复制 launcher + 写字节码 + 写 16 字节索引」，零外部工具。资源段要
// rc.exe 并改 PE 节表；C++ 数组字面量要每次重新编译——那两条都要求用户
// 装一套 MSVC，与「只装链接器」的约束冲突。
//
// ── 格式 ──
//
//     [launcher 原始字节]
//     [字节码]
//     [尾部 16 字节]  magic "AIPYAPP1" (8) + u64 小端长度
//
// 索引放尾部：头部是 PE 头与节表的地盘。
#pragma once

#include <cstdint>
#include <string>

namespace pcc {

constexpr char kBundleMagic[8] = {'A', 'I', 'P', 'Y', 'A', 'P', 'P', '1'};
constexpr std::uint64_t kTrailerSize = 16;

// 读尾部索引。没有索引（裸 launcher）或长度越界时返回 false。
// length 是字节码长度。
bool readTrailer(const std::string& path, std::uint64_t* length);

// 把 bytecode 追加到 launcher 后面写到 output。成功返回产物大小。
bool bundleExecutable(const std::string& launcher,
                      const std::string& bytecode,
                      const std::string& output, std::uint64_t* size,
                      std::string* error);

// 从打包产物里取出字节码写到 output。返回取出的字节数。
bool extractBytecode(const std::string& path, const std::string& output,
                     std::uint64_t* size, std::string* error);

}  // namespace pcc
