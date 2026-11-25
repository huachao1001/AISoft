// AiPy 虚拟机的端到端测试。
//
// ── 为什么必须端到端，而不只是单测 Value ──
//
// Value 的 41 个用例证明「语义正确」，但它们不知道字节码。VM 出错的方式
// 与 Value 完全不同：跳转回填差一个字节、for 循环少弹一层栈、参数个数错位
// ——这些都表现为「值悄悄算错」而不是崩溃，正是最难从日志里看出来的一类。
//
// 所以这里的用例都从字节码出发：用 tools/pyuic/vm_compiler.py 编出来的
// 二进制，跑完看全局槽。编译器的正确性也因此被覆盖到了。
#include "aipy/bytecode.h"
#include "aipy/vm.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

std::vector<std::uint8_t> readFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>());
}

// 每个用例：加载一份字节码，跑，返回全局槽表。
std::vector<aipy::Value> runProgram(const char* path, std::string* error) {
  const std::vector<std::uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = std::string("读不到 ") + path;
    return {};
  }
  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    *error = loaded.error;
    return {};
  }
  aipy::vm::Machine machine;
  aipy::vm::Result result = machine.run(loaded.program);
  if (result.failed) {
    *error = result.error + "（第 " + std::to_string(result.line) + " 行）";
    return {};
  }
  return machine.globals();
}

void testPureProgram(const char* dir) {
  const std::string base = std::string(dir);
  std::string error;
  const std::vector<std::uint8_t> data = readFile((base + "/pure.bc").c_str());
  if (data.empty()) {
    std::printf("FAIL: 读不到 pure.bc（先跑 tests/build_bytecode.py）\n");
    ++failures;
    return;
  }
  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    std::printf("FAIL: pure.bc 加载失败: %s\n", loaded.error.c_str());
    ++failures;
    return;
  }

  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(loaded.program);
  if (result.failed) {
    std::printf("FAIL: pure.bc 执行失败: %s（第 %d 行）\n",
                result.error.c_str(), result.line);
    ++failures;
    return;
  }

  // 按名字取全局槽。产物里带名字表就是为了这个：热加载要按名字搬运状态，
  // 测试也跟着用名字，槽号变了不影响断言。
  const std::vector<aipy::Value>& globals = machine.globals();
  const auto get = [&](const char* name) {
    for (std::size_t i = 0; i < loaded.program.globalNames.size(); ++i) {
      if (loaded.program.globalNames[i] == name) return globals[i];
    }
    std::printf("FAIL: 全局变量 %s 不存在\n", name);
    ++failures;
    return aipy::Value();
  };

  expect(get("total").toInt() == 15, "for 循环累加 1..5 == 15");
  expect(get("joined").toString() == "a-b", "join 结果");
  expect(get("upper_text").toString() == "HELLO", "upper 结果");
  expect(get("text").toString() == "012", "for + range + str 拼接");
  expect(get("counter").toInt() == 3, "while + += 三次");
  expect(get("label").toString() == "big", "条件表达式");
  expect(get("doubled").toInt() == 42, "函数调用 double(21)");
  expect(get("summed").toInt() == 6, "三参数函数 add_all");
  expect(get("length").toInt() == 5, "len");
  expect(get("maximum").toInt() == 5, "max");
  expect(get("minimum").toInt() == 1, "min");
  expect(get("rounded").toFloat() > 3.13 && get("rounded").toFloat() < 3.15,
         "round(3.14159, 2)");
}

void testBadMagic() {
  const std::uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const aipy::bytecode::Result loaded = aipy::bytecode::load(junk, sizeof(junk));
  expect(loaded.failed, "非字节码文件必须被拒绝");
  expect(!loaded.error.empty(), "拒绝时要给出原因");
}

void testTruncated() {
  const std::uint8_t header[10] = {'A', 'I', 'P', 'Y', 'B', 'C', '0', '1', 1, 0};
  const aipy::bytecode::Result loaded = aipy::bytecode::load(header, sizeof(header));
  expect(loaded.failed, "截断的文件必须被拒绝");
}

void testBuiltinTable() {
  // 内建表是编译期下标与运行期函数指针的契约。名字对不上就会调用到别的
  // 函数——这是最危险的一种错，因为它不崩、只是结果微妙地不对。
  expect(aipy::builtinIndexFromName("len") >= 0, "len 在内建表里");
  expect(aipy::builtinIndexFromName("range") >= 0, "range 在内建表里");
  expect(aipy::builtinIndexFromName("不存在的函数") < 0, "查不到的名字返回 -1");
  expect(aipy::builtinCount() > 10, "内建表非空");
}

