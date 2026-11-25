// AiPy 虚拟机：字节码的执行器。
//
// ── 为什么是字节码，而不是「编译成 C++」 ──
//
// 编译成 C++ 快，但脚本一改就要重编译整个 exe。界面脚本改一行要等一次
// 完整链接，这个反馈周期对写界面的人是致命的。字节码把「改脚本」压成
// 「重新生成一段字节码并加载」，几十毫秒。
//
// 代价是慢（实测 300 万次累加：直调 C++ 1.11 ms，VM 477 ms）。这个代价
// 对界面脚本可以接受——界面脚本的瓶颈在重绘与 GDI，不在算术。
//
// ── 为什么是显式帧栈，不是递归 ──
//
// `spike/vm` 那份原型用 `run()` 递归实现函数调用来量速度。递归的深度上限
// 是 C 栈（默认 1 MB，约几千层），而脚本里一个递归下降解析器就能吃掉它。
// 这里用显式 `std::vector<Frame>`：深度上限只看内存，溢出时报错而不是崩。
//
// ── 为什么没有异常 ──
//
// 异常会给每个调用点加 unwind 表，而且脚本子集本来就没有 try/except
// （见 docs/SUPPORT.md 的 C 类）。失败用 `Result::failed` 表示：VM 停下来、
// 带着出错位置返回，宿主决定是打印还是弹窗。
#pragma once

#include "aipy/value.h"
#include "aipy/builtins.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aipy {
namespace vm {

// ── 操作码 ──
//
// 定长操作数、小端序。不用变长整数：那会让每个操作数的解析多一次分支，
// 而这层要的正是「一条指令 = 一次 switch + 几次直接下标」。
//
// 顺序有意义：常用的落在前段（对分支预测略友好）。
enum class Op : std::uint8_t {
  kConst = 0,       // u16 常量下标
  kNone,
  kTrue,
  kFalse,
  kLoadLocal,       // u16 槽
  kStoreLocal,      // u16 槽
  kLoadGlobal,      // u16 槽
  kStoreGlobal,     // u16 槽
  kBin,             // u8 BinOp
  kCmp,             // u8 CmpOp
  kUnary,           // u8 UnOp
  kJump,            // u16 目标
  kJumpIfFalse,     // u16 目标，弹栈
  kJumpIfFalseKeep, // u16 目标，保留栈顶（and）
  kJumpIfTrueKeep,  // u16 目标，保留栈顶（or）
  kPop,
  kDup,
  kCall,            // u16 函数号, u8 实参个数
  kCallBuiltin,     // u16 内建号, u8 实参个数
  kCallMethod,      // u16 方法名常量号, u8 实参个数
  kCallExt,         // u16 扩展号, u8 实参个数
  kReturn,
  kBuildList,       // u16 元素个数
  kBuildDict,       // u16 键值对个数
  kGetItem,
  kSetItem,
  // 切片：u8 位标记，低三位表示 start/stop/step 是否压栈。
  kSlice,
  // 迭代：栈上是 [list, index] 两槽。
  kIterInit,        // 弹容器 → 压「物化后的列表」「0」
  kIterNext,        // u16 退出目标；还有元素就压一个并前进
  kIterBreak,       // 弹掉 [list, index]（break 用）
  // 下标原地运算：x[i] += v 之类。栈上是 [容器, 下标, 右值]，u8 是 BinOp。
  // 专门做成一条指令而不是「读-改-写」三条：后者要把容器与下标各复制一份，
  // 得借临时槽，而且一旦下标表达式带副作用就会被求值两次。
  kInplace,
  // with 语句。u16 是借用的局部槽号（存管理器）。
  //
  // 为什么要有槽而不是留在栈上：`with` 体里可能压任意深度的临时值，
  // 退出时栈顶不一定是管理器。放进局部槽后，`__exit__` 回头找它永远是
  // O(1)，也不用关心体内栈怎么变。
  kWithEnter,  // 弹管理器 → 存槽 → 调 __enter__() → 压返回值
  kWithExit,   // 从槽取管理器 → 调 __exit__() → 弹掉返回值
};

enum class BinOp : std::uint8_t {
  kAdd, kSub, kMul, kDiv, kFloorDiv, kMod, kPow,
  kBitAnd, kBitOr, kBitXor, kShl, kShr,
};

enum class CmpOp : std::uint8_t {
  kEq, kNe, kLt, kLe, kGt, kGe, kIn, kNotIn,
};

enum class UnOp : std::uint8_t { kNeg, kNot, kInvert };

// 扩展函数签名：裸函数指针 + 指针/个数。
// 用这个形状而不是 std::function：DLL 与静态库能共用同一签名，
// 调用边界上也不分配容器。
using ExtensionFn = Value (*)(const Value* args, int count);

// 内建函数的签名在 builtins.h 里：它多一个 `std::string* error`。
// 为什么内建要能报错而扩展不用：扩展是外部能力，失败用返回值表示（比如
// fs.read_file 失败返回 None）；内建是语言语义，`len()` 参数给错必须指出
// 是哪一行——静默返回 None 会让错误在几十行后才爆。

// 一行源位置。VM 报错时要能指回脚本的哪一行，否则「NoneType 没有 upper」
// 这种错误在界面脚本里根本查不出来。
struct LineEntry {
  std::uint32_t offset;  // 字节码偏移
  std::int32_t line;     // 源文件行号（1 起）
};

// 一个函数（含脚本里的 def）。
struct Function {
  std::string name;   // 仅用于报错
  std::vector<std::uint8_t> code;
  std::vector<LineEntry> lines;
  int paramCount = 0;
  int localCount = 0;  // 局部槽数（含参数）
};

// 编译好的程序：模块级代码 + 函数表 + 常量池 + 两张调用表。
struct Program {
  std::vector<std::uint8_t> code;       // 模块级代码
  std::vector<LineEntry> lines;         // 模块级行表
  std::vector<Function> functions;
  std::vector<Value> consts;

