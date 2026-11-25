// 读 COFF 静态库里的符号：库索引 + 各成员的 COFF 符号表。
//
// ── 为什么不能只读库索引 ──
//
// ar 归档的索引成员（名字 `/`）只收「被别处引用到的」符号。pylib 的注册项
// 天生没人引用（它们靠节合并被发现），所以索引里根本没有
// `aipy_text_fn_upper`——实测：库里出现 3 次，索引里 0 次。
//
// 只读索引的后果是「没有函数级符号 → 退回整库链接」。整库链接不报错，
// 只是产物变大，属于最该避免的静默退化。
//
// ── 所以两层都要读 ──
//
//   索引成员   快，覆盖被引用的符号
//   成员符号表 全，覆盖注册项这种「没人引用但要进产物」的符号
//
// 两层顺序：先索引（便宜），再逐成员解析 COFF 符号表（贵但完整）。
#pragma once

#include <string>
#include <vector>

namespace pcc {

// 静态库里的一个符号，以及提供它的成员（obj）名。
struct LibrarySymbol {
  std::string name;
  std::string member;  // 形如 "fn_upper.cpp.obj"；解析不到时为空
};

// 读库里的全部外部符号（定义，不含未定义引用）。
//
// 失败返回 false，原因在 error。不是 COFF 静态库时返回 false，而不是
// 猜测——调用方据此退回整库链接，那是安全的一侧。
bool readLibrarySymbols(const std::string& path,
                        std::vector<LibrarySymbol>* symbols,
                        std::string* error);

}  // namespace pcc