// 跑一份字节码，按名字取全局槽。返回 false 表示加载或执行失败（已打印原因）。
bool runAndFetch(const std::string& path,
                 std::map<std::string, aipy::Value>* out) {
  const std::vector<std::uint8_t> data = readFile(path.c_str());
  if (data.empty()) {
    std::printf("FAIL: 读不到 %s（先跑 tests/build_bytecode.py）\n", path.c_str());
    ++failures;
    return false;
  }
  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    std::printf("FAIL: %s 加载失败: %s\n", path.c_str(), loaded.error.c_str());
    ++failures;
    return false;
  }
  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(loaded.program);
  if (result.failed) {
    std::printf("FAIL: %s 执行失败: %s（第 %d 行）\n", path.c_str(),
                result.error.c_str(), result.line);
    ++failures;
    return false;
  }
  const std::vector<aipy::Value>& globals = machine.globals();
  for (std::size_t i = 0; i < loaded.program.globalNames.size(); ++i) {
    (*out)[loaded.program.globalNames[i]] = globals[i];
  }
  return true;
}

void testControlFlow(const char* dir) {
  std::map<std::string, aipy::Value> globals;
  if (!runAndFetch(std::string(dir) + "/control.bc", &globals)) return;

  const auto get = [&](const char* name) -> aipy::Value {
    const auto found = globals.find(name);
    if (found == globals.end()) {
      std::printf("FAIL: 全局变量 %s 不存在\n", name);
      ++failures;
      return aipy::Value();
    }
    return found->second;
  };

  // 嵌套循环 + break/continue：continue 必须跳过内层整个循环，
  // 所以 i==2 那一轮一次都不进。break 在 j==1 时中断内层。
  expect(get("found").toInt() == 3, "嵌套循环 + break/continue 计数");
  expect(get("pairs").toInt() == 6, "内层次数（continue 那一轮不进）");

  // 字典
  expect(get("value_c").toInt() == 3, "字典赋值后取值");
  expect(get("missing").toInt() == -1, "dict.get 带默认值");
  expect(get("has_b").toBool(), "in 判断字典键");
  expect(get("table").isDict(), "table 是字典");
  expect(get("total_keys").toInt() == 3, "遍历字典得到 3 个键");

  // 切片
  expect(get("head").toString() == "abc", "切片 [0:3]");
  expect(get("tail").toString() == "def", "切片 [3:]");
  expect(get("every_other").toString() == "ace", "切片带步长 [0:6:2]");
  expect(get("char").toString() == "b", "单字符下标");

  // 列表方法
  expect(get("first").toInt() == 1, "sort 后第一个");
  expect(get("last").toInt() == 4, "pop 返回末元素");
  expect(get("count_after_pop").toInt() == 3, "pop 后长度");

  // 递归：显式帧栈必须撑住。这里只是 5 层，深递归另有专测。
  expect(get("fact5").toInt() == 120, "递归 factorial(5)");

  // 链式比较：中间项只求值一次，且整个表达式的值是布尔。
  expect(get("in_range").toBool(), "1 < 5 < 10");
  expect(!get("not_in_range").toBool(), "1 < 5 < 3 为假");
  expect(get("simple").toBool(), "单比较");

  // 负下标与 Python 的负数语义
  expect(get("negative").toInt() == 30, "负下标从尾部算");
  expect(get("floored").toInt() == -4, "-7 // 2 == -4（向下取整）");
  expect(get("modulo").toInt() == 1, "-7 % 2 == 1（符号跟除数）");

  // and/or 返回操作数本身
  expect(get("fallback").toString() == "default", "\"\" or \"default\"");
  expect(get("keep").toString() == "value", "\"value\" or \"other\"");
  expect(get("both").toInt() == 2, "1 and 2 == 2");
}

