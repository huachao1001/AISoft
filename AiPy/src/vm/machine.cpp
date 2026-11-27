// 执行循环：取指、译码、执行。
//
// ── 为什么这一层是一个大 switch，而不是「每条指令一个函数」 ──
//
// 拆成函数会让每条指令多一次调用与返回，而这一层存在的全部理由就是
// 「一次 switch + 几次直接下标」。实测（见 PyUI/docs/ARCHITECTURE.md）
// 477 ms / 300 万次累加里，分派本身占了可观的份额。
//
// 拆到别处的是**语义**：算子怎么算（operators.cpp）、方法怎么找
// （methods.cpp）、帧长什么样（internal.h）。循环只负责「栈怎么动」。
//
// ── 四条不变量 ──
//
//   1. `frames.back()` 永远是当前帧；模块级帧的 function 为 nullptr。
//   2. 一帧的参数已经放在共享栈的 [stackBase, sp) 里，调用方负责压。
//   3. 帧的 locals 与共享栈是两个独立区域：参数进来时先从栈拷进 locals，
//      之后 locals 只由 load_local/store_local 访问。
//   4. 任何 return 都要先把返回值从当前帧的栈区里取出来，再回到上一帧。
#include "vm/internal.h"

#include "aipy/builtins.h"
#include "aipy/exceptions.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace aipy {
namespace vm {

// 协议方法调用（`__enter__` / `__exit__`）。定义在后面（它要用 execute）。
// `found` 为 false 表示该对象不支持这个协议；`error` 非空表示调用失败。
bool callDunder(const Program& program, std::vector<Value>& globals,
                const Value& target, const std::string& name,
                std::vector<Value> args, Value* out, bool* found,
                std::string* error);

namespace detail {

namespace {

// 当前线程正在跑的程序。thread_local 而不是全局：两个线程各跑不同程序时
// 互不影响，也不会在对方拆 Program 时读到悬空指针。
thread_local const Program* g_currentProgram = nullptr;

// 异常展开：把 exception 交给最近的处理块。
//
// ── 为什么是独立函数且 noinline ──
//
// 它原来是 execute 里的 lambda，被内联进去后让那个巨大函数的栈帧又多出
// 几层局部（若干个 std::string 与 Value）。脚本函数在子线程里跑时栈只有
// 默认的 1 MB，加完就直接触发 /GS 的栈检查失败（0xC0000409），症状是
// 「子线程 print 正常、函数一返回进程就崩」。
//
// 拆出来也符合铁律六：这是**语义**（怎么找处理块），不是「指令怎么执行」。
//
//   try 块   → 值栈退回到进入时的深度、压上异常值、跳到 handler 入口
//   with 块  → 先调 __exit__（CPython 在异常穿出 with 时也调），继续向外找
//   帧没块了 → 弹帧，异常交给调用者
//   没帧了   → 程序带着异常结束，result 里带上类型与消息
//
// 返回 true 表示控制已交给某个 handler；false 表示程序结束。
__declspec(noinline) bool unwindException(std::vector<Frame>& frames,
                                          std::vector<Value>& stack,
                                          std::size_t* sp, Result* result,
                                          const Value& exception,
                                          const Program& program,
                                          std::vector<Value>& globals) {
  for (;;) {
    if (frames.empty()) {
      result->failed = true;
      result->exception = exception;
      result->error = exceptionMessage(exception);
      *sp = 0;
      return false;
    }
    Frame& frame = frames.back();
    detail::ExceptionState* state = frame.exceptions.get();
    if (state == nullptr || state->blocks.empty()) {
      // 这一帧没有保护区：把异常传给调用者。先记下行号，弹出后帧就没了。
      if (frame.lines != nullptr) {
        int line = 0;
        for (const LineEntry& entry : *frame.lines) {
          if (entry.offset > frame.pc) break;
          line = entry.line;
        }
        result->line = line;
      }
      *sp = frame.stackBase;
      frames.pop_back();
      if (frames.empty()) {
        result->failed = true;
        result->exception = exception;
        result->error = exceptionMessage(exception);
        return false;
      }
      continue;
    }

    const Block block = state->blocks.back();
    state->blocks.pop_back();
    // 两个栈都退回到进入这个块时的状态：值栈上可能压着半截表达式，
    // 异常栈里可能有一层正在被处理的内层异常。
    if (*sp > block.stackLevel) *sp = block.stackLevel;
    if (state->exceptions.size() > block.exceptionDepth) {
      state->exceptions.resize(block.exceptionDepth);
    }

    if (block.kind == Block::kWith) {
      // `__exit__` 收三个参数（类型、实例、traceback）；本语言没有
      // traceback 对象，用 None 占位——脚本里判 `tb is None` 的写法仍然成立。
      //
      // 返回值是真值就**吞掉**异常：CPython 的 `contextlib.suppress` 正是
      // 靠这一条实现的。之前没有这个判断，`__exit__` 返回 True 也照样往外抛。
      //
      // `__exit__` 自己出错时不该覆盖原始异常：CPython 把它挂成 during
      // handling，这里只保留第一个——原始原因更有用。
      if (block.withSlot < frame.locals.size()) {
        const Value manager = frame.locals[block.withSlot];
        if (!manager.isNone()) {
          Value ignored;
          bool found = false;
          std::string error;
          const bool called = ::aipy::vm::callDunder(
              program, globals, manager, "__exit__",
              {makeBuiltinClass(exceptionType(exception)), exception, Value()},
              &ignored, &found, &error);
          clearPendingException();
          if (called && found && ignored.toBool()) {
            // 被吞掉：控制从这个 with 之后继续，值栈回到进入时的深度。
            frame.pc = block.target;
            if (*sp > block.stackLevel) *sp = block.stackLevel;
            return true;
          }
        }
      }
      continue;
    }

    state->exceptions.push_back(exception);
    frame.pc = block.target;
    stack[(*sp)++] = exception;
    return true;
  }
}

}  // namespace

// 按 CPython 的规则把实参绑到局部槽上。返回 false 表示不匹配，
// `error` 里是给脚本看的消息。
//
// 绑定顺序：位置参数 → 按名匹配关键字 → 检查必需参数 → 补默认值
// → 收集 *args / **kwargs。
bool bindArguments(const Function& fn, const Value& defaults,
                   const std::vector<Value>& positional, const List& keywordKeys,
                   const List& keywordValues, std::vector<Value>* locals,
                   std::string* error) {
  const int total = fn.paramCount;
  const int kwonlyStart = fn.kwonlyStart < 0 ? total : fn.kwonlyStart;
  std::vector<bool> bound(static_cast<std::size_t>(total), false);

  // 1. 位置实参只能喂位置参数段（kwonlyStart 之前）。多出来的留给 *args。
  std::size_t positionalUsed = 0;
  for (; positionalUsed < positional.size(); ++positionalUsed) {
    if (static_cast<int>(positionalUsed) >= kwonlyStart) break;
    (*locals)[positionalUsed] = positional[positionalUsed];
    bound[positionalUsed] = true;
  }
  if (positionalUsed < positional.size() && fn.varargSlot < 0) {
    *error = "takes " + std::to_string(kwonlyStart) +
             " positional argument" + (kwonlyStart == 1 ? "" : "s") +
             " but " + std::to_string(positional.size()) + " " +
             (positional.size() == 1 ? "was" : "were") + " given";
    return false;
  }

  // 2. 关键字实参：按名字找槽位；找不到且有 **kwargs 就收进去，否则报错。
  List extraKeys;
  List extraValues;
  for (std::size_t i = 0; i < keywordKeys.size(); ++i) {
    const std::string& name = keywordKeys[i].asString();
    int slot = -1;
    for (int p = 0; p < total; ++p) {
      if (fn.paramNames[static_cast<std::size_t>(p)] == name) {
        slot = p;
        break;
      }
    }
    if (slot < 0) {
      if (fn.kwargSlot < 0) {
        *error = "got an unexpected keyword argument '" + name + "'";
        return false;
      }
      extraKeys.push_back(keywordKeys[i]);
      extraValues.push_back(keywordValues[i]);
      continue;
    }
    if (bound[static_cast<std::size_t>(slot)]) {
      *error = "got multiple values for argument '" + name + "'";
      return false;
    }
    (*locals)[static_cast<std::size_t>(slot)] = keywordValues[i];
    bound[static_cast<std::size_t>(slot)] = true;
  }

  // 3. 没绑上的：有默认值就取，没有就报缺参。
  const List& defaultValues = defaults.asList();
  if (fn.defaultCount > 0 &&
      static_cast<int>(defaultValues.size()) != total) {
    *error = "default value slot is corrupt (compiler bug)";
    return false;
  }
  std::vector<std::string> missingRequired;
  std::vector<std::string> missingKeywordOnly;
  for (int slot = 0; slot < total; ++slot) {
    if (bound[static_cast<std::size_t>(slot)]) continue;
    const bool hasDefault =
        slot < 64 && (fn.defaultMask & (std::uint64_t(1) << slot)) != 0;
    if (hasDefault) {
      (*locals)[static_cast<std::size_t>(slot)] =
          defaultValues[static_cast<std::size_t>(slot)];
      continue;
    }
    (slot >= kwonlyStart ? missingKeywordOnly : missingRequired)
        .push_back(fn.paramNames[static_cast<std::size_t>(slot)]);
  }
  // 文案照抄 CPython：位置参数与仅关键字参数的缺参报错是两套说法。
  if (!missingRequired.empty()) {
    *error = "missing " + std::to_string(missingRequired.size()) +
             " required positional argument" +
             (missingRequired.size() == 1 ? "" : "s") + ": ";
    for (std::size_t i = 0; i < missingRequired.size(); ++i) {
      if (i > 0) *error += " and " ;
      *error += "'" + missingRequired[i] + "'";
    }
    return false;
  }
  if (!missingKeywordOnly.empty()) {
    *error = "missing " + std::to_string(missingKeywordOnly.size()) +
             " required keyword-only argument" +
             (missingKeywordOnly.size() == 1 ? "" : "s") + ": ";
    for (std::size_t i = 0; i < missingKeywordOnly.size(); ++i) {
      if (i > 0) *error += " and ";
      *error += "'" + missingKeywordOnly[i] + "'";
    }
    return false;
  }

  // 4. *args：多出来的位置实参打包成元组。
  if (fn.varargSlot >= 0) {
    List rest;
    for (std::size_t i = static_cast<std::size_t>(kwonlyStart);
         i < positional.size(); ++i) {
      rest.push_back(positional[i]);
    }
    (*locals)[static_cast<std::size_t>(fn.varargSlot)] =
        Value::tuple(std::move(rest));
  }

  // 5. **kwargs：没对上任何形参的关键字打包成字典。
  if (fn.kwargSlot >= 0) {
    (*locals)[static_cast<std::size_t>(fn.kwargSlot)] =
        Value::dict(extraKeys, extraValues);
  }
  return true;
}

const Program* currentProgramPointer() { return g_currentProgram; }
void setCurrentProgramPointer(const Program* program) { g_currentProgram = program; }

}  // namespace detail

// 公开入口：扩展函数用它在 VM 调用栈内拿到 Program。
const Program* currentProgram() { return detail::currentProgramPointer(); }

using detail::applyBin;
using detail::applyCmp;
using detail::applyUnary;
using detail::bindArguments;
using detail::callMethod;
using detail::classLookupRaw;
using detail::classLookup;
using detail::descriptorValue;
using detail::instanceAttrOf;
using detail::instanceSetAttr;
using detail::superLookupRaw;
using detail::superLookupValue;
using detail::Frame;
using detail::read16;

// 异常处理块。与 `CodeGen::Block` 同名但无关：这里是运行期的保护区，
// 那边是编译期的字节码块。机器码里只有这一种，用短名读起来更顺。
using Block = detail::Block;
using ExceptionState = detail::ExceptionState;
using detail::exceptionStateOf;
using detail::peekExceptionStateOf;

// 前置声明：定义在 `Machine::callFunction` 之后，两个入口共用它。
//
// `frames` 用引用而不是按值：`Frame` 里有三个 vector（locals/blocks/
// exceptions），按值传会让每次进入执行循环都多一次整栈移动。实测那种
// 形态在 Release 下会让子线程返回时崩（0xC0000409），而 Debug 正常。
static Result execute(const Program& program, std::vector<Value>& globals,
                      std::vector<Frame>& frames,
                      const List* seedStack = nullptr);

// 推进生成器到下一个 yield。返回 true 表示产出了值；false 表示已结束
// （`failed` 为真时 `errorType`/`error` 说明抛了什么）。
//
// 复用 execute 而不是另写一个循环：生成器体就是普通函数体，语义上没有
// 任何差别，唯一区别是「跑到 YIELD 就退出这一次 execute，下次从 pc 续」。
static bool advanceGenerator(const Program& program, std::vector<Value>& globals,
                             const Value& generator, Value* out,
                             std::string* errorType, std::string* error,
                             bool* failed, Value* finalValue = nullptr);

// 同步调用一个函数值（map/filter 的回调、sorted(key=)）。定义在后面：它要
// 用 execute。返回 false 时 `errorType`/`error` 说明抛了什么。
static bool callCallable(const Program& program, std::vector<Value>& globals,
                         const Value& callable,
                         const std::vector<Value>& args, Value* out,
                         std::string* errorType, std::string* error);

// 关键字实参：`names[i]` 对应 `values[i]`。仅关键字参数（sorted 的
// key/reverse）就靠它传，不能靠加长 args——那会让 argc 与实际位置实参
// 个数混淆，内建自己无法区分。
struct KeywordArgs {
  const std::string* names = nullptr;
  const Value* values = nullptr;
  int count = 0;
};

// 调一个内建。CALL_BUILTIN / CALL_BUILTIN_KW / callCallable 共用它，
// 避免「抽干生成器 / next 特例」那几段慢慢分叉。
static bool invokeBuiltin(const Program& program, std::vector<Value>& globals,
                          std::uint16_t index, Value* args, int argc,
                          const KeywordArgs& keywords, Value* out,
                          std::string* failure);

// 协议方法调用（`__enter__` / `__exit__`）。脚本实例要真的跑脚本函数，
// 所以建帧 + execute；pylib 对象走方法表转发。with 的三条路径（进入、
// 正常退出、异常退出）共用它，否则必然分叉成「这条路径能进、那条不能」。
//
// `found` 为 false 表示该对象不支持这个协议；`error` 非空表示调用失败。
bool callDunder(const Program& program, std::vector<Value>& globals,
                const Value& target, const std::string& name,
                std::vector<Value> args, Value* out, bool* found,
                std::string* error);

// 进程级的全局槽。定义在这里、声明在 vm.h：所有 Machine 实例共用同一份，
// 线程里 `global X` 才写得到主线程看得见的那个槽。
std::vector<Value> Machine::globals_;

Result Machine::run(const Program& program) {
  if (static_cast<int>(globals_.size()) < program.globalCount) {
    globals_.resize(static_cast<size_t>(program.globalCount));
  }

  Frame moduleFrame;
  moduleFrame.code = &program.code;
  moduleFrame.lines = &program.lines;
  moduleFrame.function = nullptr;
  moduleFrame.pc = 0;
  moduleFrame.stackBase = 0;
  moduleFrame.locals.assign(static_cast<size_t>(program.localCount > 0 ? program.localCount : 1),
                            Value());

  std::vector<Frame> frames;
  frames.reserve(16);
  frames.push_back(std::move(moduleFrame));
  return execute(program, globals_, frames);
}

Result Machine::callFunction(const Program& program, const std::string& name,
                             const std::vector<Value>& args) {  Result result;
  result.failed = true;

  // 按名字找函数。脚本函数不是值（Value 没有函数类型，`g = f` 会被
  // 编译器拒绝），所以线程库只能用名字指定要跑哪个函数。
  const Function* target = nullptr;
  for (const Function& candidate : program.functions) {
    if (candidate.name == name) {
      target = &candidate;
      break;
    }
  }
  if (target == nullptr) {
    result.error = "找不到函数 " + name;
    return result;
  }
  if (static_cast<int>(args.size()) != target->paramCount) {
    result.error = "函数 " + name + " 需要 " + std::to_string(target->paramCount) +
                   " 个参数，实际给了 " + std::to_string(args.size()) + " 个";
    return result;
  }

  Frame callee;
  callee.code = &target->code;
  callee.lines = &target->lines;
  callee.function = target;
  callee.pc = 0;
  callee.stackBase = 0;
  callee.locals.assign(static_cast<size_t>(target->localCount), Value());
  for (std::size_t i = 0; i < args.size(); ++i) callee.locals[i] = args[i];

  // 兜底：全局槽是进程级的，正常情况下 run() 已经按 globalCount 扩好。
  // 这里再查一次，是为了让「还没跑过模块级就被扩展调用」时不越界——
  // 越界的症状是读到空槽，脚本看到 None 而不是预期的值。
  if (static_cast<int>(globals_.size()) < program.globalCount) {
    globals_.resize(static_cast<size_t>(program.globalCount));
  }

  std::vector<Frame> frames;
  frames.reserve(16);
  frames.push_back(std::move(callee));
  return execute(program, globals_, frames);
}

// 「按值建帧」：把可调用值 + 已就绪的位置实参建成一帧。kCallMethod 的
// 脚本实例分支要用（它已按 MRO 找好函数并绑好 self）。
//
// 返回 true 表示压了帧（正常调用）；返回 false 且 `produced` 非空表示这是个
// 协程/生成器函数——调用它不执行函数体，只产出对象。`error` 非空才是失败。
static bool pushValueFrame(const Program& program, const Value& callable,
                           std::vector<Value> positional,
                           std::vector<Frame>& frames, std::size_t stackBase,
                           std::string* error, Value* produced = nullptr) {
  if (produced != nullptr) *produced = Value();
  const int index = functionIndexOf(callable);
  if (index < 0) {
    *error = "'" + typeName(callable) + "' object is not callable";
    return false;
  }
  if (index >= static_cast<int>(program.functions.size())) {
    *error = "函数号越界（编译器 bug）";
    return false;
  }
  if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
    *error = "调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
             "，脚本里可能有无限递归";
    return false;
  }
  const Function& fn = program.functions[static_cast<std::size_t>(index)];