  std::vector<std::string> builtinNames;
  std::vector<BuiltinFn> builtins;
  std::vector<std::string> extensionNames;
  std::vector<ExtensionFn> extensions;

  // 全局变量名，下标即槽号。热加载按名字搬运状态：新程序里加一个变量
  // 会让后面的槽整体移位，按下标搬运会把 count 的值灌进 label。
  std::vector<std::string> globalNames;

  int globalCount = 0;
  int localCount = 0;  // 模块级代码的局部槽（用于解包之类的临时量）
};

// 把旧程序的状态搬到新程序上（热加载）。
//
// 按名字匹配，只在两边都有这个变量时搬运。改一行脚本重新生成字节码，
// 但用户已经填进表单、点了几次按钮的状态不能丢。
std::vector<Value> carryGlobals(const Program& from, const std::vector<Value>& values,
                                const Program& to);

// 执行结果。`line` 是出错位置，0 表示不是脚本里的位置（比如加载失败）。
struct Result {
  Value value;
  bool failed = false;
  std::string error;
  int line = 0;
};

// 一帧的内部结构在 `src/vm/internal.h`：它是执行循环的私有数据，
// 不属于宿主该看见的接口。

class Machine {
 public:
  Machine() = default;

  // 执行模块级代码。可以反复调用：全局槽在两次调用之间保留，
  // 热加载时要靠它把状态带过去（见 `setGlobals`）。
  Result run(const Program& program);

  // 按名字调用一个脚本函数，用一份**独立的**帧栈。
  //
  // 给 pylib 用：脚本函数不是值（`Value` 没有函数类型），扩展拿不到函数
  // 指针，只能拿名字。独立的帧栈意味着它不碰调用方的栈，可以从别的线程
  // 调用——线程库正是这么用的。
  //
  // **全局槽是进程级共享的**（见 `globals_`）：子线程里 `global X` 写的是
  // 主线程看得见的那个槽，与 Python 的模块全局变量语义一致。让每个线程
  // 各持一份副本会让「子线程更新计数器」静默失效——那是最难查的一类 bug：
  // 程序不报错，结果就是错的。
  Result callFunction(const Program& program, const std::string& name,
                      const std::vector<Value>& args);

  // 全局槽。热加载用：新程序跑之前把旧值灌回去，计数器和表单状态不会丢。
  //
  // **进程级共享**（静态）：线程里 `global X` 写的必须是主线程看得见的槽，
  // 否则「子线程更新计数器」会静默失效。代价是并发写同一个槽需要脚本自己
  // 用 lock 保护——这与 Python 的语义一致，全局变量从来不是线程安全的。
  static const std::vector<Value>& globals() { return globals_; }
  static void setGlobals(std::vector<Value> values) { globals_ = std::move(values); }

  // 触发 `break` 时库内部用；脚本不能直接调。
  static constexpr int kMaxCallDepth = 256;

 private:
  static std::vector<Value> globals_;
};

// 当前线程正在执行的程序。扩展函数在 VM 调用栈内可以用它拿到 Program，
// 从而把脚本函数搬到别处（比如子线程）执行。
//
// 按线程存：两个线程各自跑不同的程序时互不影响，也不会读到对方正在拆的
// Program。返回 nullptr 表示「不在 VM 调用中」。
const Program* currentProgram();

// 反汇编：给测试与排错用。每行形如 `12: load_global 3`。
std::vector<std::string> disassemble(const Program& program);

// 操作码名字，反汇编与报错信息共用。
const char* opName(Op op);

}  // namespace vm
}  // namespace aipy
