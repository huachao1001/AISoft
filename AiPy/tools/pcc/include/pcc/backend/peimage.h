// 读产物 exe 里「实际被链接进来的扩展模块与函数」。
//
// ── 为什么读产物而不是读打包器的摘要 ──
//
// `pcc --exe` 会打印 `linked functions: ...`，但那是打包器**自己说**
// 它点了哪些名。两者可能不一致：曾经为了验证测试本身而故意注入「全点名」，
// 摘要照旧只列脚本用到的函数，测试仍然报通过——测试信了被告，没有信证据。
//
// 产物的 `.aipyfn` 节是链接器合并出来的结果，VM 运行时看到的就是这份数据。
// 它只包含真的被链接进来的注册项，所以「只调 pending 却把 start 也链进来」
// 在这条路径上一定会被发现。
//
// ── 节内容 ──
//
//   .aipyreg  aipy::pylib::Module    = { const char* name }
//   .aipyfn   aipy::pylib::Function  = { const char* module; const char* name;
//                                        void* fn }
//
// 字符串在 `.rdata`，所以要把「虚拟地址 → 文件偏移」映射一次。
#pragma once

#include <string>
#include <vector>

namespace pcc {

// 产物里一个被链接进来的扩展函数。
struct LinkedFunction {
  std::string module;
  std::string name;
};

struct LinkedExtensions {
  std::vector<std::string> modules;
  std::vector<LinkedFunction> functions;
};

// 读 exe 的 `.aipyreg` / `.aipyfn` 节。
//
// 失败返回 false 并在 error 里说明原因。没有这两个节时返回 true 但列表为空
// ——「没用任何扩展」是合法状态，不是错误。
bool readLinkedExtensions(const std::string& path, LinkedExtensions* out,
                          std::string* error);

}  // namespace pcc
