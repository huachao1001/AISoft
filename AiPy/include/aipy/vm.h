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
// ── 为什么没有 C++ 异常 ──
//
// C++ 的 `throw` 会给每个调用点加 unwind 表，而执行循环存在的全部理由就是
// 「一次 switch + 几次直接下标」（铁律六）。所以脚本层的异常走一条显式的
// 处理块栈：编译器在字节码里插 SETUP_EXC / POP_EXC，VM 在抛出时按它展开。
// 语义与 CPython 的 `try` / `except` / `else` / `finally` / `raise` 对齐。
//
// 值层的失败（除零、键不存在、类型不符）通过线程局部的「待处理异常」旁路
// 通知执行循环，见 `value.h`。VM 自身的错误（编译器 bug）仍用 `Result::failed`：
// 那两种错误对脚本作者意味着完全不同的东西，不能混成一个通道。
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
  kCall,
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
  // 切片赋值：u8 位标记（同 kSlice）。栈上是 [容器, start?, stop?, step?, 值]。
  kSetSlice,
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

  // ── 异常 ──
  //
  // 不用 C++ 的 throw：那会给每条指令的调用点加 unwind 表，而这里的全部
  // 理由就是「一次 switch + 几次直接下标」（铁律六）。改用显式的处理块栈，
  // 与 with 的机制同构：编译器在字节码里插 SETUP_EXC/POP_EXC，VM 在异常
  // 发生时按处理块跳转。
  kSetupExc,   // u16 handler 目标；压处理块，记下当前 sp / 异常深度
  kPopExc,     // 弹掉最内层处理块（离开保护区，或跳出 try）
  kRaise,      // 弹栈顶值当异常抛出
  kRaiseType,  // u16 类型名常量号, u8 有没有消息（有则弹栈顶当消息）
  kReraise,    // 重抛当前处理的异常（bare `raise`）
  // u16 不匹配时的目标, u16 类型名常量号。看 frame.exceptions.back() 的
  // 类型是否匹配；匹配则继续，否则跳到目标。
  kExcMatch,
  kCurrentExc,  // 把当前处理的异常压栈（`except ... as e` 用）
  kClearExc,    // 离开 except 体：弹掉一层「当前异常」
  // `del x`：把槽置成「已删除」哨兵。再读这个槽会抛 NameError，
  // 与 CPython 一致——赋 None 是不够的，None 是合法值。
  kDelLocal,   // u16 槽号
  kDelGlobal,  // u16 槽号
  kBuildTuple, // u16 元素个数：从栈上取 N 个元素建成元组
  kBuildSet,   // u16 元素个数：从栈上取 N 个元素建成集合（去重）

  // ── 动态调用（新指令一律追加在末尾，不许插进中间）──
  //
  // 插进中间会让 Op 的数值整体偏移，而字节码里的操作码就是枚举数值，
  // 已生成的 .bc 与 .pylib 里的字节码会静默执行成别的指令。旧指令的位置
  // 因此是冻结的，新指令只能往后加。
  //
  // 形参表里有 `*args` / `**kwargs` / 仅关键字参数时，实参无法在编译期
  // 拍平成位置顺序（个数与名字都要到运行期才知道），所以走这条：
  // 栈上是 [位置实参列表, 关键字字典]，由 VM 按 CPython 规则绑定。
  kCallDyn,  // u16 函数号

  // `f(**d)`：栈上是 [目标字典, 来源字典]，把来源的键值并进目标。
  // 键重复抛 TypeError，与 CPython 一致。
  kDictMerge,

  // 取函数值：u16 函数号, u16 捕获个数。压上一个 kObject 函数值。
  // `g = f` / `f = lambda ...` 都走它——函数从此是能存进变量的值。
  kMakeFunction,

  // 调用栈顶的函数值：栈上是 [可调用值, 位置实参...]，u8 是实参个数。
  // `g(1)` 里的 g 到运行期才知道指向哪个函数，所以不能编成 CALL + 函数号。
  kCallValue,
};

enum class BinOp : std::uint8_t {
  kAdd, kSub, kMul, kDiv, kFloorDiv, kMod, kPow,
  kBitAnd, kBitOr, kBitXor, kShl, kShr,
};

enum class CmpOp : std::uint8_t {
  kEq, kNe, kLt, kLe, kGt, kGe, kIn, kNotIn, kIs, kIsNot,
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
  // 有默认值的参数个数，以及存放默认值的隐藏全局槽。
  // 默认值在**定义处**求值（CPython 语义），所以不在函数表里存值。
  int defaultCount = 0;
  int defaultSlot = -1;
  // 每个具名参数一位：置位表示它有默认值。`def f(*, a=1, b)` 的默认值
  // 不连续，光靠「尾部个数」表达不出来。
  std::uint64_t defaultMask = 0;
  // `*args` / `**kwargs` 的局部槽（-1 表示没写）。它们排在普通参数之后。
  int varargSlot = -1;
  int kwargSlot = -1;
  // 从哪个参数下标开始是仅关键字参数（等于 paramCount 表示没有）。
  int kwonlyStart = -1;
  // 仅关键字参数里带默认值的个数（它们排在整个默认值列表的末尾）。
  int kwonlyDefaultCount = 0;
  // 参数名，按槽号排列（含仅关键字参数，不含 *args / **kwargs）。
  // CALL_DYN 要按名字把关键字实参对上形参。
  std::vector<std::string> paramNames;
  // 闭包捕获值的起始局部槽。捕获槽紧跟在参数/vararg/kwarg 之后，按
  // MAKE_FUNCTION 压栈的顺序排列；-1 表示这个函数不捕获。
  int captureStart = -1;
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
//
// 未捕获的异常与「VM 自身坏了」走同一个结构：两者都要让宿主停下来并报错，
// 区别只在对脚本作者的含义。`exception` 非 None 表示这是脚本层的异常
// （`ZeroDivisionError` 之类），`error` 里是它的消息。
struct Result {
  Value value;
  bool failed = false;
  std::string error;
  int line = 0;
  // 未捕获的异常值；不是异常时是 None。用 Value 而不是再存一份类型名 +
  // 消息：异常本来就是 Value，多存一份字段等于给同一样东西开两个表示。
  Value exception;
};

// 未捕获异常的类型名；`exception` 不是异常时返回空串。
inline std::string resultExceptionType(const Result& result) {
  return exceptionType(result.exception);
}

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