void testErrorLine(const char* dir) {
  // 出错必须带源行号。没有行号的错误在界面脚本里根本查不出来——
  // 「这个类型没有方法 upper」不告诉你在第几行，等于没说。
  const std::string path = std::string(dir) + "/error_line.bc";
  const std::vector<std::uint8_t> data = readFile(path.c_str());
  if (data.empty()) {
    std::printf("FAIL: 读不到 error_line.bc\n");
    ++failures;
    return;
  }
  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    std::printf("FAIL: error_line.bc 加载失败: %s\n", loaded.error.c_str());
    ++failures;
    return;
  }
  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(loaded.program);
  expect(result.failed, "错误脚本必须失败");
  // 出错在第 8 行：broken = value.upper()。前四行是注释，
  // 断言取真实行号而不是「大概在第几行」——这正是这条测试的意义。
  expect(result.line == 8, "错误行号必须是 8（value.upper() 那一行）");
  expect(!result.error.empty(), "错误要有原因");
}

void testCarryGlobals() {
  // 热加载：旧程序的两个变量搬到新程序，按名字而不是按槽号。
  // 新程序里故意换了顺序，按槽号搬就会把 count 的值给 label。
  aipy::vm::Program from;
  from.globalNames = {"count", "label"};
  from.globalCount = 2;

  aipy::vm::Program to;
  to.globalNames = {"label", "count", "extra"};
  to.globalCount = 3;

  const std::vector<aipy::Value> old = {aipy::Value(7LL), aipy::Value(std::string("hi"))};
  const std::vector<aipy::Value> carried = aipy::vm::carryGlobals(from, old, to);

  expect(carried.size() == 3, "搬运后长度等于新程序的全局槽数");
  expect(carried[0].toString() == "hi", "label 按名字搬到了新槽 0");
  expect(carried[1].toInt() == 7, "count 按名字搬到了新槽 1");
  expect(carried[2].isNone(), "新程序多出来的变量是 None");
}

void testDeepRecursion(const char* dir) {
  // 显式帧栈的意义就在这一条：200 层递归必须跑通，且不影响 C++ 栈。
  // 哪天执行循环被改回递归调用 run()，这里会当场栈溢出而不是安静地算错。
  std::map<std::string, aipy::Value> globals;
  if (!runAndFetch(std::string(dir) + "/recursion.bc", &globals)) return;
  const auto found = globals.find("shallow");
  if (found == globals.end()) {
    std::printf("FAIL: recursion.bc 没有 shallow\n");
    ++failures;
    return;
  }
  expect(found->second.toInt() == 200, "200 层递归返回 200");
}

void testCallDepthLimit() {
  // 超过上限要给出可读错误，而不是崩。这里直接构造一段无限递归的字节码：
  //   loop(n): LOAD_LOCAL 0 / CALL loop,1 / RETURN
  // 参数原样传下去，所以永远不收敛。用最小的手写字节码验证「上限真的
  // 生效」，不依赖 Python 编译器。
  //
  // 顺带验证共享栈不会先溢出：参数移进 locals 后 sp 回到原处，每层只多
  // 一份 locals，所以先撞上的一定是 kMaxCallDepth。
  aipy::vm::Program program;
  program.globalCount = 0;
  program.localCount = 1;

  aipy::vm::Function function;
  function.name = "loop";
  function.paramCount = 1;
  function.localCount = 1;
  function.code = {
      static_cast<std::uint8_t>(aipy::vm::Op::kLoadLocal), 0, 0,
      static_cast<std::uint8_t>(aipy::vm::Op::kCall), 0, 0, 1,
      static_cast<std::uint8_t>(aipy::vm::Op::kReturn),
  };
  function.lines.push_back(aipy::vm::LineEntry{0, 1});
  program.functions.push_back(function);

  // 模块级：先压一个参数再调用（CALL 从栈上取参数）。
  program.code = {
      static_cast<std::uint8_t>(aipy::vm::Op::kNone),
      static_cast<std::uint8_t>(aipy::vm::Op::kCall), 0, 0, 1,
      static_cast<std::uint8_t>(aipy::vm::Op::kReturn),
  };
  program.lines.push_back(aipy::vm::LineEntry{0, 1});

  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(program);
  expect(result.failed, "无限递归必须在某个深度被拦住");
  expect(result.error.find("调用层数") != std::string::npos,
         "拦住的错误要说明是调用层数，而不是笼统的失败");
}

}  // namespace

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";

  testBuiltinTable();
  testBadMagic();
  testTruncated();
  testPureProgram(dir);
  testControlFlow(dir);
  testErrorLine(dir);
  testCarryGlobals();
  testDeepRecursion(dir);
  testCallDepthLimit();

  std::printf("vm tests: %s（%d 个失败）\n", failures == 0 ? "OK" : "FAILED", failures);
  return failures == 0 ? 0 : 1;
}
