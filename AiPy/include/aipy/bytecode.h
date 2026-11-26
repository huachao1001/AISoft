// 字节码文件的读取：把构建期产物变成 `vm::Program`。
//
// 「怎么读自己的字节码」属于语言运行时，不属于宿主——就像 CPython 的
// marshal 在解释器里。放到宿主里会让「换个宿主就得重写加载器」，正是
// 拆 AiPy 出去要避免的事。
//
// 格式权威定义在 tools/pyuic/bytecode_pack.py 头部。这里没有对齐填充，
// 全部小端定长，读失败一律报错而不是猜——文件是构建期产物，读不通说明
// 打包与加载器版本不一致。
#pragma once

#include "aipy/vm.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aipy {
namespace bytecode {

inline constexpr char kMagic[8] = {'A', 'I', 'P', 'Y', 'B', 'C', '0', '1'};
inline constexpr std::uint16_t kVersion = 6;

struct Result {
  vm::Program program;
  bool failed = false;
  std::string error;
};

// 从内存读。不持有 data：返回的 Program 里已有全部数据的副本。
Result load(const std::uint8_t* data, std::size_t size);

inline Result load(const std::vector<std::uint8_t>& data) {
  return load(data.data(), data.size());
}

// ── 追加在可执行文件尾部的字节码 ──
//
// 打包成 exe 的做法：把字节码直接追加到 launcher 的 PE 镜像后面，再写
// 16 字节尾部索引。Windows 加载器只认节表，尾部多余字节不影响启动，
// 所以不需要资源段、不需要内置解压、也不需要任何 C++ 编译器参与打包。
//
// 判据放在尾部而不是头部：头部是 PE 镜像的地盘，改它就得重写节表。
//
// 尾部 16 字节：magic "AIPYAPP1" + payload 长度 u64（小端）。
inline constexpr char kAttachedMagic[8] = {'A', 'I', 'P', 'Y', 'A', 'P', 'P', '1'};
inline constexpr std::size_t kAttachedTrailerSize = 16;

struct Attached {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

// 在整份文件里找追加的字节码。找不到返回 false——调用方据此区分
// 「这个 exe 是打包产物」和「这个是裸 launcher」。
bool findAttached(const std::uint8_t* file, std::size_t size, Attached* out);

// 先找再读。没有内嵌脚本时报错而不是回落到空程序：静默跑一个空程序
// 会让用户以为脚本逻辑被优化掉了。
Result loadAttached(const std::uint8_t* file, std::size_t size);

}  // namespace bytecode
}  // namespace aipy