  std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
  // 默认值在这里补不了（拿不到 globals）：脚本方法的默认参数很少见，
  // 真需要时调用方会把位补齐。
  std::string bindError;
  if (!bindArguments(fn, Value(), positional, List{}, List{}, &locals,
                     &bindError)) {
    *error = fn.name + "() " + bindError;
    return false;
  }

  const List& captures = functionCaptures(callable);
  if (!captures.empty() && fn.captureStart >= 0) {
    for (std::size_t i = 0; i < captures.size(); ++i) {
      const std::size_t slot = static_cast<std::size_t>(fn.captureStart) + i;
      if (slot < locals.size()) locals[slot] = captures[i];
    }
  }

  // 协程 / 生成器：建帧就等于执行函数体，所以只把局部量封进对象。
  if (produced != nullptr && (fn.isCoroutine || fn.isGenerator)) {
    *produced = fn.isCoroutine ? makeCoroutine(index, std::move(locals))
                               : makeGenerator(index, std::move(locals));
    return false;
  }

  Frame callee;
  callee.code = &fn.code;
  callee.lines = &fn.lines;
  callee.function = &fn;
  callee.pc = 0;
  callee.stackBase = stackBase;
  callee.locals = std::move(locals);
  frames.push_back(std::move(callee));
  return true;
}

// 建一帧并压入。
static void pushFrame(const Function& fn, std::vector<Value> locals,
                      std::vector<Frame>& frames, std::size_t stackBase) {
  Frame callee;
  callee.code = &fn.code;
  callee.lines = &fn.lines;
  callee.function = &fn;
  callee.pc = 0;
  callee.stackBase = stackBase;
  callee.locals = std::move(locals);
  frames.push_back(std::move(callee));
}

