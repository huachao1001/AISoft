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

#include <string>

namespace aipy {
namespace vm {

namespace detail {

namespace {

// 当前线程正在跑的程序。thread_local 而不是全局：两个线程各跑不同程序时
// 互不影响，也不会在对方拆 Program 时读到悬空指针。
thread_local const Program* g_currentProgram = nullptr;

}  // namespace

const Program* currentProgramPointer() { return g_currentProgram; }
void setCurrentProgramPointer(const Program* program) { g_currentProgram = program; }

}  // namespace detail

// 公开入口：扩展函数用它在 VM 调用栈内拿到 Program。
const Program* currentProgram() { return detail::currentProgramPointer(); }

using detail::applyBin;
using detail::applyCmp;
using detail::applyUnary;
using detail::callMethod;
using detail::Frame;
using detail::read16;

// 前置声明：定义在 `Machine::callFunction` 之后，两个入口共用它。
static Result execute(const Program& program, std::vector<Value>& globals,
                      std::vector<Frame> frames);

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
  return execute(program, globals_, std::move(frames));
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
  return execute(program, globals_, std::move(frames));
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
                      std::vector<Frame> frames) {
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
        stack[sp++] = globals[slot];
        break;
      }
      case Op::kStoreGlobal: {
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= globals.size()) return fail("全局槽越界（编译器 bug）");
        globals[slot] = stack[--sp];
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
        if (argc != fn.paramCount) {
          return fail("函数 " + fn.name + " 需要 " +
                      std::to_string(fn.paramCount) + " 个参数，实际给了 " +
                      std::to_string(argc) + " 个");
        }

        Frame callee;
        callee.code = &fn.code;
        callee.lines = &fn.lines;
        callee.function = &fn;
        callee.pc = 0;
        callee.stackBase = sp - argc;  // 参数已在栈上
        callee.locals.assign(static_cast<size_t>(fn.localCount), Value());
        for (int i = 0; i < argc; ++i) {
          callee.locals[static_cast<size_t>(i)] = stack[callee.stackBase + static_cast<size_t>(i)];
        }
        sp = callee.stackBase;  // 参数移进 locals 后从共享栈弹出
        frames.push_back(std::move(callee));
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
        const std::string name = program.consts[nameIndex].toString();
        std::string error;
        const Value value = callMethod(target, name, args, argc, &error);
        if (!error.empty()) return fail(error);
        stack[sp++] = value;
        break;
      }

      case Op::kReturn: {
        const Value value = sp > frame.stackBase ? stack[--sp] : Value();
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
        // 弹管理器 → 存进局部槽 → 调 __enter__() → 压返回值。
        //
        // 存槽而不是留在栈上：`with` 体内会压任意深度的临时值，退出时栈顶
        // 不一定是管理器。
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("with 临时槽越界（编译器 bug）");
        const Value manager = stack[--sp];
        frame.locals[slot] = manager;
        std::string error;
        const Value entered = callMethod(manager, "__enter__", nullptr, 0, &error);
        if (!error.empty()) return fail(error);
        stack[sp++] = entered;
        break;
      }

      case Op::kWithExit: {
        // 从槽里取回管理器，调 __exit__()。
        //
        // 不压任何东西：`__exit__` 的返回值在这里没有语义（AiPy 没有异常
        // 通道，不存在「__exit__ 吞掉异常」那种情形）。早先这里多写了一次
        // `--sp`，把 with 体留下的无关值弹掉了，栈就此错位。
        const std::uint16_t slot = read16(code, frame.pc);
        frame.pc += 2;
        if (slot >= frame.locals.size()) return fail("with 临时槽越界（编译器 bug）");
        const Value manager = frame.locals[slot];
        // 清空槽：让管理器能被回收，也让同一槽被下一个 with 复用时安全。
        frame.locals[slot] = Value();
        std::string error;
        callMethod(manager, "__exit__", nullptr, 0, &error);
        if (!error.empty()) return fail(error);
        break;
      }
    }
  }

  result.value = sp > 0 ? stack[sp - 1] : Value();
  return result;
}

}  // namespace vm
}  // namespace aipy
