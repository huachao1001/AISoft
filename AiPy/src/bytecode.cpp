// 字节码加载：把构建期生成的二进制读成 `vm::Program`。
//
// ── 为什么加载器在 AiPy 里 ──
//
// 「怎么读自己的字节码」是语言运行时的一部分，就像 CPython 的 marshal 在
// 解释器里一样。放到宿主（PyUI）里会让「换一个宿主就得重新实现加载器」，
// 而那正是拆 AiPy 出去要避免的事。
//
// ── 格式 ──
//
// 权威定义在 tools/pyuic/bytecode_pack.py 头部：定长、小端、无对齐填充。
// 这里不做任何「容错」——文件是构建期产物，格式不对说明打包与加载器版本
// 不一致，直接报错比猜着读安全。
#include "aipy/bytecode.h"

#include "aipy/pylib.h"

#include <cstring>

namespace aipy {
namespace bytecode {
namespace {

// 逐字段读的游标。越界一律返回 0 并置 failed，调用方只查一次标志即可。
class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool failed() const { return failed_; }
  std::size_t remaining() const { return size_ - offset_; }

  std::uint8_t u8() {
    if (offset_ + 1 > size_) return fail();
    return data_[offset_++];
  }
  std::uint16_t u16() {
    if (offset_ + 2 > size_) return fail();
    const std::uint16_t value =
        static_cast<std::uint16_t>(data_[offset_] | (data_[offset_ + 1] << 8));
    offset_ += 2;
    return value;
  }
  std::uint32_t u32() {
    if (offset_ + 4 > size_) return fail();
    const std::uint32_t value =
        static_cast<std::uint32_t>(data_[offset_]) |
        (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
        (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16) |
        (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
    offset_ += 4;
    return value;
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::int64_t i64() {
    const std::uint64_t low = u32();
    const std::uint64_t high = u32();
    return static_cast<std::int64_t>(low | (high << 32));
  }
  double f64() {
    const std::uint64_t bits = static_cast<std::uint64_t>(u32()) |
                               (static_cast<std::uint64_t>(u32()) << 32);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  std::string text() {
    const std::uint32_t length = u32();
    if (offset_ + length > size_) return fail_text();
    std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return value;
  }
  std::vector<std::uint8_t> bytes(std::size_t count) {
    if (offset_ + count > size_) return fail_bytes();
    std::vector<std::uint8_t> out(data_ + offset_, data_ + offset_ + count);
    offset_ += count;
    return out;
  }

 private:
  std::uint8_t fail() {
    failed_ = true;
    return 0;
  }
  std::string fail_text() {
    failed_ = true;
    return std::string();
  }
  std::vector<std::uint8_t> fail_bytes() {
    failed_ = true;
    return std::vector<std::uint8_t>();
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

}  // namespace

Result load(const std::uint8_t* data, std::size_t size) {
  Result result;
  if (size < 8 || std::memcmp(data, kMagic, 8) != 0) {
    result.failed = true;
    result.error = "不是 AiPy 字节码文件（magic 不匹配）";
    return result;
  }

  Reader reader(data + 8, size - 8);
  const std::uint16_t version = reader.u16();
  if (version != kVersion) {
    result.failed = true;
    result.error = "字节码版本 " + std::to_string(version) + " 与运行时 " +
                   std::to_string(kVersion) + " 不一致，请重新生成";
    return result;
  }

  vm::Program& program = result.program;
  program.globalCount = reader.u16();
  program.localCount = reader.u16();
  const int constCount = reader.u16();
  const int functionCount = reader.u16();
  const int builtinCount = reader.u16();
  const int importCount = reader.u16();
  const int extensionCount = reader.u16();

  // 全局名字在常量之前读（与 bytecode_pack.py 的写出顺序一致）：
  // 热加载按名字搬运状态，名字表必须跟着产物走。
  const int globalNameCount = reader.u16();
  program.globalNames.reserve(static_cast<std::size_t>(globalNameCount));
  for (int i = 0; i < globalNameCount; ++i) {
    program.globalNames.push_back(reader.text());
  }

  program.consts.reserve(static_cast<std::size_t>(constCount));
  for (int i = 0; i < constCount; ++i) {
    const std::uint8_t kind = reader.u8();
    switch (kind) {
      case 0: program.consts.push_back(Value()); break;
      case 1: program.consts.push_back(Value(reader.i64() != 0)); break;
      case 2: program.consts.push_back(Value(static_cast<long long>(reader.i64()))); break;
      case 3: program.consts.push_back(Value(reader.f64())); break;
      case 4: program.consts.push_back(Value(reader.text())); break;
      case 5: program.consts.push_back(Value::bytes(reader.text())); break;
      case 6: program.consts.push_back(Value::ellipsis()); break;
      default:
        result.failed = true;
        result.error = "常量类型 " + std::to_string(kind) + " 未知";
        return result;
    }
  }

  program.functions.reserve(static_cast<std::size_t>(functionCount));
  for (int i = 0; i < functionCount; ++i) {
    vm::Function function;
    function.paramCount = reader.u16();
    function.localCount = reader.u16();
    function.defaultCount = reader.u16();
    // 无默认值时写 0xFFFF：u16 的 -1，避免与合法槽号 0 混淆。
    const std::uint16_t defaultSlot = reader.u16();
    function.defaultSlot = defaultSlot == 0xFFFF ? -1 : defaultSlot;
    const std::uint32_t maskLow = reader.u32();
    const std::uint32_t maskHigh = reader.u32();
    function.defaultMask =
        (static_cast<std::uint64_t>(maskHigh) << 32) | maskLow;
    const std::uint16_t varargSlot = reader.u16();
    function.varargSlot = varargSlot == 0xFFFF ? -1 : varargSlot;
    const std::uint16_t kwargSlot = reader.u16();
    function.kwargSlot = kwargSlot == 0xFFFF ? -1 : kwargSlot;
    const std::uint16_t kwonlyStart = reader.u16();
    function.kwonlyStart = kwonlyStart == 0xFFFF ? -1 : kwonlyStart;
    function.posonlyCount = static_cast<int>(reader.u16());
    const std::uint16_t captureStart = reader.u16();
    function.captureStart = captureStart == 0xFFFF ? -1 : captureStart;
    function.isGenerator = reader.u16() != 0;
    function.isCoroutine = reader.u16() != 0;
    const std::uint16_t nameCount = reader.u16();
    function.paramNames.reserve(nameCount);
    for (int n = 0; n < nameCount; ++n) function.paramNames.push_back(reader.text());
    const std::uint32_t codeLength = reader.u32();
    const std::uint16_t lineCount = reader.u16();
    const std::uint16_t nameLength = reader.u16();

    const std::vector<std::uint8_t> nameBytes = reader.bytes(nameLength);
    function.name.assign(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
    function.code = reader.bytes(codeLength);

    function.lines.reserve(lineCount);
    for (int j = 0; j < lineCount; ++j) {
      vm::LineEntry entry;
      entry.offset = reader.u32();
      entry.line = reader.i32();
      function.lines.push_back(entry);
    }
    program.functions.push_back(std::move(function));
  }

  const std::uint32_t moduleCodeLength = reader.u32();
  const std::uint16_t moduleLineCount = reader.u16();
  program.code = reader.bytes(moduleCodeLength);
  program.lines.reserve(moduleLineCount);
  for (int i = 0; i < moduleLineCount; ++i) {
    vm::LineEntry entry;
    entry.offset = reader.u32();
    entry.line = reader.i32();
    program.lines.push_back(entry);
  }

  // 内建表：按名字查本运行时的函数指针。查不到说明打包用的编译器比这里的
  // 运行时要新（或旧），必须报错——静默跳过会让脚本调用到错误的下标。
  program.builtinNames.reserve(static_cast<std::size_t>(builtinCount));
  program.builtins.reserve(static_cast<std::size_t>(builtinCount));
  for (int i = 0; i < builtinCount; ++i) {
    const std::string name = reader.text();
    const int index = builtinIndexFromName(name);
    if (index < 0) {
      result.failed = true;
      result.error = "字节码引用了运行时没有的内建 " + name;
      return result;
    }
    program.builtinNames.push_back(name);
    program.builtins.push_back(builtinAt(index));
  }

  // import 列表：只是「脚本提到过这些模块」，不驱动任何运行期行为
  // （链接决策在打包期按扩展表做）。读掉它以免偏移错位。
  for (int i = 0; i < importCount; ++i) {
    reader.text();
  }

  // 扩展表：CALL_EXT 的下标指向这里。与 import 不同——import 是「脚本
  // 提到了这个模块」，扩展表是「真的调用了这个函数」。按需链接要看的是
  // 后者：脚本 import 了 text 但一个 text 函数都没用，就不该把 text.pylib
  // 链进产物。
  program.extensionNames.reserve(static_cast<std::size_t>(extensionCount));
  for (int i = 0; i < extensionCount; ++i) {
    program.extensionNames.push_back(reader.text());
  }

  // 按名字解析扩展函数指针。
  //
  // 这一步不报错：脚本 import 了 `text`，但这个产物可能没把 text.pylib
  // 链进来（按需的代价）。此时把 nullptr 填进去，等到真的调用那一行再报
  // 「扩展函数 text.upper 没有链接进来」——那比启动就失败更有用：可能整个
  // 应用只有某个菜单用到它。
  //
  // 名字里没有点号的是**裸名**（`open`）：它由某个库用 AIPY_PYLIB_BUILTIN
  // 注册成「像内建一样可调用」。带点的走模块查找，不带点的走裸名表——
  // 名字形态本身就区分了两者，不需要额外的标志位。
  program.extensions.reserve(program.extensionNames.size());
  for (const std::string& name : program.extensionNames) {
    const bool bare = name.find('.') == std::string::npos;
    program.extensions.push_back(bare ? pylib::resolveBare(name.c_str())
                                      : pylib::resolve(name.c_str()));
  }

  if (reader.failed()) {
    result.failed = true;
    result.error = "字节码文件不完整（读到一半就结束了）";
    return result;
  }
  return result;
}

bool findAttached(const std::uint8_t* file, std::size_t size, Attached* out) {
  if (out == nullptr) return false;
  out->data = nullptr;
  out->size = 0;
  if (size < kAttachedTrailerSize) return false;

  // 索引在最后 16 字节，往前读而不是全文件扫 magic：脚本内容里可能
  // 恰好出现 "AIPYAPP1"（比如示例演示这个格式），从头扫会命中假位置。
  const std::uint8_t* trailer = file + size - kAttachedTrailerSize;
  if (std::memcmp(trailer, kAttachedMagic, 8) != 0) return false;

  std::uint64_t length = 0;
  for (int i = 0; i < 8; ++i) {
    length |= static_cast<std::uint64_t>(trailer[8 + i]) << (i * 8);
  }
  // 长度越界说明文件被截断或被改过。不猜、不截断，直接当没有内嵌脚本。
  if (length == 0 || length > size - kAttachedTrailerSize) return false;

  out->data = file + (size - kAttachedTrailerSize - length);
  out->size = static_cast<std::size_t>(length);
  return true;
}

Result loadAttached(const std::uint8_t* file, std::size_t size) {
  Attached attached;
  if (!findAttached(file, size, &attached)) {
    Result result;
    result.failed = true;
    result.error = "这个 exe 里没有内嵌脚本（可能是裸 launcher）";
    return result;
  }
  return load(attached.data, attached.size);
}

}  // namespace bytecode
}  // namespace aipy