// 调一个内建。CALL_BUILTIN 与 CALL_BUILTIN_KW 共用这一段，避免两份
// 「抽干生成器 / next 特例」慢慢分叉。
//
// `sep` / `end` 只对 print 有意义：空 Value 表示「没给」。print 的默认
// 分隔符与结尾由 print 自己补，所以这里只在非空时把参数个数撑到它们的位置。
//
// 返回 false 表示异常已抛出（raiseException），调用方不要再压返回值。
static bool invokeBuiltin(const Program& program, std::vector<Value>& globals,
                          std::uint16_t index, Value* args, int argc,
                          const KeywordArgs& keywords, Value* out,
                          std::string* failure) {
  failure->clear();
  // `next(g)`：推进一格。只能在这里做——值层拿不到 Program。
  if (program.builtinNames[index] == "next") {
    if (argc != 1) {
      raiseException("TypeError", "next() takes exactly one argument (" +
                                      std::to_string(argc) + " given)");
      return false;
    }
    if (!isGenerator(args[0])) {
      raiseException("TypeError", "'" + typeName(args[0]) +
                                      "' object is not an iterator");
      return false;
    }
    Value produced;
    std::string errorType;
    std::string error;
    bool failed = false;
    if (advanceGenerator(program, globals, args[0], &produced, &errorType,
                         &error, &failed)) {
      *out = produced;
      return true;
    }
    if (failed) {
      raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
      return false;
    }
    raiseException("StopIteration", "");
    return false;
  }

  // 生成器先抽干成列表：内建（list/sum/len…）只认容器，而值层的
  // iterate() 拿不到 Program，无法推进生成器。
  for (int i = 0; i < argc; ++i) {
    if (!isGenerator(args[i])) continue;
    List collected;
    Value produced;
    std::string errorType;
    std::string error;
    bool failed = false;
    while (advanceGenerator(program, globals, args[i], &produced, &errorType,
                            &error, &failed)) {
      collected.push_back(produced);
    }
    if (failed) {
      raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
      return false;
    }
    args[i] = Value::list(std::move(collected));
  }

  const std::string& builtinName = program.builtinNames[index];

  // map(func, iterable) / filter(func, iterable)：要真的调脚本函数。
  // 放在「生成器抽干」之后，所以两种内建都能吃生成器参数。
  if (builtinName == "map" || builtinName == "filter") {
    if (argc != 2) {
      *failure = builtinName + " expected 2 arguments, got " +
                 std::to_string(argc);
      return false;
    }
    // filter(None, xs)：按真值过滤，与 CPython 一致。
    const bool byTruth = builtinName == "filter" && args[0].isNone();
    if (!byTruth && functionIndexOf(args[0]) < 0 &&
        !isBuiltinFunction(args[0]) && !isBuiltinClass(args[0])) {
      *failure = "'" + typeName(args[0]) + "' object is not callable";
      return false;
    }
    List collected;
    for (const Value& item : iterate(args[1])) {
      Value verdict;
      if (byTruth) {
        verdict = item;
      } else {
        std::string errorType;
        std::string error;
        if (!callCallable(program, globals, args[0], {item}, &verdict,
                          &errorType, &error)) {
          raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
          return false;
        }
      }
      if (builtinName == "map") {
        collected.push_back(verdict);
      } else if (verdict.toBool()) {
        collected.push_back(item);
      }
    }
    *out = Value::list(std::move(collected));
    return true;
  }

  // min/max 带 key=：键要先求出来，值层调不了脚本函数。
  // `min(xs, key=f)` 与 `min(a, b, key=f)` 两种形态都要支持。
  if ((builtinName == "min" || builtinName == "max") && keywords.count > 0) {
    Value key;
    for (int i = 0; i < keywords.count; ++i) {
      if (keywords.names[i] == "key") key = keywords.values[i];
    }
    if (argc == 0) {
      *failure = builtinName + " expected at least 1 argument, got 0";
      return false;
    }
    const bool ascending = builtinName == "min";
    // 单参数是容器，多参数是候选值。
    List candidates = argc == 1 ? iterate(args[0]) : List(args, args + argc);
    if (candidates.empty()) {
      *failure = builtinName + "() arg is an empty sequence";
      return false;
    }
    Value best = candidates[0];
    Value bestKey;
    if (!key.isNone()) {
      std::string errorType;
      std::string error;
      if (!callCallable(program, globals, key, {best}, &bestKey, &errorType,
                        &error)) {
        raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
        return false;
      }
    }
    for (std::size_t i = 1; i < candidates.size(); ++i) {
      Value candidateKey;
      if (!key.isNone()) {
        std::string errorType;
        std::string error;
        if (!callCallable(program, globals, key, {candidates[i]}, &candidateKey,
                          &errorType, &error)) {
          raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
          return false;
        }
      }
      const bool better =
          key.isNone()
              ? (ascending ? lt(candidates[i], best).toBool()
                           : gt(candidates[i], best).toBool())
              : (ascending ? lt(candidateKey, bestKey).toBool()
                           : gt(candidateKey, bestKey).toBool());
      if (better) {
        best = candidates[i];
        bestKey = candidateKey;
      }
    }
    *out = best;
    return true;
  }

  // dict(a=1, b=2)：关键字就是键值对，键是字符串。位置实参仍按
  // dict(pairs) 的解释，关键字覆盖同名键（与 CPython 一致）。
  if (builtinName == "dict" && keywords.count > 0) {
    Value base;
    if (argc > 0) {
      if (argc != 1) {
        *failure = "dict expected at most 1 positional argument, got " +
                   std::to_string(argc);
        return false;
      }
      if (args[0].isDict()) {
        base = Value::dict(args[0].dictKeys(), args[0].dictValues());
      } else {
        base = dictFromPairs(args[0]);
        if (hasPendingException()) return false;
      }
    } else {
      base = Value::dict({}, {});
    }
    for (int i = 0; i < keywords.count; ++i) {
      setItem(base, Value(keywords.names[i]), keywords.values[i]);
    }
    *out = base;
    return true;
  }

  // sum(iterable, start=0)：生成器已在上面抽干成列表，这里只管累加。
  if (builtinName == "sum") {
    Value start;
    for (int i = 0; i < keywords.count; ++i) {
      if (keywords.names[i] == "start") start = keywords.values[i];
    }
    if (argc < 1) {
      *failure = "sum expected at least 1 argument, got 0";
      return false;
    }
    if (argc > 1) start = args[1];
    Value total = start.isNone() ? Value(0LL) : start;
    for (const Value& item : iterate(args[0])) total = add(total, item);
    *out = total;
    return true;
  }

  // print 的 sep/end。
  if (builtinName == "print") {
    Value sep;
    Value end;
    for (int i = 0; i < keywords.count; ++i) {
      if (keywords.names[i] == "sep") {
        sep = keywords.values[i];
      } else if (keywords.names[i] == "end") {
        end = keywords.values[i];
      }
    }
    *out = aipy::printWithSeparators(args, argc, sep, end);
    return true;
  }

  // sorted 的 key / reverse 是仅关键字参数。
  if (builtinName == "sorted") {
    if (argc != 1) {
      *failure = "sorted expected 1 argument, got " + std::to_string(argc);
      return false;
    }
    Value key;
    bool reverse = false;
    for (int i = 0; i < keywords.count; ++i) {
      if (keywords.names[i] == "key") {
        key = keywords.values[i];
      } else if (keywords.names[i] == "reverse") {
        reverse = keywords.values[i].toBool();
      }
    }
    // 有 key 时先对每个元素求一次键，再按键排序。只求一次是关键：key 带
    // 副作用时（计数、日志）求两次的结果与 CPython 不同。
    if (!key.isNone()) {
      const List items = iterate(args[0]);
      List keys;
      keys.reserve(items.size());
      for (const Value& item : items) {
        Value produced;
        std::string errorType;
        std::string error;
        if (!callCallable(program, globals, key, {item}, &produced, &errorType,
                          &error)) {
          raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
          return false;
        }
        keys.push_back(produced);
      }
      *out = sortedCopy(args[0], Value::list(std::move(keys)), reverse);
      return true;
    }
    *out = sortedCopy(args[0], Value(), reverse);
    return true;
  }

  // round(x, ndigits=None) / enumerate(iterable, start=0) / str(object='')
  // 这几个的关键字就是它们的位置参数别名，直接换成位置。
  if (builtinName == "round" || builtinName == "enumerate" ||
      builtinName == "str") {
    for (int i = 0; i < keywords.count; ++i) {
      int position = -1;
      if (builtinName == "round" && keywords.names[i] == "ndigits") position = 1;
      if (builtinName == "enumerate" && keywords.names[i] == "start") position = 1;
      if (builtinName == "str" && keywords.names[i] == "object") position = 0;
      if (position < 0) continue;
      while (argc <= position) args[argc++] = Value();
      args[position] = keywords.values[i];
    }
  }

  // int 的 base 是仅关键字参数。
  if (builtinName == "int") {
    if (argc != 1) {
      *failure = "int expected 1 argument, got " + std::to_string(argc);
      return false;
    }
    long long base = 10;
    for (int i = 0; i < keywords.count; ++i) {
      if (keywords.names[i] == "base") base = keywords.values[i].toInt();
    }
    if (base == 10) {
      // 走原来的实现（含错误消息）。
      std::string error;
      const Value value = program.builtins[index](args, argc, &error);
      if (!error.empty()) {
        *failure = error;
        return false;
      }
      *out = value;
      return true;
    }
    *out = integerFromBase(args[0], base);
    return true;
  }

  std::string error;
  const Value value = program.builtins[index](args, argc, &error);
  if (!error.empty()) {
    *failure = error;
    return false;
  }
  *out = value;
  return true;
}

