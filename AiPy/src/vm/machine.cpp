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

#include <cstdio>
#include <cstdlib>
#include <string>

namespace aipy {
namespace vm {

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
                                          const Value& exception) {
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
      // __exit__ 自己出错时不该覆盖原始异常：CPython 把它挂成 during handling，
      // 这里只保留第一个——原始原因更有用。
      if (block.withSlot < frame.locals.size()) {
        const Value manager = frame.locals[block.withSlot];
        if (!manager.isNone()) {
          std::string error;
          callMethod(manager, "__exit__", nullptr, 0, &error);
          clearPendingException();
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
using detail::instanceAttrOf;
using detail::instanceSetAttr;
using detail::superLookupRaw;
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
                      std::vector<Frame>& frames);

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
static bool pushValueFrame(const Program& program, const Value& callable,
                           std::vector<Value> positional,
                           std::vector<Frame>& frames, std::size_t stackBase,
                           std::string* error) {
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
                      std::vector<Frame>& frames) {
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
    return detail::unwindException(frames, stack, &sp, &result, exception);
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
        // 类对象：`A.x` 直接取类成员（不绑定）。
        if (isClass(target)) {
          const Value value = classLookupRaw(target, name);
          if (value.isNone()) {
            // 免不了再确认一次：成员本身可以是 None。
            const std::string clsName = classNameOf(target);
            bool found = false;
            for (const Value& memberValue : classMemberValues(target)) (void)memberValue;
            const List& keys = classMemberKeys(target);
            for (const Value& key : keys) {
              if (key.asString() == name) {
                found = true;
                break;
              }
            }
            if (!found) {
              raiseException("AttributeError",
                             "type object '" + clsName +
                                 "' has no attribute '" + name + "'");
              break;
            }
          }
          stack[sp++] = value;
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
        const std::uint8_t argc = code[frame.pc + 2];
        frame.pc += 3;
        if (index >= program.builtins.size()) return fail("内建号越界（编译器 bug）");
        Value args[8];
        if (argc > 8) return fail("内建最多 8 个参数");
        for (int i = argc - 1; i >= 0; --i) args[i] = stack[--sp];
        std::string error;
        const Value value = program.builtins[index](args, argc, &error);
        if (!error.empty()) return fail(error);
        stack[sp++] = value;
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

        // 脚本实例：沿 MRO 找成员。找到函数值就建帧，与 CALL_VALUE 同构
        // 但先把 self 补成第一个实参。编译期不知道接收者类型，所以这条
        // 指令要同时服务「字符串方法」与「脚本方法」。
        if (isInstance(target)) {
          std::string error;
          const Value bound = instanceAttrOf(target, name, &error);
          if (!error.empty()) {
            raiseException("AttributeError", error);
            break;
          }
          if (!isBoundMethod(bound)) {
            // 成员存在但不是函数：CPython 报 TypeError。
            raiseException("TypeError", "'" + typeName(bound) +
                                            "' object is not callable");
            break;
          }
          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          positional.push_back(boundMethodSelf(bound));
          for (int i = 0; i < argc; ++i) positional.push_back(args[i]);
          std::string frameError;
          if (!pushValueFrame(program, bound, std::move(positional), frames,
                              sp, &frameError)) {
            raiseException("TypeError", frameError);
          }
          break;
        }

        // `super().m(...)`：从当前类的基类开始找，self 仍是原实例。
        if (isSuper(target)) {
          const Value self = superSelf(target);
          const Value current = superClassOf(target);
          const Value member = superLookupRaw(self, current, name);
          if (!isFunction(member)) {
            raiseException("AttributeError",
                           "'super' object has no attribute '" + name + "'");
            break;
          }
          std::vector<Value> positional;
          positional.reserve(static_cast<std::size_t>(argc) + 1);
          positional.push_back(self);
          for (int i = 0; i < argc; ++i) positional.push_back(args[i]);
          std::string frameError;
          if (!pushValueFrame(program, member, std::move(positional), frames,
                              sp, &frameError)) {
            raiseException("TypeError", frameError);
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

      case Op::kIterInit: {
        // 把容器物化成一个列表再按下标走。
        //
        // 为什么物化而不是持有迭代器：脚本里常见「遍历时改原列表」，
        // 也常见「遍历刚生成的 range」。物化一次（长度级别的一次拷贝）
        // 换来「循环变量与外层容器互不影响」，与 CPython 的
        // `for x in list` 行为一致；而 range 本来就该物化。
        const Value container = stack[--sp];
        Value::List items = iterate(container);
        stack[sp++] = Value::list(std::move(items));
        stack[sp++] = Value(0LL);
        break;
      }
      case Op::kIterNext: {
        const std::size_t exitTarget = read16(code, frame.pc);
        frame.pc += 2;
        if (sp < 2) return fail("for 循环状态缺失（编译器 bug）");
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
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("with 临时槽越界（编译器 bug）");
        const Value manager = stack[--sp];
        frame.locals[slot] = manager;
        std::string error;
        const Value entered = callMethod(manager, "__enter__", nullptr, 0, &error);
        if (!error.empty()) {
          // CPython 对不支持上下文协议的对象抛 TypeError，不是 AttributeError。
          raiseException("TypeError", "'" + typeName(manager) +
                                         "' object does not support the context "
                                         "manager protocol");
          break;
        }
        stack[sp++] = entered;

        Block block;
        block.kind = Block::kWith;
        block.withSlot = slot;
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
        std::string error;
        callMethod(manager, "__exit__", nullptr, 0, &error);
        if (!error.empty()) {
          raiseException("TypeError", "'" + typeName(manager) +
                                         "' object does not support the context "
                                         "manager protocol");
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

}  // namespace vm
}  // namespace aipy
