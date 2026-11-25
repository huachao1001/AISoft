// Value 的语义测试
//
// ── 为什么这一份必须存在 ──
//
// 整个库的兼容性承诺是「同一份脚本，CPython 跑一遍、编译产物跑一遍，
// 结果一样」。这句话能否成立，几乎全部取决于这一层：脚本里的每个运算、
// 每次容器修改都会落到 aipy::Value 上。控件画错了看得见，这里的语义错了
// 却可能只在某个分支里悄悄给出不同结果。
//
// 每个用例都对应 CPython 的一条可观察行为，注释里写出那句 Python 代码。
#include "aipy/value.h"

#include "test_framework.h"

#include <string>

namespace aipy {
namespace {

using aipy::Value;

// ── 真值（bool(x)）──

TEST(py_value_none_is_falsy) {
  CHECK(!Value().toBool());
  CHECK(Value().isNone());
}

TEST(py_value_int_zero_only_is_falsy) {
  CHECK(!Value(0LL).toBool());
  CHECK(Value(1LL).toBool());
  // bool(-1) 在 Python 里是 True：负数非零也为真，不是「> 0」
  CHECK(Value(-1LL).toBool());
}

TEST(py_value_empty_containers_are_falsy) {
  // bool([]) == False，bool([0]) == True
  CHECK(!Value::list(aipy::Value::List{}).toBool());
  CHECK(Value::list(aipy::Value::List{Value(0LL)}).toBool());
}

TEST(py_value_empty_string_is_falsy) {
  CHECK(!Value(std::string()).toBool());
  // "0" 非空，所以是真——这一条最容易在移植时写错
  CHECK(Value(std::string("0")).toBool());
}

// ── 整数与浮点 ──

TEST(py_value_int_division_truncates_toward_zero) {
  // Python 的 // 是向下取整；/ 是真除。生成代码用 div 与 floorDiv 分开。
  CHECK_EQ(aipy::div(Value(7LL), Value(2LL)).toInt(), 3LL);
  CHECK_NEAR(aipy::div(Value(7LL), Value(2LL)).toFloat(), 3.5, 1e-9);
}

TEST(py_value_floor_division_rounds_down) {
  // -7 // 2 == -4（向负无穷取整，不是向零截断）
  CHECK_EQ(aipy::floorDiv(Value(-7LL), Value(2LL)).toInt(), -4LL);
  CHECK_EQ(aipy::floorDiv(Value(7LL), Value(2LL)).toInt(), 3LL);
}

TEST(py_value_modulo_follows_python_sign) {
  // Python 里 -7 % 3 == 2（结果符号跟随除数），C++ 的 % 会给 -1。
  // 脚本里做环形下标、时间换算时这条差异会立刻暴露。
  CHECK_EQ(aipy::mod(Value(-7LL), Value(3LL)).toInt(), 2LL);
  CHECK_EQ(aipy::mod(Value(7LL), Value(3LL)).toInt(), 1LL);
}

TEST(py_value_adding_int_and_float_gives_float) {
  CHECK_EQ(static_cast<int>(aipy::add(Value(1LL), Value(2.5)).type()),
           static_cast<int>(Value::Type::kFloat));
  CHECK_NEAR(aipy::add(Value(1LL), Value(2.5)).toFloat(), 3.5, 1e-9);
}

TEST(py_value_string_addition_concatenates) {
  const Value result = aipy::add(Value(std::string("a")), Value(std::string("b")));
  CHECK_EQ(result.toString(), std::string("ab"));
}

TEST(py_value_string_times_int_repeats) {
  const Value result = aipy::mul(Value(std::string("ab")), Value(3LL));
  CHECK_EQ(result.toString(), std::string("ababab"));
}

TEST(py_value_power_and_negative_exponent) {
  CHECK_EQ(aipy::power(Value(2LL), Value(10LL)).toInt(), 1024LL);
  // 2 ** -1 == 0.5：负指数返回浮点，不返回 0
  CHECK_NEAR(aipy::power(Value(2LL), Value(-1LL)).toFloat(), 0.5, 1e-9);

  // 整数幂的结果必须是 int，不能是 float：str(2 ** 10) 在 CPython 里
  // 是 "1024"，返回 float 会得到 "1024.0"，界面上立刻看出区别。
  const Value integer = aipy::power(Value(2LL), Value(10LL));
  CHECK_EQ(integer.toString(), std::string("1024"));
  CHECK(integer.type() == Value::Type::kInt);

  // 0 ** 0 == 1（CPython 的定义，不是数学上的未定义）
  CHECK_EQ(aipy::power(Value(0LL), Value(0LL)).toInt(), 1LL);
  // 浮点指数仍得到浮点：4 ** 0.5 == 2.0
  CHECK_NEAR(aipy::power(Value(4LL), Value(0.5)).toFloat(), 2.0, 1e-9);
  CHECK(aipy::power(Value(4LL), Value(0.5)).type() == Value::Type::kFloat);
}

// ── 比较 ──

TEST(py_value_comparison_is_numeric_across_types) {
  // 1 == 1.0 在 Python 里是 True
  CHECK(aipy::eq(Value(1LL), Value(1.0)).toBool());
  // Python 里 bool 是 int 的子类型：True == 1、0 == False
  CHECK(aipy::eq(Value(0LL), Value(false)).toBool());
  CHECK(aipy::eq(Value(1LL), Value(true)).toBool());
  // 连带的大小比较也要把它们当数字
  CHECK(aipy::lt(Value(false), Value(true)).toBool());
}

TEST(py_value_string_comparison_is_lexicographic) {
  CHECK(aipy::lt(Value(std::string("a")), Value(std::string("b"))).toBool());
  CHECK(aipy::ge(Value(std::string("b")), Value(std::string("b"))).toBool());
}

TEST(py_value_contains_checks_lists_and_strings) {
  const Value items = Value::list(aipy::Value::List{Value(1LL), Value(2LL)});
  CHECK(aipy::contains(items, Value(2LL)).toBool());
  CHECK(!aipy::contains(items, Value(3LL)).toBool());
  // 3 in "123" —— 子串判断，不是字符判断
  CHECK(aipy::contains(Value(std::string("123")), Value(std::string("23"))).toBool());
}

// ── 逻辑（and/or 返回操作数本身）──

TEST(py_value_and_or_return_operands_not_bools) {
  // x = "" or "默认" → "默认"；这是脚本里最常见的写法
  CHECK_EQ(aipy::lor(Value(std::string("")), Value(std::string("默认"))).toString(),
           std::string("默认"));
  // "a" and "b" → "b"
  CHECK_EQ(aipy::land(Value(std::string("a")), Value(std::string("b"))).toString(),
           std::string("b"));
}

TEST(py_value_lazy_and_does_not_evaluate_right) {
  // 对应 `if items and items[0]`：左边为假时右边不能被求值，
  // 否则空列表取下标就会越界。
  bool evaluated = false;
  const Value result = aipy::landLazy(Value(false), [&evaluated]() {
    evaluated = true;
    return Value(1LL);
  });
  CHECK(!result.toBool());
  CHECK_MSG(!evaluated, "右边在左边为假时被求值了");
}

TEST(py_value_lazy_or_does_not_evaluate_right_when_true) {
  bool evaluated = false;
  const Value result = aipy::lorLazy(Value(std::string("v")), [&evaluated]() {
    evaluated = true;
    return Value(1LL);
  });
  CHECK_EQ(result.toString(), std::string("v"));
  CHECK_MSG(!evaluated, "右边在左边为真时被求值了");
}

TEST(py_value_ternary_evaluates_only_one_branch) {
  bool taken = false;
  const Value result = aipy::iff(Value(true),
                               [&taken]() {
                                 taken = true;
                                 return Value(std::string("yes"));
                               },
                               []() { return Value(std::string("no")); });
  CHECK_EQ(result.toString(), std::string("yes"));
  CHECK(taken);
}

// ── 列表：引用语义 ──
//
// 这是 aipy::Value 与「C++ 值语义」差别最大的一处，也是最容易写成
// 「复制一份」的地方。Python 里 b = a 之后 b.append(1) 会改到 a。

TEST(py_value_list_assignment_aliases_the_same_container) {
  Value a = Value::list(aipy::Value::List{Value(1LL)});
  Value b = a;  // 对应 Python 的 b = a
  aipy::append(b, Value(2LL));
  CHECK_EQ(aipy::length(a).toInt(), 2LL);
  CHECK_EQ(aipy::length(b).toInt(), 2LL);
}

TEST(py_value_list_negative_index_reads_from_tail) {
  const Value items = Value::list(aipy::Value::List{Value(1LL), Value(2LL), Value(3LL)});
  CHECK_EQ(aipy::at(items, Value(-1LL)).toInt(), 3LL);
  CHECK_EQ(aipy::at(items, Value(0LL)).toInt(), 1LL);
}

TEST(py_value_list_out_of_range_returns_none) {
  // Python 会抛 IndexError；脚本层没有异常通道，所以返回 None——
  // 这样 `if x is None` 能兜住，而不是让程序崩。
  const Value items = Value::list(aipy::Value::List{Value(1LL)});
  CHECK(aipy::at(items, Value(5LL)).isNone());
}

TEST(py_value_list_slice_returns_copy) {
  // a[1:3] 是新的列表；改它不会影响原列表
  const Value items = Value::list(aipy::Value::List{Value(0LL), Value(1LL), Value(2LL), Value(3LL)});
  const Value sliced = aipy::slice(items, Value(1LL), Value(3LL));
  CHECK_EQ(aipy::length(sliced).toInt(), 2LL);
  CHECK_EQ(aipy::at(sliced, Value(0LL)).toInt(), 1LL);
  CHECK_EQ(aipy::length(items).toInt(), 4LL);
}

TEST(py_value_set_item_grows_list_like_python_append) {
  Value items = Value::list(aipy::Value::List{});
  aipy::setItem(items, Value(0LL), Value(std::string("a")));
  CHECK_EQ(aipy::length(items).toInt(), 1LL);
  CHECK_EQ(aipy::at(items, Value(0LL)).toString(), std::string("a"));
}

TEST(py_value_list_pop_removes_and_returns) {
  Value items = Value::list(aipy::Value::List{Value(1LL), Value(2LL)});
  const Value removed = aipy::popAt(items, Value(-1LL));
  CHECK_EQ(removed.toInt(), 2LL);
  CHECK_EQ(aipy::length(items).toInt(), 1LL);
}

TEST(py_value_list_sort_is_numeric_order) {
  Value items = Value::list(aipy::Value::List{Value(3LL), Value(1LL), Value(2LL)});
  aipy::sortList(items);
  CHECK_EQ(aipy::at(items, Value(0LL)).toInt(), 1LL);
  CHECK_EQ(aipy::at(items, Value(2LL)).toInt(), 3LL);
}

TEST(py_value_iterate_over_dict_yields_keys) {
  // 脚本里 `for k in d` 拿到的是键
  Value dict = Value::dict(aipy::Value::List{Value(std::string("a"))},
                           aipy::Value::List{Value(1LL)});
  const std::vector<Value> keys = aipy::iterate(dict);
  CHECK_EQ(keys.size(), static_cast<size_t>(1));
  CHECK_EQ(keys[0].toString(), std::string("a"));
}

// ── 字典 ──

TEST(py_value_dict_lookup_and_missing) {
  Value dict = Value::dict(aipy::Value::List{Value(std::string("k"))},
                           aipy::Value::List{Value(7LL)});
  CHECK_EQ(aipy::dictGet(dict, Value(std::string("k"))).toInt(), 7LL);
  CHECK(aipy::dictGet(dict, Value(std::string("nope"))).isNone());
  // d.get(k, 默认) 在键不存在时给默认值
  CHECK_EQ(aipy::dictGetOr(dict, Value(std::string("nope")), Value(0LL)).toInt(), 0LL);
}

TEST(py_value_dict_keeps_insertion_order) {
  // Python 3.7+ 保证字典按插入顺序迭代；脚本里靠它做「最近使用」列表
  Value dict = Value::dict(aipy::Value::List{}, aipy::Value::List{});
  aipy::dictSet(dict, Value(std::string("b")), Value(1LL));
  aipy::dictSet(dict, Value(std::string("a")), Value(2LL));
  aipy::dictSet(dict, Value(std::string("c")), Value(3LL));
  const std::vector<Value> keys = aipy::iterate(aipy::dictKeysOf(dict));
  CHECK_EQ(keys.size(), static_cast<size_t>(3));
  CHECK_EQ(keys[0].toString(), std::string("b"));
  CHECK_EQ(keys[1].toString(), std::string("a"));
  CHECK_EQ(keys[2].toString(), std::string("c"));
}

TEST(py_value_dict_assignment_is_also_reference_semantics) {
  Value a = Value::dict(aipy::Value::List{}, aipy::Value::List{});
  Value b = a;
  aipy::dictSet(b, Value(std::string("k")), Value(1LL));
  CHECK_EQ(aipy::dictGet(a, Value(std::string("k"))).toInt(), 1LL);
}

TEST(py_value_dict_pop_returns_value) {
  Value dict = Value::dict(aipy::Value::List{Value(std::string("k"))},
                           aipy::Value::List{Value(5LL)});
  CHECK_EQ(aipy::dictPop(dict, Value(std::string("k")), Value()).toInt(), 5LL);
  CHECK(aipy::dictGet(dict, Value(std::string("k"))).isNone());
}

TEST(py_value_bool_arithmetic_behaves_like_int) {
  // True + True == 2：脚本里把布尔当计数用不算罕见，
  // 如果 add 不认 kBool 就会得到 0 或抛错。
  CHECK_EQ(aipy::add(Value(true), Value(true)).toInt(), 2LL);
}

// ── 字符串 ──

TEST(py_value_string_indexing_is_by_character) {
  // "abc"[1] == "b"；中文要按字符而不是字节，否则切出来是半个字
  CHECK_EQ(aipy::at(Value(std::string("abc")), Value(1LL)).toString(), std::string("b"));
  const Value chinese = Value(std::string("中文"));
  CHECK_EQ(aipy::at(chinese, Value(1LL)).toString(), std::string("文"));
  CHECK_EQ(aipy::length(chinese).toInt(), 2LL);
}

TEST(py_value_string_slice_is_by_character) {
  const Value text = Value(std::string("中文abc"));
  CHECK_EQ(aipy::slice(text, Value(0LL), Value(2LL)).toString(), std::string("中文"));
  CHECK_EQ(aipy::slice(text, Value(2LL), Value()).toString(), std::string("abc"));
}

// ── 数值转换（脚本里 str()/int() 的行为）──

TEST(py_value_int_of_float_truncates_toward_zero) {
  // int(-1.7) == -1，不是 -2
  CHECK_EQ(aipy::toInteger(Value(-1.7)).toInt(), -1LL);
  CHECK_EQ(aipy::toInteger(Value(1.9)).toInt(), 1LL);
}

TEST(py_value_text_of_float_keeps_python_spelling) {
  // str(1.0) == "1.0"：整数取值的浮点也必须带小数点，否则用户看不出
  // 这是浮点，拼出 "2" + "1" 会得到 "21" 而不是 "2.01"。
  CHECK_EQ(aipy::toText(Value(1.0)).toString(), std::string("1.0"));
  CHECK_EQ(aipy::toText(Value(0.5)).toString(), std::string("0.5"));
  // 但 int 就是没有小数点
  CHECK_EQ(aipy::toText(Value(1LL)).toString(), std::string("1"));
}

TEST(py_value_round_uses_half_even) {
  // CPython 的 round 用「银行家舍入」：round(0.5) == 0，round(1.5) == 2。
  // 如果这里用了「四舍五入」，脚本算出的数会与解释器不一致。
  CHECK_NEAR(aipy::roundValue(Value(0.5)).toFloat(), 0.0, 1e-9);
  CHECK_NEAR(aipy::roundValue(Value(1.5)).toFloat(), 2.0, 1e-9);
  CHECK_NEAR(aipy::roundValue(Value(2.5)).toFloat(), 2.0, 1e-9);
}

TEST(py_value_clone_is_shallow_like_python_copy_of_list) {
  // list(a) 是浅拷贝：新列表、同一批元素
  Value items = Value::list(aipy::Value::List{Value(1LL)});
  Value copied = aipy::clone(items);
  aipy::append(copied, Value(2LL));
  CHECK_EQ(aipy::length(items).toInt(), 1LL);
  CHECK_EQ(aipy::length(copied).toInt(), 2LL);
}

// ── 类型判断 ──

TEST(py_value_is_same_only_for_none_and_identity) {
  // `x is None` 是脚本里最常用的判空；非 None 的两个值一律为假
  CHECK(aipy::isSame(Value(), Value()).toBool());
  CHECK(!aipy::isSame(Value(1LL), Value()).toBool());
  CHECK(!aipy::isSame(Value(std::string("")), Value()).toBool());
}

TEST(py_value_none_detection_for_failed_write) {
  // write_file 失败返回错误串、成功返回 None；脚本里写的是
  // `if error is not None:`。这里验证成功/失败两侧分别被判成 None 与非 None。
  const Value success = Value();
  const Value failure = Value(std::string("磁盘已满"));
  CHECK(success.isNone());
  CHECK(!failure.isNone());
  CHECK(aipy::isSame(success, Value()).toBool());
  CHECK(!aipy::isSame(failure, Value()).toBool());
}

// ── 浮点转文本 ──
//
// 期望值全部由 CPython 生成。这条链路看起来只是「打印」，实际上是
// 「脚本把数字拼进界面文字」的唯一入口：str() 少一位有效数字，界面上就是
// 另一个数；指数阈值差一格，1e15 会显示成 "1e+15" 而不是
// "1000000000000000.0"。
//
// 用宏而不是 CHECK_EQ(Value(x).toString(), "..."):浮点字面量出现在
// 期望值里时，编译器会把两侧都当 double 处理，反而看不出哪一边写错了。
#define CHECK_STR(value, expected)                                           \
  do {                                                                       \
    const std::string actual_ = Value(static_cast<double>(value)).toString(); \
    if (actual_ != std::string(expected)) {                                  \
      FAIL_AT(std::string("str(" #value ") 应为 \"") + (expected) +          \
              "\"，实际 \"" + actual_ + "\"");                               \
    }                                                                        \
  } while (false)

TEST(py_value_float_text_matches_cpython) {
  // 整数与符号
  CHECK_STR(0.0, "0.0");
  CHECK_STR(-0.0, "-0.0");  // CPython 保留负零的符号
  CHECK_STR(1.0, "1.0");
  CHECK_STR(-1.0, "-1.0");
  CHECK_STR(2.0, "2.0");
  CHECK_STR(100.0, "100.0");

  // 常见小数
  CHECK_STR(0.5, "0.5");
  CHECK_STR(0.1, "0.1");
  CHECK_STR(2.5, "2.5");
  CHECK_STR(1.5, "1.5");
  CHECK_STR(1234.5678, "1234.5678");

  // 最短往返：这些值少一位有效数字，解析回来就不是原来的 double
  CHECK_STR(0.3333333333333333, "0.3333333333333333");
  CHECK_STR(0.30000000000000004, "0.30000000000000004");
  CHECK_STR(3.14159265358979, "3.14159265358979");

  // 指数记法的阈值：decpt <= -4 或 decpt > 16
  // 1e15 的整数部分有 16 位，CPython 仍用定点写法
  CHECK_STR(1000000000000000.0, "1000000000000000.0");
  CHECK_STR(123456789012345.0, "123456789012345.0");
  CHECK_STR(1234567890123456.0, "1234567890123456.0");
  // 1e16 起改为指数记法
  CHECK_STR(1e16, "1e+16");
  CHECK_STR(1e17, "1e+17");
  CHECK_STR(1e21, "1e+21");

  // 0.0001 是定点，1e-05 已越过阈值
  CHECK_STR(0.0001, "0.0001");
  CHECK_STR(1e-05, "1e-05");
  CHECK_STR(1e-06, "1e-06");

  // 极端值：格式化不能崩、不能丢精度
  CHECK_STR(1e100, "1e+100");
  CHECK_STR(1e-100, "1e-100");
  CHECK_STR(1.7976931348623157e308, "1.7976931348623157e+308");
  CHECK_STR(5e-324, "5e-324");
  CHECK_STR(1.2345678901234568e-10, "1.2345678901234568e-10");
}

#undef CHECK_STR

// ── round(x, n) ──
//
// 期望值同样取自 CPython。这一组全是「先乘再取整」的经典翻车点：
// round(2.675, 2) 的正确答案是 2.67，因为 double 里 2.675 的精确值是
// 2.67499999...；而 round(1.335, 2) 是 1.33，round(8.005, 2) 是 8.01。
// 这些值没有规律，只能实测，所以一条条列出来。
TEST(py_value_round_digits_matches_cpython) {
  CHECK_EQ(aipy::roundDigits(Value(2.675), Value(2LL)).toFloat(), 2.67);
  CHECK_EQ(aipy::roundDigits(Value(1.335), Value(2LL)).toFloat(), 1.33);
  CHECK_EQ(aipy::roundDigits(Value(0.125), Value(2LL)).toFloat(), 0.12);
  CHECK_EQ(aipy::roundDigits(Value(0.135), Value(2LL)).toFloat(), 0.14);
  CHECK_EQ(aipy::roundDigits(Value(1.005), Value(2LL)).toFloat(), 1.0);
  CHECK_EQ(aipy::roundDigits(Value(4.35), Value(1LL)).toFloat(), 4.3);
  CHECK_EQ(aipy::roundDigits(Value(4.45), Value(1LL)).toFloat(), 4.5);
  CHECK_EQ(aipy::roundDigits(Value(8.005), Value(2LL)).toFloat(), 8.01);

  // 0.1 + 0.2 的正确结果：舍入到 2 位就是 0.3
  CHECK_EQ(aipy::roundDigits(Value(0.30000000000000004), Value(2LL)).toFloat(), 0.3);
  CHECK_EQ(aipy::roundDigits(Value(0.3333333333333333), Value(5LL)).toFloat(), 0.33333);
  CHECK_EQ(aipy::roundDigits(Value(0.9999), Value(3LL)).toFloat(), 1.0);

  // n = 0 仍是就近取偶，且返回 float（不是 int）
  CHECK_EQ(aipy::roundDigits(Value(2.5), Value(0LL)).toFloat(), 2.0);
  CHECK_EQ(aipy::roundDigits(Value(1.5), Value(0LL)).toFloat(), 2.0);
  CHECK_EQ(aipy::roundDigits(Value(-2.5), Value(0LL)).toFloat(), -2.0);
  CHECK_EQ(aipy::roundDigits(Value(-1.5), Value(0LL)).toFloat(), -2.0);
  CHECK_EQ(aipy::roundDigits(Value(-0.125), Value(2LL)).toFloat(), -0.12);

  CHECK_EQ(aipy::roundDigits(Value(1234.5678), Value(2LL)).toFloat(), 1234.57);
  // 超出 double 精度的大数：不能进位到错误的方向
  CHECK_EQ(aipy::roundDigits(Value(1000000000000000.5), Value(0LL)).toFloat(),
           1000000000000000.0);
}

}  // namespace
}