// 执行循环本体：帧已建好，从 frames.back() 开始跑。
//
// 独立函数而不是成员，只是为了让 `run` 与 `callFunction` 共用同一段
// switch。**switch 本身不许拆**（见文件头与 AGENTS.md 铁律六）——那会让
// 每条指令多一次调用返回。
//
// `globals` 由调用方传：`run` 用 Machine 自己的槽，`callFunction` 也用
// 同一个——子线程跑脚本函数时应该看见同一份全局状态，这与 Python 里
// 线程共享模块全局变量的语义一致。
static Result execute(const Program& program, std::vector<Value>& globals,
                      std::vector<Frame>& frames,
                      const List* seedStack) {
  Result result;

  // 「当前正在跑哪个程序」。扩展函数只拿到 `const Value*`，拿不到 Program；
  // 线程库需要它才能把脚本函数搬到子线程去跑。给指针而不是副本：拷贝一份
  // Program 要复制整个字节码，而这里只在调用栈内有效、不跨越任何边界。
  const Program* previous = detail::currentProgramPointer();
  detail::setCurrentProgramPointer(&program);
  struct Restore {
    const Program* previous;
    ~Restore() { detail::setCurrentProgramPointer(previous); }
  } restore{previous};

  // 共享值栈。8192 深：脚本不是任意表达式生成器，超过这个深度几乎只
  // 可能是编译器 bug（比如跳转回填错导致死循环压栈）。报错比崩好。
  constexpr std::size_t kStackSize = 8192;
  std::vector<Value> stack(kStackSize);
  std::size_t sp = 0;

  // 恢复生成器：把暂停时存下的栈段摆回 stackBase 起，接着跑。
  if (seedStack != nullptr && !frames.empty()) {
    const std::size_t base = frames.back().stackBase;
    for (std::size_t i = 0; i < seedStack->size() && base + i < kStackSize; ++i) {
      stack[base + i] = (*seedStack)[i];
    }
    sp = base + seedStack->size();
  }

  // 取当前帧对应的行号。没找到就说 0（宿主可以据此决定要不要显示位置）。
  const auto lineOf = [&]() -> int {
    const Frame& frame = frames.back();
    if (frame.lines == nullptr) return 0;
    int line = 0;
    for (const LineEntry& entry : *frame.lines) {
      if (entry.offset > frame.pc) break;
      line = entry.line;
    }
    return line;
  };

  const auto fail = [&](const std::string& message) {
    result.failed = true;
    result.error = message;
    result.line = lineOf();
    return result;
  };

  // 异常展开与判定的薄封装：真正的逻辑在不内联的 unwindException 里，
  // 原因见它的注释（子线程栈只有 1 MB）。
  const auto unwind = [&](const Value& exception) -> bool {
    return detail::unwindException(frames, stack, &sp, &result, exception,
                                   program, globals);
  };

  while (!frames.empty()) {
    Frame& frame = frames.back();

    if (frame.pc >= frame.code->size()) {
      // 跑到末尾：模块级正常结束，函数则视作 return None。
      if (frames.size() == 1) {
        result.value = sp > 0 ? stack[sp - 1] : Value();
        return result;
      }
      result.value = Value();
      sp = frame.stackBase;
      frames.pop_back();
      continue;
    }

    const std::vector<std::uint8_t>& code = *frame.code;
    const auto op = static_cast<Op>(code[frame.pc++]);

    // ── 异常通道的收尾 ──
    //
    // 值层失败时不中断指令，只留下「待处理异常」（见 value.h）。这里取出
    // 并展开。放在 switch **之前**：上一条指令已经完整执行完，新指令还没
    // 开始，此时展开不会破坏任何中间状态。
    //
    // 为什么不用 C++ 异常：那会给每个调用点加 unwind 表，而这条执行路径
    // 的全部理由就是「一次 switch + 几次直接下标」（铁律六）。
    if (hasPendingException()) {
      std::string type;
      std::string message;
      if (takePendingException(&type, &message)) {
        if (!unwind(makeException(type, message))) return result;
        continue;
      }
    }

    switch (op) {
      case Op::kConst: {
        const std::uint16_t index = read16(code, frame.pc);
        frame.pc += 2;
        if (sp >= kStackSize) return fail("操作数栈溢出");
        stack[sp++] = program.consts[index];
        break;
      }
      case Op::kNone: stack[sp++] = Value(); break;
      case Op::kTrue: stack[sp++] = Value(true); break;
      case Op::kFalse: stack[sp++] = Value(false); break;

      case Op::kLoadLocal: {
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("局部槽越界（编译器 bug）");
        if (frame.locals[slot].isDeleted()) {
          raiseException("NameError",
                         "local variable '" +
                             frame.locals[slot].deletedName() +
                             "' referenced before assignment");
          break;
        }
        stack[sp++] = frame.locals[slot];
        break;
      }
      case Op::kStoreLocal: {
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("局部槽越界（编译器 bug）");
        frame.locals[slot] = stack[--sp];
        break;
      }
      case Op::kLoadGlobal: {
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= globals.size()) return fail("全局槽越界（编译器 bug）");
        if (globals[slot].isDeleted()) {
          raiseException("NameError",
                         "name '" + globals[slot].deletedName() +
                             "' is not defined");
          break;
        }        stack[sp++] = globals[slot];
        break;
      }
      case Op::kStoreGlobal: {
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= globals.size()) return fail("全局槽越界（编译器 bug）");
        globals[slot] = stack[--sp];
        break;
      }

      case Op::kDelLocal: {
        // u16 槽号 + u16 名字常量号：名字只用于 NameError 消息。
        const std::uint16_t slot = read16(code, frame.pc);
        const std::uint16_t nameIndex = read16(code, frame.pc + 2);
        frame.pc += 4;
        if (slot >= frame.locals.size()) return fail("局部槽越界（编译器 bug）");
        if (nameIndex >= program.consts.size()) {
          return fail("删除的名字越界（编译器 bug）");
        }
        if (frame.locals[slot].isDeleted()) {
          raiseException("NameError", "name '" +
                                          program.consts[nameIndex].toString() +
                                          "' is not defined");
          break;
        }
        frame.locals[slot] =
            Value::deleted(program.consts[nameIndex].toString());
        break;
      }

      case Op::kDelGlobal: {
        const std::uint16_t slot = read16(code, frame.pc);
        const std::uint16_t nameIndex = read16(code, frame.pc + 2);
        frame.pc += 4;
        if (slot >= globals.size()) return fail("全局槽越界（编译器 bug）");
        if (nameIndex >= program.consts.size()) {
          return fail("删除的名字越界（编译器 bug）");
        }
        if (globals[slot].isDeleted()) {
          raiseException("NameError", "name '" +
                                          program.consts[nameIndex].toString() +
                                          "' is not defined");
          break;
        }
        globals[slot] = Value::deleted(program.consts[nameIndex].toString());
        break;
      }

      case Op::kBin: {
        const auto operand = static_cast<BinOp>(code[frame.pc++]);
        const Value b = stack[--sp];
        stack[sp - 1] = applyBin(operand, stack[sp - 1], b);
        break;
      }
      case Op::kCmp: {
        const auto operand = static_cast<CmpOp>(code[frame.pc++]);
        const Value b = stack[--sp];
        stack[sp - 1] = applyCmp(operand, stack[sp - 1], b);
        break;
      }
      case Op::kUnary: {
        const auto operand = static_cast<UnOp>(code[frame.pc++]);
        stack[sp - 1] = applyUnary(operand, stack[sp - 1]);
        break;
      }

      case Op::kJump:
        frame.pc = read16(code, frame.pc);
        break;
      case Op::kJumpIfFalse: {
        const std::size_t target = read16(code, frame.pc);
        frame.pc += 2;
        // 条件已求值、必须弹掉；Keep 版本才是「把栈顶留着」。
        if (!stack[--sp].toBool()) frame.pc = target;
        break;
      }
      case Op::kJumpIfFalseKeep: {
        const std::size_t target = read16(code, frame.pc);
        frame.pc += 2;
        if (!stack[sp - 1].toBool()) frame.pc = target;
        break;
      }
      case Op::kJumpIfTrueKeep: {
        const std::size_t target = read16(code, frame.pc);
        frame.pc += 2;
        if (stack[sp - 1].toBool()) frame.pc = target;
        break;
      }

      case Op::kPop: --sp; break;
      case Op::kDup: stack[sp] = stack[sp - 1]; ++sp; break;

      case Op::kCall: {
        const std::uint16_t index = read16(code, frame.pc);
        const std::uint8_t argc = code[frame.pc + 2];
        frame.pc += 3;
        if (index >= program.functions.size()) return fail("函数号越界（编译器 bug）");
        if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
          return fail("调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
                      "，脚本里可能有无限递归");
        }
        const Function& fn = program.functions[index];

        std::vector<Value> positional(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
          positional[static_cast<std::size_t>(i)] = stack[sp - static_cast<std::size_t>(argc) + static_cast<std::size_t>(i)];
        }

        std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
        const Value defaults =
            fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
                ? globals[static_cast<std::size_t>(fn.defaultSlot)]
                : Value();
        std::string bindError;
        if (!bindArguments(fn, defaults, positional, List{}, List{}, &locals,
                           &bindError)) {
          raiseException("TypeError", fn.name + "() " + bindError);
          break;
        }
        sp -= static_cast<std::size_t>(argc);

        // 生成器函数：不建帧，只把已绑好的局部量塞进生成器对象。函数体
        // 一行都不跑——`g = gen()` 与 yield 之间的先后顺序是 CPython 可
        // 观察到的分界。
        if (fn.isCoroutine) {
          stack[sp++] = makeCoroutine(index, std::move(locals));
          break;
        }
        if (fn.isGenerator) {
          stack[sp++] = makeGenerator(index, std::move(locals));
          break;
        }

        pushFrame(fn, std::move(locals), frames, sp);
        break;
      }

      case Op::kDictMerge: {
        // 栈上是 [目标字典, 来源字典]。键重复抛 TypeError，与 CPython 一致。
        if (sp < 2) return fail("字典合并缺少操作数（编译器 bug）");
        const Value source = stack[--sp];
        Value& target = stack[sp - 1];
        if (!target.isDict() || !source.isDict()) {
          raiseException("TypeError",
                         "argument after ** must be a mapping, not " +
                             typeName(source));
          break;
        }
        const List& keys = source.dictKeys();
        const List& values = source.dictValues();
        bool duplicate = false;
        std::string duplicateName;
        for (std::size_t i = 0; i < keys.size(); ++i) {
          if (target.dictIndex(keys[i]) >= 0) {
            duplicate = true;
            duplicateName = keys[i].toString();
            break;
          }
          setItem(target, keys[i], values[i]);
        }
        if (duplicate) {
          raiseException("TypeError",
                         "got multiple values for keyword argument '" +
                             duplicateName + "'");
        }
        break;
      }

      case Op::kCallDyn: {
        // 栈上是 [位置实参列表, 关键字字典]。位置实参的**个数**与关键字的
        // **名字**都要到运行期才知道（`*args` / `**kwargs` 展开），所以没办
        // 法像 kCall 那样在编译期拍平。
        const std::uint16_t index = read16(code, frame.pc);
        frame.pc += 2;
        if (index >= program.functions.size()) return fail("函数号越界（编译器 bug）");
        if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
          return fail("调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
                      "，脚本里可能有无限递归");
        }
        const Function& fn = program.functions[index];

        const Value keywords = stack[--sp];
        const Value positionals = stack[--sp];

        std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
        const Value defaults =
            fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
                ? globals[static_cast<std::size_t>(fn.defaultSlot)]
                : Value();
        std::string bindError;
        if (!bindArguments(fn, defaults, positionals.asList(),
                           keywords.dictKeys(), keywords.dictValues(), &locals,
                           &bindError)) {
          raiseException("TypeError", fn.name + "() " + bindError);
          break;
        }

        Frame callee;
        callee.code = &fn.code;
        callee.lines = &fn.lines;
        callee.function = &fn;
        callee.pc = 0;
        callee.stackBase = sp;
        callee.locals = std::move(locals);
        frames.push_back(std::move(callee));
        break;
      }

      case Op::kMakeFunction: {
        const std::uint16_t index = read16(code, frame.pc);
        const std::uint16_t captureCount = read16(code, frame.pc + 2);
        frame.pc += 4;
        if (index >= program.functions.size()) return fail("函数号越界（编译器 bug）");
        List captures;
        captures.reserve(captureCount);
        for (int i = captureCount; i > 0; --i) {
          captures.push_back(stack[sp - static_cast<std::size_t>(i)]);
        }
        sp -= captureCount;
        stack[sp++] = makeFunction(index, std::move(captures));
        break;
      }

      case Op::kCallValue: {
        // 栈上是 [可调用值, 位置实参...]。值到运行期才知道指向哪个函数，
        // 所以不能像 kCall 那样把函数号写进指令。
        const std::uint8_t argc = code[frame.pc];
        frame.pc += 1;
        const std::size_t base = sp - static_cast<std::size_t>(argc) - 1;
        const Value callable = stack[base];
        // 内建函数值（`f = len` 之后 `f(x)`）与内建类型值（`f = int`）：
        // 都同步跑完拿到返回值。它们不能走下面那条「建帧后继续执行循环」
        // 的路径——内建没有字节码帧。
        if (isBuiltinFunction(callable) || isBuiltinClass(callable)) {
          std::vector<Value> callArgs;
          callArgs.reserve(static_cast<std::size_t>(argc));
          for (int i = 0; i < argc; ++i) {
            callArgs.push_back(stack[base + 1 + static_cast<std::size_t>(i)]);
          }
          Value produced;
          std::string errorType;
          std::string error;
          const bool ok = callCallable(program, globals, callable, callArgs,
                                       &produced, &errorType, &error);
          sp = base;
          if (!ok) {
            raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
            break;
          }
          stack[sp++] = produced;
          break;
        }
        // 已绑定实例的方法值：函数号在同一个字段里，self 单独存。
        const int index = functionIndexOf(callable);
        if (index < 0) {
          raiseException("TypeError", "'" + typeName(callable) +
                                          "' object is not callable");
          break;
        }
        if (index >= static_cast<int>(program.functions.size())) {
          return fail("函数号越界（编译器 bug）");
        }
        if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
          return fail("调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
                      "，脚本里可能有无限递归");
        }
        const Function& fn = program.functions[static_cast<std::size_t>(index)];

        std::vector<Value> positional;
        positional.reserve(static_cast<std::size_t>(argc) + 1);
        // 方法值（`obj.method`）把 self 补成第一个位置实参。脚本方法的
        // 第一个形参就是 self，编译器已把它算进 paramCount。
        if (isBoundMethod(callable)) positional.push_back(boundMethodSelf(callable));
        for (int i = 0; i < argc; ++i) {
          positional.push_back(stack[base + 1 + static_cast<std::size_t>(i)]);
        }

        std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
        const Value defaults =
            fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
                ? globals[static_cast<std::size_t>(fn.defaultSlot)]
                : Value();
        std::string bindError;
        if (!bindArguments(fn, defaults, positional, List{}, List{}, &locals,
                           &bindError)) {
          raiseException("TypeError", fn.name + "() " + bindError);
          break;
        }

        // 闭包捕获：函数的那句 def/lambda 在外层求值时记下的值，按顺序
        // 填进 captureStart 起的槽。这些槽是编译器分配好的具名局部槽
        // （lambda 体里 `LOAD_LOCAL` 直接按名字读它们）。
        const List& captures = functionCaptures(callable);
        if (!captures.empty() && fn.captureStart >= 0) {
          for (std::size_t i = 0; i < captures.size(); ++i) {
            const std::size_t slot =
                static_cast<std::size_t>(fn.captureStart) + i;
            if (slot < locals.size()) locals[slot] = captures[i];
          }
        }

        sp = base;

        // 生成器：只造对象，不执行函数体。捕获值已填进 locals，一并搬过去。
        if (fn.isCoroutine) {
          stack[sp++] = makeCoroutine(index, std::move(locals));
          break;
        }
        if (fn.isGenerator) {
          stack[sp++] = makeGenerator(index, std::move(locals));
          break;
        }

        Frame callee;
        callee.code = &fn.code;
        callee.lines = &fn.lines;
        callee.function = &fn;
        callee.pc = 0;
        callee.stackBase = sp;
        callee.locals = std::move(locals);
        frames.push_back(std::move(callee));
        break;
      }
      case Op::kCallValueDyn: {
        // 栈上是 [可调用值, 位置实参列表, 关键字字典]。与 kCallValue 同一套
        // 绑定，只是实参的形状到运行期才知道（`f(*args, **kw)`）。
        const Value keywords = stack[--sp];
        const Value positionals = stack[--sp];
        const Value callable = stack[--sp];
        const int index = functionIndexOf(callable);
        if (index < 0) {
          raiseException("TypeError", "'" + typeName(callable) +
                                          "' object is not callable");
          break;
        }
        if (index >= static_cast<int>(program.functions.size())) {
          return fail("函数号越界（编译器 bug）");
        }
        if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
          return fail("调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
                      "，脚本里可能有无限递归");
        }
        const Function& fn = program.functions[static_cast<std::size_t>(index)];

        std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
        const Value defaults =
            fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
                ? globals[static_cast<std::size_t>(fn.defaultSlot)]
                : Value();
        std::string bindError;
        if (!bindArguments(fn, defaults, positionals.asList(),
                           keywords.dictKeys(), keywords.dictValues(), &locals,
                           &bindError)) {
          raiseException("TypeError", fn.name + "() " + bindError);
          break;
        }

        const List& captures = functionCaptures(callable);
        if (!captures.empty() && fn.captureStart >= 0) {
          for (std::size_t i = 0; i < captures.size(); ++i) {
            const std::size_t slot =
                static_cast<std::size_t>(fn.captureStart) + i;
            if (slot < locals.size()) locals[slot] = captures[i];
          }
        }

        // 生成器：只造对象，不执行函数体。
        if (fn.isCoroutine) {
          stack[sp++] = makeCoroutine(index, std::move(locals));
          break;
        }
        if (fn.isGenerator) {
          stack[sp++] = makeGenerator(index, std::move(locals));
          break;
        }

        Frame callee;
        callee.code = &fn.code;
        callee.lines = &fn.lines;
        callee.function = &fn;
        callee.pc = 0;
        callee.stackBase = sp;
        callee.locals = std::move(locals);
        frames.push_back(std::move(callee));
        break;
      }

      case Op::kMakeClass: {
        // 栈上是 [基类...] 再 [成员值...]。成员名是紧跟指令的 u16 常量号
        // （个数 = 成员个数），基类个数单独给。
        const std::uint16_t nameIndex = read16(code, frame.pc);
        const std::uint16_t memberCount = read16(code, frame.pc + 2);
        const std::uint16_t baseCount = read16(code, frame.pc + 4);
        frame.pc += 6;
        if (nameIndex >= program.consts.size()) return fail("类名越界（编译器 bug）");
        if (memberCount > 64 || baseCount > 64) return fail("类成员过多（编译器 bug）");

        std::uint16_t memberNameIndices[64];
        for (int i = 0; i < memberCount; ++i) {
          memberNameIndices[i] = read16(code, frame.pc);
          frame.pc += 2;
        }

        List members;
        members.reserve(memberCount);
        // 栈上从低到高就是书写顺序，直接顺取；用 insert(begin) 会整体
        // 反转，症状是成员名与函数值错配（`__init__` 拿到最后一个函数）。
        for (int i = memberCount; i > 0; --i) {
          members.push_back(stack[sp - static_cast<std::size_t>(i)]);
        }
        sp -= memberCount;

        List bases;
        bases.reserve(baseCount);
        for (int i = baseCount; i > 0; --i) {
          bases.push_back(stack[sp - static_cast<std::size_t>(i)]);
        }
        sp -= baseCount;

        List keys;
        keys.reserve(memberCount);
        for (int i = 0; i < memberCount; ++i) {
          keys.push_back(program.consts[memberNameIndices[i]]);
        }

        stack[sp++] = makeClass(program.consts[nameIndex].asString(),
                                std::move(bases), std::move(keys),
                                std::move(members));
        break;
      }

      case Op::kGetAttr: {
        const std::uint16_t nameIndex = read16(code, frame.pc);
        frame.pc += 2;
        if (nameIndex >= program.consts.size()) return fail("属性名越界（编译器 bug）");
        if (sp < 1) return fail("属性访问缺少接收者（编译器 bug）");
        const Value target = stack[--sp];
        const std::string name = program.consts[nameIndex].asString();

        if (isInstance(target)) {
          std::string error;
          const Value value = instanceAttrOf(target, name, &error);
          if (!error.empty()) {
            raiseException("AttributeError", error);
            break;
          }
          stack[sp++] = value;
          break;
        }
        // 类对象：`A.x` 直接取类成员，但要走描述符语义——`A.cm` 要绑
        // 到 A 本身，`A.sm` 与 `A.m` 返回裸函数（CPython 里 py3 的方法
        // 通过类访问不绑定）。
        if (isClass(target)) {
          Value member;
          if (!classLookup(target, name, &member)) {
            raiseException("AttributeError", "type object '" + classNameOf(target) +
                                                 "' has no attribute '" + name + "'");
            break;
          }
          stack[sp++] = descriptorValue(member, target, false);
          break;
        }
        // `super().m` 作为值取用（不是立刻调用）：与 kCallMethod 同一套
        // 描述符语义，只是把结果压栈。
        if (isSuper(target)) {
          const Value self = superSelf(target);
          const Value current = superClassOf(target);
          Value member;
          if (!superLookupValue(self, current, name, &member)) {
            raiseException("AttributeError",
                           "'super' object has no attribute '" + name + "'");
            break;
          }
          stack[sp++] = descriptorValue(member, self, isInstance(self));
          break;
        }
        raiseException("AttributeError",
                       "'" + typeName(target) + "' object has no attribute '" +
                           name + "'");
        break;
      }

      case Op::kSetAttr: {
        const std::uint16_t nameIndex = read16(code, frame.pc);
        frame.pc += 2;
        if (nameIndex >= program.consts.size()) return fail("属性名越界（编译器 bug）");
        if (sp < 2) return fail("属性赋值缺少操作数（编译器 bug）");
        const Value value = stack[--sp];
        const Value target = stack[--sp];
        const std::string name = program.consts[nameIndex].asString();
        if (isInstance(target)) {
          instanceSetAttr(target, name, value);
          break;
        }
        if (isClass(target)) {
          List& keys = const_cast<List&>(classMemberKeys(target));
          List& values = const_cast<List&>(classMemberValues(target));
          bool replaced = false;
          for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].asString() == name) {
              values[i] = value;
              replaced = true;
              break;
            }
          }
          if (!replaced) {
            keys.push_back(Value(name));
            values.push_back(value);
          }
          break;
        }
        raiseException("AttributeError",
                       "'" + typeName(target) + "' object has no attribute '" +
                           name + "'");
        break;
      }

      case Op::kInstantiate: {
        const std::uint8_t argc = code[frame.pc];
        frame.pc += 1;
        const std::size_t base = sp - static_cast<std::size_t>(argc) - 1;
        const Value cls = stack[base];
        if (!isClass(cls)) {
          raiseException("TypeError", "'" + typeName(cls) +
                                          "' object is not callable");
          break;
        }
        const Value instance = makeInstance(cls);

        // 有 __init__ 就建帧调它；返回值由 kReturn 用 initInstance 顶替
        // （CPython 里 `A()` 的结果永远是实例）。
        const Value init = classLookupRaw(cls, "__init__");
        if (isFunction(init)) {
          const int index = functionIndexOf(init);
          if (index < 0 || index >= static_cast<int>(program.functions.size())) {
            return fail("函数号越界（编译器 bug）");
          }
          if (frames.size() >= static_cast<std::size_t>(Machine::kMaxCallDepth)) {
            return fail("调用层数超过 " + std::to_string(Machine::kMaxCallDepth) +
                        "，脚本里可能有无限递归");
          }
          const Function& fn = program.functions[static_cast<std::size_t>(index)];

          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          positional.push_back(instance);  // self
          for (int i = 0; i < argc; ++i) {
            positional.push_back(stack[base + 1 + static_cast<std::size_t>(i)]);
          }

          std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
          const Value defaults =
              fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
                  ? globals[static_cast<std::size_t>(fn.defaultSlot)]
                  : Value();
          std::string bindError;
          if (!bindArguments(fn, defaults, positional, List{}, List{}, &locals,
                             &bindError)) {
            raiseException("TypeError", classNameOf(cls) + "() " + bindError);
            break;
          }

          sp = base;

          Frame callee;
          callee.code = &fn.code;
          callee.lines = &fn.lines;
          callee.function = &fn;
          callee.pc = 0;
          callee.stackBase = sp;
          callee.locals = std::move(locals);
          callee.initInstance = instance;
          callee.isInitFrame = true;
          frames.push_back(std::move(callee));
          break;
        }

        sp = base;
        stack[sp++] = instance;
        break;
      }

      case Op::kMakeSuper: {
        const std::uint16_t nameIndex = read16(code, frame.pc);
        frame.pc += 2;
        if (nameIndex >= program.consts.size()) return fail("类名越界（编译器 bug）");
        if (sp < 1) return fail("super() 缺少 self（编译器 bug）");
        const Value self = stack[--sp];
        // 当前类从全局槽按名字取：类对象是模块级绑定的。
        const std::string className = program.consts[nameIndex].asString();
        Value currentClass;
        for (int slot = 0; slot < program.globalCount; ++slot) {
          const std::string& name =
              program.globalNames[static_cast<std::size_t>(slot)];
          if (name == className) {
            currentClass = globals[static_cast<std::size_t>(slot)];
            break;
          }
        }
        stack[sp++] = makeSuper(self, currentClass);
        break;
      }

      case Op::kCallBuiltin: {
        const std::uint16_t index = read16(code, frame.pc);
        const int argc = code[frame.pc + 2];
        frame.pc += 3;
        if (index >= program.builtins.size()) return fail("内建号越界（编译器 bug）");
        if (argc > 8) return fail("内建最多 8 个参数");
        Value args[8];
        for (int i = argc - 1; i >= 0; --i) args[i] = stack[--sp];
        Value produced;
        std::string failure;
        if (!invokeBuiltin(program, globals, index, args, argc, KeywordArgs(),
                           &produced, &failure)) {
          if (!failure.empty()) return fail(failure);
          break;
        }
        stack[sp++] = produced;
        break;
      }

      case Op::kCallBuiltinKw: {
        // 栈上是 [位置实参..., 键, 值, 键, 值, ...]。
        const std::uint16_t index = read16(code, frame.pc);
        const int positional = code[frame.pc + 2];
        const int keywordCount = code[frame.pc + 3];
        frame.pc += 4;
        if (index >= program.builtins.size()) return fail("内建号越界（编译器 bug）");
        if (positional + keywordCount > 8) return fail("内建最多 8 个参数");

        const std::string& name = program.builtinNames[index];
        if (!builtinTakesKeywords(name)) {
          raiseException("TypeError", name + "() takes no keyword arguments");
          break;
        }

        // 栈顶是最后压的键值对，所以先把关键字全部弹出来（同时记住每个
        // 关键字要落在哪个位置），再弹位置实参。
        Value keywordValues[8];
        std::string keywordNames[8];
        for (int i = keywordCount - 1; i >= 0; --i) {
          keywordValues[i] = stack[--sp];
          keywordNames[i] = stack[--sp].asString();
        }

        Value args[8];
        for (int i = positional - 1; i >= 0; --i) args[i] = stack[--sp];
        int argc = positional;

        // 关键字只做合法性校验，不在这里增补位置——仅关键字参数
        // （sorted 的 key/reverse）由 invokeBuiltin 按名字取，不能占用
        // 位置槽，否则 argc 会虚增。
        Value sep;   // 只有 print 用；空 Value 表示「没给」
        Value end;
        for (int i = 0; i < keywordCount; ++i) {
          const std::string& key = keywordNames[i];
          if (name == "print") {
            if (key == "sep") {
              sep = keywordValues[i];
            } else if (key == "end") {
              end = keywordValues[i];
            } else {
              raiseException("TypeError", "print() got an unexpected keyword "
                                          "argument '" + key + "'");
              break;
            }
            continue;
          }
          if (builtinKeywordPosition(name, key) < 0 &&
              !builtinAcceptsKeyword(name, key)) {
            raiseException("TypeError", name + "() got an unexpected keyword "
                                            "argument '" + key + "'");
            break;
          }
        }
        if (hasPendingException()) break;

        Value produced;
        std::string failure;
        KeywordArgs keywordArgs;
        keywordArgs.names = keywordNames;
        keywordArgs.values = keywordValues;
        keywordArgs.count = keywordCount;
        if (!invokeBuiltin(program, globals, index, args, argc, keywordArgs,
                           &produced, &failure)) {
          if (!failure.empty()) return fail(failure);
          break;
        }
        stack[sp++] = produced;
        break;
      }

      case Op::kCallExt: {
        const std::uint16_t index = read16(code, frame.pc);
        const std::uint8_t argc = code[frame.pc + 2];
        frame.pc += 3;
        if (index >= program.extensions.size()) return fail("扩展号越界（编译器 bug）");
        if (program.extensions[index] == nullptr) {
          return fail("扩展函数 " + program.extensionNames[index] + " 没有链接进来");
        }
        Value args[8];
        if (argc > 8) return fail("扩展调用最多 8 个参数");
        for (int i = argc - 1; i >= 0; --i) args[i] = stack[--sp];
        stack[sp++] = program.extensions[index](args, argc);
        break;
      }

      case Op::kCallMethod: {
        const std::uint16_t nameIndex = read16(code, frame.pc);
        const std::uint8_t argc = code[frame.pc + 2];
        frame.pc += 3;
        if (nameIndex >= program.consts.size()) return fail("方法名越界（编译器 bug）");
        if (argc > 8) return fail("方法调用最多 8 个参数");
        Value args[8];
        for (int i = argc - 1; i >= 0; --i) args[i] = stack[--sp];
        const Value target = stack[--sp];
        const std::string name = program.consts[nameIndex].asString();

        // 脚本实例：沿 MRO 找成员。instanceAttrOf 已按描述符语义处理过：
        // 普通方法绑好了 self，staticmethod 是裸函数，classmethod 绑的是类。
        if (isInstance(target)) {
          std::string error;
          const Value bound = instanceAttrOf(target, name, &error);
          if (!error.empty()) {
            raiseException("AttributeError", error);
            break;
          }
          if (!isBoundMethod(bound) && !isFunction(bound)) {
            // 成员存在但不是函数：CPython 报 TypeError。
            raiseException("TypeError", "'" + typeName(bound) +
                                            "' object is not callable");
            break;
          }
          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          // 只有已绑定的方法才补 self；staticmethod 的裸函数直接调。
          if (isBoundMethod(bound)) positional.push_back(boundMethodSelf(bound));
          for (int i = 0; i < argc; ++i) positional.push_back(args[i]);
          std::string frameError;
          Value produced;
          if (!pushValueFrame(program, bound, std::move(positional), frames,
                              sp, &frameError, &produced)) {
            if (!frameError.empty()) {
              raiseException("TypeError", frameError);
            } else {
              stack[sp++] = produced;
            }
          }
          break;
        }

        // 类对象：`A.m(...)`、`A.cm(...)`、`A.sm(...)`。
        // 描述符语义一样要生效——classmethod 绑到 A 本身（这是它和
        // 定义它的类可能不同的地方），staticmethod 是裸函数。
        if (isClass(target)) {
          Value member;
          if (!classLookup(target, name, &member)) {
            raiseException("AttributeError", "type object '" +
                                                 classNameOf(target) +
                                                 "' has no attribute '" + name + "'");
            break;
          }
          const Value bound = descriptorValue(member, target, false);
          if (!isBoundMethod(bound) && !isFunction(bound)) {
            raiseException("TypeError", "'" + typeName(bound) +
                                            "' object is not callable");
            break;
          }
          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          if (isBoundMethod(bound)) positional.push_back(boundMethodSelf(bound));
          for (int i = 0; i < argc; ++i) positional.push_back(args[i]);
          std::string frameError;
          Value produced;
          if (!pushValueFrame(program, bound, std::move(positional), frames,
                              sp, &frameError, &produced)) {
            if (!frameError.empty()) {
              raiseException("TypeError", frameError);
            } else {
              stack[sp++] = produced;
            }
          }
          break;
        }

        // `super().m(...)`：从当前类的基类开始找，self 仍是原实例。
        if (isSuper(target)) {
          const Value self = superSelf(target);
          const Value current = superClassOf(target);
          Value member;
          if (!superLookupValue(self, current, name, &member)) {
            raiseException("AttributeError",
                           "'super' object has no attribute '" + name + "'");
            break;
          }
          // super() 在 classmethod 里时 self 是类而不是实例：此时按
          // 「经由类访问」绑定，classmethod 拿到的是这个类。
          const Value bound =
              descriptorValue(member, self, isInstance(self));
          if (!isBoundMethod(bound) && !isFunction(bound)) {
            raiseException("TypeError", "'" + typeName(bound) +
                                            "' object is not callable");
            break;
          }
          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          if (isBoundMethod(bound)) positional.push_back(boundMethodSelf(bound));
          for (int i = 0; i < argc; ++i) positional.push_back(args[i]);
          std::string frameError;
          Value produced;
          if (!pushValueFrame(program, bound, std::move(positional), frames,
                              sp, &frameError, &produced)) {
            if (!frameError.empty()) {
              raiseException("TypeError", frameError);
            } else {
              stack[sp++] = produced;
            }
          }
          break;
        }

        // 其它接收者：字符串/列表/字典/pylib 对象的方法表。
        std::string error;
        const Value value = callMethod(target, name, args, argc, &error);
        if (!error.empty()) return fail(error);
        stack[sp++] = value;
        break;
      }

      case Op::kCallMethodKw: {
        // 栈是 [接收者, 位置实参..., 键1, 值1, ...]。
        const std::uint16_t nameIndex = read16(code, frame.pc);
        const std::uint8_t positional = code[frame.pc + 2];
        const std::uint8_t keywordCount = code[frame.pc + 3];
        frame.pc += 4;
        if (nameIndex >= program.consts.size()) {
          return fail("方法名越界（编译器 bug）");
        }
        if (positional > 8 || keywordCount > 8) {
          return fail("方法调用最多 8 个参数");
        }
        Value keywordValues[8];
        std::string keywordNames[8];
        for (int i = keywordCount - 1; i >= 0; --i) {
          keywordValues[i] = stack[--sp];
          keywordNames[i] = stack[--sp].asString();
        }
        Value args[8];
        for (int i = positional - 1; i >= 0; --i) args[i] = stack[--sp];
        const Value target = stack[--sp];
        const std::string name = program.consts[nameIndex].asString();

        // 只有 list.sort 接受关键字。其它方法一律报错而不是静默忽略：
        // `x.append(v=1)` 被忽略会变成「追加了 None」，比报错难查得多。
        if (name != "sort" || !target.isList()) {
          std::string rejected;
          for (int i = 0; i < keywordCount; ++i) {
            if (!rejected.empty()) rejected += ", ";
            rejected += "'" + keywordNames[i] + "'";
          }
          raiseException("TypeError",
                         typeName(target) + "." + name +
                             "() takes no keyword arguments (" + rejected + ")");
          break;
        }

        Value key;
        bool reverse = false;
        for (int i = 0; i < keywordCount; ++i) {
          if (keywordNames[i] == "key") {
            key = keywordValues[i];
          } else if (keywordNames[i] == "reverse") {
            reverse = keywordValues[i].toBool();
          } else {
            raiseException("TypeError", "sort() got an unexpected keyword "
                                        "argument '" + keywordNames[i] + "'");
            break;
          }
        }
        if (hasPendingException()) break;

        // 键先一次性求好再排序：与 CPython 一样，key 带副作用时只调一次。
        if (!key.isNone()) {
          const List items = iterate(target);
          List keys;
          keys.reserve(items.size());
          for (const Value& item : items) {
            Value produced;
            std::string errorType;
            std::string error;
            if (!callCallable(program, globals, key, {item}, &produced,
                              &errorType, &error)) {
              raiseException(errorType.empty() ? "RuntimeError" : errorType,
                             error);
              break;
            }
            keys.push_back(produced);
          }
          if (hasPendingException()) break;
          sortListByKeys(target, Value::list(std::move(keys)), reverse);
        } else {
          sortListByKeys(target, Value(), reverse);
        }
        stack[sp++] = Value();
        break;
      }

      case Op::kReturn: {
        Value value = sp > frame.stackBase ? stack[--sp] : Value();
        // `A()` 的结果是实例，不是 __init__ 的返回值。
        if (frame.isInitFrame) value = frame.initInstance;
        sp = frame.stackBase;
        frames.pop_back();
        if (frames.empty()) {
          result.value = value;
          return result;
        }
        stack[sp++] = value;
        break;
      }
      case Op::kBuildList: {
        const std::uint16_t count = read16(code, frame.pc);
        frame.pc += 2;
        Value::List items;
        items.reserve(count);
        for (int i = count; i > 0; --i) items.push_back(stack[sp - static_cast<std::size_t>(i)]);
        sp -= count;
        stack[sp++] = Value::list(std::move(items));
        break;
      }

      case Op::kBuildTuple: {
        const std::uint16_t count = read16(code, frame.pc);
        frame.pc += 2;
        Value::List items;
        items.reserve(count);
        for (int i = count; i > 0; --i) items.push_back(stack[sp - static_cast<std::size_t>(i)]);
        sp -= count;
        stack[sp++] = Value::tuple(std::move(items));
        break;
      }

      case Op::kBuildSet: {
        const std::uint16_t count = read16(code, frame.pc);
        frame.pc += 2;
        Value::List items;
        for (int i = count; i > 0; --i) {
          const Value& item = stack[sp - static_cast<std::size_t>(i)];
          // 去重：集合字面量里重复的元素只算一个。
          bool seen = false;
          for (const Value& existing : items) {
            if (eq(existing, item).toBool()) {
              seen = true;
              break;
            }
          }
          if (!seen) items.push_back(item);
        }
        sp -= count;
        stack[sp++] = Value::set(std::move(items));
        break;
      }

      case Op::kBuildDict: {
        const std::uint16_t pairs = read16(code, frame.pc);
        frame.pc += 2;
        Value::List keys;
        Value::List values;
        keys.reserve(pairs);
        values.reserve(pairs);
        for (int i = pairs; i > 0; --i) {
          const std::size_t base = sp - static_cast<std::size_t>(i) * 2;
          keys.push_back(stack[base]);
          values.push_back(stack[base + 1]);
        }
        sp -= static_cast<std::size_t>(pairs) * 2;
        stack[sp++] = Value::dict(keys, values);
        break;
      }

      case Op::kGetItem: {
        if (sp < 2) return fail("下标运算缺少操作数（编译器 bug）");
        const Value index = stack[--sp];
        stack[sp - 1] = at(stack[sp - 1], index);
        break;
      }
      case Op::kSetItem: {
        if (sp < 3) return fail("下标赋值缺少操作数（编译器 bug）");
        const Value value = stack[--sp];
        const Value index = stack[--sp];
        const Value container = stack[--sp];
        setItem(container, index, value);
        break;
      }

      case Op::kSlice: {
        // 位标记：1=有 start，2=有 stop，4=有 step。压栈顺序是
        // [容器, start, stop, step]（缺的就不压）。
        const std::uint8_t flags = code[frame.pc++];
        Value step;
        Value stop;
        Value start;
        if (flags & 4) step = stack[--sp];
        if (flags & 2) stop = stack[--sp];
        if (flags & 1) start = stack[--sp];
        const Value container = stack[--sp];
        stack[sp++] = sliceStep(container, start, stop, step);
        break;
      }

      case Op::kSetSlice: {
        const std::uint8_t flags = code[frame.pc++];
        const Value items = stack[--sp];
        Value step;
        Value stop;
        Value start;
        if (flags & 4) step = stack[--sp];
        if (flags & 2) stop = stack[--sp];
        if (flags & 1) start = stack[--sp];
        const Value container = stack[--sp];
        assignSlice(container, start, stop, step, items);
        break;
      }

      case Op::kYield: {
        // 暂停这一帧：把 pc、局部量、以及本帧在值栈上的那一段摊平进
        // 生成器对象，然后把结果交给恢复者。
        if (!frame.isGeneratorFrame) return fail("yield 不在生成器里（编译器 bug）");
        Value value = sp > frame.stackBase ? stack[--sp] : Value();
        // 本帧的栈段：stackBase..sp 之间的就是它自己的中间值。
        List snapshot;
        snapshot.reserve(sp > frame.stackBase ? sp - frame.stackBase : 0);
        for (std::size_t i = frame.stackBase; i < sp; ++i) {
          snapshot.push_back(stack[i]);
        }
        setGeneratorState(frame.generatorObject, static_cast<int>(frame.pc),
                          frame.locals, std::move(snapshot), true, false);
        sp = frame.stackBase;
        frames.pop_back();
        result.yielded = true;
        result.value = value;
        return result;
      }

      case Op::kAwait: {
        // `await 协程`：驱动它跑到底，把 return 的值压回栈上。
        // 协程体里不许有 yield，所以正常情况下一次就能跑完；万一真的有
        // yield，这里继续推进直到结束——脚本观察不到差别，也不会卡住。
        const Value coroutine = stack[--sp];
        if (!isGenerator(coroutine)) {
          raiseException("TypeError", "object " + typeName(coroutine) +
                                          " can't be used in 'await' expression");
          break;
        }
        Value produced;
        Value finalValue;
        std::string errorType;
        std::string error;
        bool failed = false;
        while (advanceGenerator(program, globals, coroutine, &produced,
                                &errorType, &error, &failed, &finalValue)) {
        }
        if (failed) {
          raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
          break;
        }
        stack[sp++] = finalValue;
        break;
      }

      case Op::kMakeStaticMethod: {
        // 栈顶是刚造出的函数值，包一层标记。包装值直接进类成员表，
        // 访问时由 descriptorValue 决定不绑定接收者。
        if (sp < 1) return fail("staticmethod 缺少函数值（编译器 bug）");
        stack[sp - 1] = makeStaticMethod(stack[sp - 1]);
        break;
      }

      case Op::kMakeClassMethod: {
        if (sp < 1) return fail("classmethod 缺少函数值（编译器 bug）");
        stack[sp - 1] = makeClassMethod(stack[sp - 1]);
        break;
      }

      case Op::kLoadType: {
        // u16 类型名常量号。压一个类值，供 `type(x) is int` 这类比较使用。
        const std::uint16_t nameIndex = read16(code, frame.pc);
        frame.pc += 2;
        if (nameIndex >= program.consts.size()) return fail("类型名越界（编译器 bug）");
        stack[sp++] = makeBuiltinClass(program.consts[nameIndex].asString());
        break;
      }

      case Op::kMakeBuiltinFunction: {
        // u16 内建号。压一个可调用值（`key=len`、`f = len`）。
        // 这个号是**本程序**内建表的下标（只收脚本用到的那些），不是全局
        // 表下标——名字必须从 program.builtinNames 取，否则 `print(len)`
        // 会显示成别的位置上的内建。
        const std::uint16_t builtinIndex = read16(code, frame.pc);
        frame.pc += 2;
        if (builtinIndex >= program.builtins.size()) {
          return fail("内建号越界（编译器 bug）");
        }
        stack[sp++] = makeBuiltinFunction(builtinIndex,
                                          program.builtinNames[builtinIndex]);
        break;
      }

      case Op::kUnpack: {
        // u16 元素个数。弹一个序列，把前 N 个元素压回栈（第一个在最上，
        // 这样紧跟其后的 STORE 会依次存倒数第一个名字…… 见编译器侧的
        // 反向发射顺序）。
        const std::uint16_t wanted = read16(code, frame.pc);
        frame.pc += 2;
        if (sp < 1) return fail("元组解包缺少值（编译器 bug）");
        const Value source = stack[--sp];
        const Value::List items = iterate(source);
        if (items.size() != wanted) {
          // CPython 区分「不够」和「太多」两种消息。
          const std::string expected = std::to_string(wanted);
          const std::string got = std::to_string(items.size());
          if (items.size() < wanted) {
            raiseException("ValueError", "not enough values to unpack (expected " +
                                             expected + ", got " + got + ")");
          } else {
            raiseException("ValueError", "too many values to unpack (expected " +
                                             expected + ")");
          }
          break;
        }
        // 顺序很关键：随后的 STORE 是从**栈顶**逐个取值的，而编译器按
        // 「从后往前」分配目标（`a, b = x` 先存 b 再存 a）。所以第一个
        // 元素必须最后压（在栈顶），最后一个元素先压。
        for (const Value& item : items) {
          if (sp >= kStackSize) return fail("操作数栈溢出");
          stack[sp++] = item;
        }
        break;
      }

      case Op::kIterInit: {
        const Value container = stack[--sp];
        // 生成器不能物化：它的全部意义就是惰性。原样留在栈上，由
        // ITER_NEXT 一次推进一格。
        if (isGenerator(container)) {
          stack[sp++] = container;
          stack[sp++] = Value(0LL);
          break;
        }
        // 把容器物化成一个列表再按下标走。
        //
        // 为什么物化而不是持有迭代器：脚本里常见「遍历时改原列表」，
        // 也常见「遍历刚生成的 range」。物化一次（长度级别的一次拷贝）
        // 换来「循环变量与外层容器互不影响」，与 CPython 的
        // `for x in list` 行为一致；而 range 本来就该物化。
        Value::List items = iterate(container);
        stack[sp++] = Value::list(std::move(items));
        stack[sp++] = Value(0LL);
        break;
      }
      case Op::kIterNext: {
        const std::size_t exitTarget = read16(code, frame.pc);
        frame.pc += 2;
        if (sp < 2) return fail("for 循环状态缺失（编译器 bug）");

        // 生成器：栈上是 [生成器, 占位]。每次恢复一小段，直到 yield 或结束。
        if (isGenerator(stack[sp - 2])) {
          const Value generator = stack[sp - 2];
          sp -= 2;
          Value produced;
          std::string errorType;
          std::string error;
          bool failed = false;
          const bool hasValue = advanceGenerator(program, globals, generator,
                                                &produced, &errorType, &error,
                                                &failed);
          if (failed) {
            // 生成器体里的异常要出现在**调用方**的栈上，走同一条异常通道。
            raiseException(errorType.empty() ? "RuntimeError" : errorType, error);
            break;
          }
          if (!hasValue) {
            frame.pc = exitTarget;
            break;
          }
          // 把状态重新摆回 for 循环约定的 [list, index]，index 恒为 0：
          // 生成器自己记着「跑到哪了」，这里只是让同一条 ITER_NEXT 能继续用。
          stack[sp++] = generator;
          stack[sp++] = Value(0LL);
          stack[sp++] = produced;
          break;
        }

        // 栈布局：[list, index]
        const long long index = stack[sp - 1].toInt();
        const Value::List& items = stack[sp - 2].asList();
        if (index >= static_cast<long long>(items.size())) {
          sp -= 2;
          frame.pc = exitTarget;
          break;
        }
        stack[sp - 1] = Value(index + 1);
        stack[sp++] = items[static_cast<std::size_t>(index)];
        break;
      }
      case Op::kIterBreak:
        if (sp < 2) return fail("for 循环状态缺失（编译器 bug）");
        sp -= 2;
        break;

      case Op::kInplace: {
        // x[i] op= v。编译器保证栈上是 [容器, 下标, 右值]。
        const auto operand = static_cast<BinOp>(code[frame.pc++]);
        const Value value = stack[--sp];
        const Value index = stack[--sp];
        const Value container = stack[--sp];
        setItem(container, index, applyBin(operand, at(container, index), value));
        break;
      }

      case Op::kWithEnter: {
        // 弹管理器 → 存进局部槽 → 调 __enter__() → 压返回值 → 压 with 块。
        //
        // 存槽而不是留在栈上：`with` 体内会压任意深度的临时值，异常展开时
        // 栈顶更不可能是管理器。
        //
        // 块在 `__enter__` 之后才压：进入失败时不该由这个 with 去调
        // `__exit__`（CPython 也一样，进入失败就不退出）。
        const std::uint16_t slot = read16(code, frame.pc);
        const std::uint16_t exitTarget = read16(code, frame.pc + 2);
        frame.pc += 4;
        if (slot >= frame.locals.size()) return fail("with 临时槽越界（编译器 bug）");
        const Value manager = stack[--sp];
        frame.locals[slot] = manager;
        Value entered;
        bool found = false;
        std::string error;
        if (!callDunder(program, globals, manager, "__enter__", {}, &entered,
                        &found, &error) ||
            !found) {
          // CPython 对不支持上下文协议的对象抛 TypeError，不是 AttributeError。
          raiseException("TypeError", "'" + typeName(manager) +
                                         "' object does not support the context "
                                         "manager protocol");
          break;
        }
        if (!error.empty()) {
          raiseException("RuntimeError", error);
          break;
        }
        stack[sp++] = entered;

        Block block;
        block.kind = Block::kWith;
        block.withSlot = slot;
        block.target = exitTarget;
        block.stackLevel = sp;
        ExceptionState& state = exceptionStateOf(frame);
        block.exceptionDepth = state.exceptions.size();
        state.blocks.push_back(block);
        break;
      }

      case Op::kWithExit: {
        // 正常退出 with：弹块，调 __exit__()。
        //
        // `__exit__` 的返回值在这条路径上没有语义：CPython 只在异常穿出时
        // 看它是否吞掉异常。
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("with 临时槽越界（编译器 bug）");
        if (const ExceptionState* state = peekExceptionStateOf(frame)) {
          if (!state->blocks.empty() &&
              state->blocks.back().kind == Block::kWith) {
            exceptionStateOf(frame).blocks.pop_back();
          }
        }
        const Value manager = frame.locals[slot];
        // 清空槽：让管理器能被回收，也让同一槽被下一个 with 复用时安全。
        frame.locals[slot] = Value();
        Value ignored;
        bool found = false;
        std::string error;
        if (!callDunder(program, globals, manager, "__exit__",
                        {Value(), Value(), Value()}, &ignored, &found, &error) ||
            !found) {
          raiseException("TypeError", "'" + typeName(manager) +
                                         "' object does not support the context "
                                         "manager protocol");
          break;
        }
        if (!error.empty()) {
          raiseException("RuntimeError", error);
          break;
        }
        break;
      }

      // ── 异常 ──

      case Op::kSetupExc: {
        // 压一层保护区。记录值栈与异常栈的当前深度，展开时退回到这里。
        const std::uint16_t target = read16(code, frame.pc);
        frame.pc += 2;
        Block block;
        block.kind = Block::kTry;
        block.target = target;
        block.stackLevel = sp;
        ExceptionState& state = exceptionStateOf(frame);
        block.exceptionDepth = state.exceptions.size();
        state.blocks.push_back(block);
        break;
      }

      case Op::kPopExc: {
        if (const ExceptionState* state = peekExceptionStateOf(frame)) {
          if (!state->blocks.empty()) exceptionStateOf(frame).blocks.pop_back();
        }
        break;
      }

      case Op::kRaise: {
        // `raise X`：X 已经求值并压栈。不是异常对象时报 TypeError，
        // 与 CPython 的「只能抛出 BaseException 的实例」一致。
        if (sp == 0) return fail("raise 缺少异常值（编译器 bug）");
        const Value value = stack[--sp];
        if (!isException(value)) {
          if (!unwind(makeException("TypeError",
                                    "exceptions must derive from BaseException"))) {
            return result;
          }
          break;
        }
        if (!unwind(value)) return result;
        break;
      }

      case Op::kRaiseType: {
        // u16 类型名常量号, u8 有没有消息（有则弹栈顶当消息）。
        // 编译器只对「内建异常类」发这一条，所以类型名在编译期已校验。
        const std::uint16_t nameIndex = read16(code, frame.pc);
        const std::uint8_t hasMessage = code[frame.pc + 2];
        frame.pc += 3;
        if (nameIndex >= program.consts.size()) {
          return fail("异常类型名越界（编译器 bug）");
        }
        const std::string type = program.consts[nameIndex].toString();
        std::string message;
        if (hasMessage != 0) message = stack[--sp].toString();
        if (!unwind(makeException(type, message))) return result;
        break;
      }

      case Op::kReraise: {
        const ExceptionState* state = peekExceptionStateOf(frame);
        if (state == nullptr || state->exceptions.empty()) {
          if (!unwind(makeException("RuntimeError",
                                    "No active exception to re-raise"))) {
            return result;
          }
          break;
        }
        if (!unwind(state->exceptions.back())) return result;
        break;
      }

      case Op::kExcMatch: {
        // u16 命中时的目标, u16 期望类型名的常量号。
        //
        // 语义是「匹配则跳转，不匹配则继续下一条」——这样 `except (A, B):`
        // 直接排成两条 EXC_MATCH，最后一条不匹配就自然落到下一个 handler。
        // 反过来写（不匹配才跳）每条后面都要补一条 JUMP。
        const std::uint16_t hitTarget = read16(code, frame.pc);
        const std::uint16_t nameIndex = read16(code, frame.pc + 2);
        frame.pc += 4;
        if (nameIndex >= program.consts.size()) {
          return fail("异常类型名越界（编译器 bug）");
        }
        const std::string wanted = program.consts[nameIndex].toString();
        const ExceptionState* state = peekExceptionStateOf(frame);
        if (state != nullptr && !state->exceptions.empty() &&
            exceptionMatches(exceptionType(state->exceptions.back()), wanted)) {
          frame.pc = hitTarget;
        }
        break;
      }

      case Op::kCurrentExc: {
        // `except ... as e`：把正在处理的异常压栈，交给随后的 nameStore。
        const ExceptionState* state = peekExceptionStateOf(frame);
        if (state != nullptr && !state->exceptions.empty()) {
          stack[sp++] = state->exceptions.back();
        } else {
          stack[sp++] = Value();
        }
        break;
      }

      case Op::kClearExc: {
        // 离开 except 体：弹掉这一层「当前异常」，让外层的 bare raise 看到
        // 外层的异常（CPython 在 except 结束时同样清掉 sys.exc_info）。
        const ExceptionState* state = peekExceptionStateOf(frame);
        if (state != nullptr && !state->exceptions.empty()) {
          exceptionStateOf(frame).exceptions.pop_back();
        }
        break;
      }
    }

    // 值栈边界的统一闸门：各分支自己有弹压，但漏一处就是越界写，而越界
    // 写会破坏 std::vector 之后的内存（Release 下表现为 __fastfail，即
    // 0xC0000409，Debug 下反而看不出来）。查一次比逐条加便宜得多。
    if (sp > kStackSize) return fail("操作数栈溢出（编译器 bug）");
  }

  result.value = sp > 0 ? stack[sp - 1] : Value();
  return result;
}

// 推进生成器一格。
//
// 首次推进时把已绑好的局部量装进帧；之后每次用存下的 pc 与栈段续跑。
// 这样一次 execute 只跑到下一个 YIELD 为止，惰性与副作用顺序都与 CPython
// 一致（`g = gen()` 不执行函数体，第一次 `next` 才跑到第一个 yield）。
static bool advanceGenerator(const Program& program, std::vector<Value>& globals,
                             const Value& generator, Value* out,
                             std::string* errorType, std::string* error,
                             bool* failed, Value* finalValue) {
  *failed = false;
  errorType->clear();
  error->clear();
  if (!isGenerator(generator)) {
    *failed = true;
    *errorType = "TypeError";
    *error = "'" + typeName(generator) + "' object is not a generator";
    return false;
  }
  if (generatorDone(generator)) return false;

  const int index = generatorFunctionIndex(generator);
  if (index < 0 || index >= static_cast<int>(program.functions.size())) {
    *failed = true;
    *errorType = "RuntimeError";
    *error = "generator function index out of range";
    return false;
  }
  const Function& fn = program.functions[static_cast<std::size_t>(index)];

  Frame frame;
  frame.code = &fn.code;
  frame.lines = &fn.lines;
  frame.function = &fn;
  frame.pc = static_cast<std::size_t>(generatorPcOf(generator));
  frame.stackBase = 0;
  frame.locals = generatorLocals(generator);
  if (frame.locals.size() < static_cast<std::size_t>(fn.localCount)) {
    frame.locals.resize(static_cast<std::size_t>(fn.localCount), Value());
  }
  frame.isGeneratorFrame = true;
  frame.generatorObject = generator;

  const bool started = generatorStarted(generator);
  List seed = generatorStack(generator);
  // `next()` 给 `yield` 表达式送的是 None（本语言不支持 send）。恢复时把
  // 它补在栈顶，否则后续 POP 会对着空栈弹，sp 下溢成 SIZE_MAX——
  // 症状是「操作数栈溢出（编译器 bug）」出现在第二次迭代。
  if (started) seed.push_back(Value());

  std::vector<Frame> frames;
  frames.push_back(std::move(frame));

  Result result = execute(program, globals, frames, started ? &seed : nullptr);
  if (result.failed) {
    *failed = true;
    *errorType = resultExceptionType(result);
    *error = result.error;
    if (errorType->empty()) *errorType = "RuntimeError";
    // 抛异常后生成器就结束了（CPython 同样不会让它再继续）。
    setGeneratorState(generator, 0, List{}, List{}, true, true);
    return false;
  }
  if (result.yielded) {
    *out = result.value;
    return true;
  }
  // 跑到末尾（`return` 或函数体结束）：协程要把返回值交出去，
  // 生成器则只是结束。
  if (finalValue != nullptr) *finalValue = result.value;
  setGeneratorState(generator, 0, List{}, List{}, true, true);
  return false;
}

// 同步调用一个函数值（`map`/`filter` 的回调、`sorted(key=)` 都要它）。
//
// 与 kCallValue 的差别：那条是「建帧后继续跑同一个执行循环」，这里必须
// 跑完拿返回值——内建需要一个具体的结果，不能挂起。所以独立建帧 + execute。
static bool callCallable(const Program& program, std::vector<Value>& globals,
                         const Value& callable,
                         const std::vector<Value>& args, Value* out,
                         std::string* errorType, std::string* error) {
  errorType->clear();
  error->clear();

  // 内建函数值（`key=len`）：走内置表，不能当脚本函数号用。
  if (isBuiltinFunction(callable)) {
    const int builtin = builtinFunctionIndex(callable);
    if (builtin < 0 || builtin >= static_cast<int>(program.builtins.size())) {
      *errorType = "RuntimeError";
      *error = "内建号越界（编译器 bug）";
      return false;
    }
    if (args.size() > 8) {
      *errorType = "TypeError";
      *error = "内建最多 8 个参数";
      return false;
    }
    Value buffer[9];
    for (std::size_t i = 0; i < args.size(); ++i) buffer[i] = args[i];
    std::string failure;
    Value produced;
    if (!invokeBuiltin(program, globals, static_cast<std::uint16_t>(builtin),
                       buffer, static_cast<int>(args.size()), KeywordArgs(),
                       &produced, &failure)) {
      *errorType = failure.empty() ? std::string("RuntimeError")
                                   : std::string("TypeError");
      *error = failure;
      return false;
    }
    *out = produced;
    return true;
  }

  // 异常类（`ValueError("x")`、`MyError()`）：CPython 里异常类是可调用
  // 对象，调用就构造一个异常实例。`raise` 语句走 RAISE_TYPE 不经过这里，
  // 但 `e = ValueError("x")`、`f(ValueError("x"))` 必须有值可传。
  if (isBuiltinClass(callable) &&
      isExceptionClassName(builtinClassName(callable))) {
    if (args.size() > 1) {
      *errorType = "TypeError";
      *error = builtinClassName(callable) + " expected at most 1 argument, got " +
               std::to_string(args.size());
      return false;
    }
    *out = makeException(builtinClassName(callable),
                         args.empty() ? std::string() : args[0].toString());
    return true;
  }

  // 内建类型名（`str`、`list`、`int`…）是可调用的构造器，同时又能当类型用。
  // CPython 里它们本来就是一个对象：`str is type("")`。这里按名字找回同名
  // 内建并转发，`map(str, xs)` 因此与 `str(x)` 走同一条实现。
  if (isBuiltinClass(callable)) {
    const std::string className = builtinClassName(callable);
    // program.builtins 只收脚本引用过的内建，`str` 可能不在里面（脚本只把
    // 它当类型用过），所以按名字在脚本表里找，找不到就直接用全局函数指针。
    std::uint16_t local = 0;
    bool found = false;
    for (std::size_t i = 0; i < program.builtinNames.size(); ++i) {
      if (program.builtinNames[i] == className) {
        local = static_cast<std::uint16_t>(i);
        found = true;
        break;
      }
    }
    if (!found && builtinIndexFromName(className) < 0) {
      *errorType = "TypeError";
      *error = "'" + className + "' object is not callable";
      return false;
    }
    if (args.size() > 8) {
      *errorType = "TypeError";
      *error = "内建最多 8 个参数";
      return false;
    }
    Value buffer[9];
    for (std::size_t i = 0; i < args.size(); ++i) buffer[i] = args[i];
    Value produced;
    if (found) {
      std::string failure;
      if (!invokeBuiltin(program, globals, local, buffer,
                         static_cast<int>(args.size()), KeywordArgs(),
                         &produced, &failure)) {
        *errorType = "TypeError";
        *error = failure;
        return false;
      }
    } else {
      std::string failure;
      produced = builtinAt(builtinIndexFromName(className))(
          buffer, static_cast<int>(args.size()), &failure);
      if (!failure.empty()) {
        *errorType = "TypeError";
        *error = failure;
        return false;
      }
    }
    *out = produced;
    return true;
  }

  const int index = functionIndexOf(callable);
  if (index < 0) {
    *errorType = "TypeError";
    *error = "'" + typeName(callable) + "' object is not callable";
    return false;
  }
  if (index >= static_cast<int>(program.functions.size())) {
    *errorType = "RuntimeError";
    *error = "函数号越界（编译器 bug）";
    return false;
  }
  const Function& fn = program.functions[static_cast<std::size_t>(index)];

  std::vector<Value> positional;
  positional.reserve(args.size() + 1);
  if (isBoundMethod(callable)) positional.push_back(boundMethodSelf(callable));
  for (const Value& arg : args) positional.push_back(arg);

  std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
  const Value defaults =
      fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
          ? globals[static_cast<std::size_t>(fn.defaultSlot)]
          : Value();
  std::string bindError;
  if (!bindArguments(fn, defaults, positional, List{}, List{}, &locals,
                     &bindError)) {
    *errorType = "TypeError";
    *error = fn.name + "() " + bindError;
    return false;
  }
  const List& captures = functionCaptures(callable);
  if (!captures.empty() && fn.captureStart >= 0) {
    for (std::size_t i = 0; i < captures.size(); ++i) {
      const std::size_t slot = static_cast<std::size_t>(fn.captureStart) + i;
      if (slot < locals.size()) locals[slot] = captures[i];
    }
  }
  if (fn.isGenerator || fn.isCoroutine) {
    *errorType = "TypeError";
    *error = "the callback must be a plain function, not a generator";
    return false;
  }

  std::vector<Frame> frames;
  frames.reserve(4);
  pushFrame(fn, std::move(locals), frames, 0);
  const Result result = execute(program, globals, frames);
  if (result.failed) {
    *errorType = resultExceptionType(result);
    if (errorType->empty()) *errorType = "RuntimeError";
    *error = result.error;
    return false;
  }
  *out = result.value;
  return true;
}

// 协议方法调用（`__enter__` / `__exit__`）。
//
// 两条来源要分开处理：
//   * 脚本实例：沿 MRO 找函数值，建帧跑一遍 execute——脚本方法必须真的执行；
//   * pylib 对象（文件、锁）：走方法表转发，VM 不认识它是什么。
//
// 之前 with 只走 callMethod，于是脚本类写的 `__enter__` 完全不会被调用，
// 症状是 `with Guard():` 报「object 不支持上下文管理器协议」，而同一个类
// 在 CPython 里明明可以。
bool callDunder(const Program& program, std::vector<Value>& globals,
                const Value& target, const std::string& name,
                std::vector<Value> args, Value* out, bool* found,
                std::string* error) {
  *found = false;
  if (out != nullptr) *out = Value();
  error->clear();

  if (isInstance(target)) {
    Value member;
    if (!classLookup(instanceClass(target), name, &member)) {
      // 实例字段里也可能放着一个可调用值。
      std::string fieldError;
      member = instanceAttrOf(target, name, &fieldError);
      if (!fieldError.empty()) return true;
    }
    const Value bound = descriptorValue(member, target, true);
    if (!isBoundMethod(bound) && !isFunction(bound)) return true;

    std::vector<Value> positional;
    positional.reserve(args.size() + 1);
    if (isBoundMethod(bound)) positional.push_back(boundMethodSelf(bound));
    for (Value& arg : args) positional.push_back(std::move(arg));

    const int index = functionIndexOf(bound);
    if (index < 0 || index >= static_cast<int>(program.functions.size())) {
      *error = "'" + typeName(bound) + "' object is not callable";
      return false;
    }
    const Function& fn = program.functions[static_cast<std::size_t>(index)];
    std::vector<Value> locals(static_cast<std::size_t>(fn.localCount), Value());
    const Value defaults =
        fn.defaultSlot >= 0 && fn.defaultSlot < static_cast<int>(globals.size())
            ? globals[static_cast<std::size_t>(fn.defaultSlot)]
            : Value();
    std::string bindError;
    if (!bindArguments(fn, defaults, positional, List{}, List{}, &locals,
                       &bindError)) {
      *error = fn.name + "() " + bindError;
      return false;
    }
    const List& captures = functionCaptures(bound);
    if (!captures.empty() && fn.captureStart >= 0) {
      for (std::size_t i = 0; i < captures.size(); ++i) {
        const std::size_t slot = static_cast<std::size_t>(fn.captureStart) + i;
        if (slot < locals.size()) locals[slot] = captures[i];
      }
    }
    if (fn.isGenerator || fn.isCoroutine) {
      // 协议方法写成生成器没有意义，CPython 也只是把它当普通函数调。
      *error = fn.name + "() is a generator; context manager protocol "
                         "methods must return normally";
      return false;
    }

    std::vector<Frame> frames;
    frames.reserve(4);
    pushFrame(fn, std::move(locals), frames, 0);
    const Result result = execute(program, globals, frames);
    if (result.failed) {
      const std::string type = resultExceptionType(result);
      *error = (type.empty() ? std::string("RuntimeError") : type) + ": " +
               result.error;
      return false;
    }
    *found = true;
    if (out != nullptr) *out = result.value;
    return true;
  }

  std::string methodError;
  const Value value = callMethod(target, name, args.empty() ? nullptr : args.data(),
                                 static_cast<int>(args.size()), &methodError);
  if (!methodError.empty()) return true;  // 没有这个方法 → found = false
  *found = true;
  if (out != nullptr) *out = value;
  return true;
}

}  // namespace vm
}  // namespace aipy
