// 脚本层的动态值
//
// 为什么需要一个动态值：Python 子集在构建期编译成 C++ 之后，变量类型不能靠
// 编译器推断——`current = "0"` 之后可能是字符串，`acc = 0.0` 之后是数字，
// 而 Python 的语义本来就允许同一个名字前后换类型。用 std::variant 式的
// Value 承载它，生成的代码就不需要类型推断，编译期只剩一件事要保证：
// 名字都能解析到。
//
// 代价是每次运算都要走一次运行期分支。这仍然比「内嵌解释器」便宜一个量级：
// 没有字节码、没有名字查表；一条 `a + b` 编译出来就是一次 inline 函数调用。
//
// ── 容器为什么走堆 + 引用计数 ──
//
// Python 的 list / dict 是**引用类型**：`b = a` 之后两者是同一个对象，
// `b.append(x)` 会改变 a。若用 std::vector 按值持有，`b = a` 就会变成深拷贝，
// 于是「改 b 不影响 a」——这看起来更安全，实则与 Python 的语义不符，而且
// 会让「函数里改传入的列表」这种最常见的写法静默失效。
//
// 所以列表与字典的数据放在堆上、由侵入式引用计数管理：赋值是共享，语义与
// CPython 一致。字符串的字符数据也一起走堆——不是因为它需要共享（str 不可变），
// 而是因为它必须离开 Value 本体（见下面「紧凑表示」）。
//
// ── 为什么不用 std::shared_ptr ──
//
// shared_ptr 自己就占 16 字节（对象指针 + 控制块指针），把它放进 Value 会让
// 结构体回到 32 字节，瘦身等于白做。侵入式计数只多一个指针，且计数与载荷
// 同一次分配，还少一个控制块。
//
// 不做原子操作：脚本全部在 UI 线程执行，跨线程共享容器不在支持范围内。
// 真要跨线程，应该显式拷贝或加锁，而不是靠引用计数的原子性。
#pragma once

#include "pyui/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pyui {

class View;

namespace py {

class Value;

using List = std::vector<Value>;

// 容器的实际数据。因为 Value 要持有它，而它又反过来装 Value，
// 只能在 Value 定义之后才能给出完整定义（见文件末尾）。
struct ListData;
struct DictData;
struct HeapData;

class Value {
 public:
  // 生成的代码用它写字面量列表：py::Value::List{a, b}
  using List = py::List;

  // uint8_t 是刻意的：默认的 int 底层类型会占 4 字节，正好把整个结构体
  // 撑过 16 字节的预算（tag 1 + 对齐 7 + 载荷 8 = 16）。
  enum class Type : std::uint8_t {
    kNone,
    kBool,
    kInt,
    kFloat,
    kString,
    kList,
    kDict,
    kView,
  };

  Value() = default;
  // 拷贝/移动必须手写：堆载荷用侵入式计数，编译器生成的版本不知道要增减。
  Value(const Value& other);
  Value(Value&& other) noexcept;
  Value& operator=(const Value& other);
  Value& operator=(Value&& other) noexcept;
  ~Value();

  Value(std::nullptr_t) {}
  Value(bool value) : type_(Type::kBool), int_(value ? 1 : 0) {}
  Value(int value) : type_(Type::kInt), int_(value) {}
  Value(long long value) : type_(Type::kInt), int_(value) {}
  Value(double value) : type_(Type::kFloat), float_(value) {}
  Value(const char* value);
  Value(std::string value);
  Value(View* value) : type_(Type::kView), view_(value) {}
  Value(List value);  // 见 py_value.cpp：包装成新的 HeapData

  Type type() const { return type_; }
  bool isNone() const { return type_ == Type::kNone; }
  bool isNumber() const { return type_ == Type::kInt || type_ == Type::kFloat; }
  bool isDict() const { return type_ == Type::kDict; }
  bool isList() const { return type_ == Type::kList; }
  // 列表或字典：`for x in y` 与 `len(y)` 都接受两种容器
  bool isContainer() const { return type_ == Type::kList || type_ == Type::kDict; }

  View* asView() const { return type_ == Type::kView ? view_ : nullptr; }
  // 非列表时返回一个空列表（静态对象，避免悬空引用）
  const List& asList() const;
  // 非字典时返回空数组
  const List& dictKeys() const;
  const List& dictValues() const;
  // 非字符串时返回空串
  const std::string& asString() const;

  // Python 的真值规则：None/0/0.0/""/[]/{}/空视图指针为假
  bool toBool() const;
  long long toInt() const;
  double toFloat() const;
  // Python str() 的等价物；浮点按 %.10g 输出并去掉多余的 0
  std::string toString() const;

  // ── 构造与查询 ──
  static Value list(List items);
  static Value dict(const List& keys, const List& values);
  // 字典取键的下标；没有这个键返回 -1。键比较用 eq 的语义。
  long long dictIndex(const Value& key) const;
  // 可变的容器视图。指向的数据由 shared_ptr 持有，所以 const 方法也能
  // 安全地返回可变指针——这恰好对应 Python 的「引用语义」：拿着同一个
  // 容器对象改内容，与这个 Value 句柄本身是不是 const 无关。
  List* mutableList() const;
  DictData* mutableDict() const;

 private:
  // ── 紧凑表示 ──
  //
  // 这个结构体被 VM 的每次 push/pop 拷贝。原型实测：96 字节时 300 万次
  // 循环 805 ms，16 字节时 1.11 ms。所以不能有 std::string 成员（它一个人
  // 就 32 字节），也不能有 std::shared_ptr（它一个人就 16 字节）。
  //
  // 布局（16 字节）：
  //   [0]  Type (uint8) + 7 字节填充
  //   [8]  int_ / float_ / view_ / heap_ 四选一的 8 字节载荷
  //
  // 堆指针塞进 union 是关键：它和数字载荷**不可能同时有效**（由 type_ 决定），
  // 所以共享那 8 字节没有歧义。这也意味着拷贝一个整数不需要碰引用计数。
  //
  // bool 特意**不用**独立字段，而是存成 kInt 的 0/1：两个字段表示同一件事
  // 就有写一个忘另一个的风险（真实踩过：Value(true) 写了 int_，toBool()
  // 读 bool_，于是 True 变 False，循环一次都不进）。
  Type type_ = Type::kNone;
  union {
    long long int_;
    double float_;
    View* view_;
    HeapData* heap_;
  };
};

// 容器的完整定义。放在 Value 之后是因为它们装的是 Value。
// 字典的键与值按下标对齐，顺序是插入顺序——Python 3.7+ 保证字典保序，
// 「按插入顺序显示」在界面上是能看见的，不能丢。
struct DictData {
  List keys;
  List values;
};

// 堆载荷：字符串、列表、字典共用一块。
//
// 为什么合成一个结构体而不是三个指针成员：三个指针就是 24 字节，
// 加上 tag 与数字载荷会让 Value 回到 32 字节，重新毁掉 VM 的性能。
// 合成之后 Value 只有一个 8 字节指针，而且**字符串与容器不可能同时是
// 有效载荷**（类型由 type_ 决定），所以复用一个指针没有歧义。
//
// 继承 DictData 而不是复制它的两个成员：mutableDict() 要返回一个能真正写
// 回堆载荷的 DictData*，继承让这个向上转型是安全的；若改成组合，那个函数
// 就只能返回副本，而副本会把写入吞掉。
//
// refs 是侵入式引用计数，非原子（脚本只在 UI 线程跑，见文件头）。
struct HeapData : DictData {
  std::string text;        // kString
  List items;              // kList
  std::uint32_t refs = 1;  // 1 表示「刚创建，调用方已持有」
};

// ── 运算符 ──
Value add(const Value& a, const Value& b);
Value sub(const Value& a, const Value& b);
Value mul(const Value& a, const Value& b);
Value div(const Value& a, const Value& b);
Value mod(const Value& a, const Value& b);
Value neg(const Value& a);
Value invert(const Value& a);  // ~x

Value eq(const Value& a, const Value& b);
Value ne(const Value& a, const Value& b);
Value lt(const Value& a, const Value& b);
Value le(const Value& a, const Value& b);
Value gt(const Value& a, const Value& b);
Value ge(const Value& a, const Value& b);

// Python 的 and/or 返回操作数本身而不是布尔值，这两个函数保持同一语义：
// `x = a or "默认"` 在脚本里是最常见的写法，返回 bool 会让它失效。
Value land(const Value& a, const Value& b);
Value lor(const Value& a, const Value& b);
Value lnot(const Value& a);

// 短路版本：右边只在需要时才求值。
//
// 为什么不能复用 land/lor：它们的参数是已经求好的值，而 Python 的
// `if x and f()` 里 f() 在 x 为假时根本不调用。生成代码用 lambda 传右边，
// 保住这个语义——否则脚本里 `if items and items[0]` 就会越界。
Value landLazy(const Value& a, const std::function<Value()>& b);
Value lorLazy(const Value& a, const std::function<Value()>& b);

// 三元表达式。同样必须惰性：只求值选中的那一支。
Value iff(const Value& condition, const std::function<Value()>& ifTrue,
          const std::function<Value()>& ifFalse);
// `item in container`
Value contains(const Value& container, const Value& item);
// 按位：& | ^
Value bitAnd(const Value& a, const Value& b);
Value bitOr(const Value& a, const Value& b);
Value bitXor(const Value& a, const Value& b);
Value shiftLeft(const Value& a, const Value& b);
Value shiftRight(const Value& a, const Value& b);

// ── 内建函数 ──
Value toNumber(const Value& v);   // float(x)
Value toInteger(const Value& v);  // int(x)
Value toText(const Value& v);     // str(x)
Value length(const Value& v);     // len(x)
Value absolute(const Value& v);   // abs(x)
Value roundValue(const Value& v); // round(x)
Value roundDigits(const Value& v, const Value& digits);  // round(x, n)
Value boolOf(const Value& v);     // bool(x)
Value sumOf(const Value& items);  // sum(iterable)
Value clone(const Value& v);      // list(v) / dict(d) / str(s)

// x[i]：字符串按 UTF-8 字符取（不是字节），列表按下标，字典按键
Value at(const Value& container, const Value& index);
// x[start:end]：start/end 可以为 None，负数从尾部算
Value slice(const Value& container, const Value& start, const Value& end);

// x ** y
Value power(const Value& a, const Value& b);
// x // y：向负无穷取整的整除（Python 语义），不是 C 的截断
Value floorDiv(const Value& a, const Value& b);

// `x is None`
Value isNone(const Value& v);
// `x is y`：容器按是否为同一对象判断，其余按值相等
Value isSame(const Value& a, const Value& b);

// for 循环遍历：列表给元素，字符串按**字符**给（不是按字节），字典给键。
// 返回一个临时 vector 而不是迭代器：脚本里的容器通常很小，而让生成代码
// 直接 range-for 一个 vector 比维护一对迭代器简单得多。
std::vector<Value> iterate(const Value& container);

// range() 的循环条件。单独一个函数是因为「步长为正/为负」的判断展开在
// 生成代码里会变成三元表达式，出问题时很难看出原本的意图。
inline bool rangeContinue(long long index, long long stop, long long step) {
  return step >= 0 ? index < stop : index > stop;
}

// ── 容器原地操作（Python 语义：就地修改，返回 None）──
void append(const Value& list, const Value& item);
void extend(const Value& list, const Value& items);
void insertAt(const Value& list, const Value& index, const Value& item);
Value popAt(const Value& list, const Value& index);
void clearContainer(const Value& container);
// 排序：字符串按字节序，数字按数值；混合类型时按类型序，与 eq 的规则一致
void sortList(const Value& list);
void reverseList(const Value& list);

Value dictFrom(const Value& keys, const Value& values);
Value dictGet(const Value& dict, const Value& key);  // 缺键返回 None
Value dictGetOr(const Value& dict, const Value& key, const Value& fallback);
void dictSet(const Value& dict, const Value& key, const Value& value);
Value dictKeysOf(const Value& dict);
Value dictValuesOf(const Value& dict);
Value dictItemsOf(const Value& dict);  // [[k, v], ...]
Value dictPop(const Value& dict, const Value& key, const Value& fallback);
void dictUpdate(const Value& target, const Value& other);

// ── 下标赋值 ──
// 对应 Python 的 `x[i] = v`：
//   列表按下标（越界时扩容，负数从尾部算）
//   字典按键（不存在则新增，保持插入顺序）
//   其它类型静默忽略——脚本里「这个 id 上到底是哪个控件」写错时，
//   界面不响应比崩溃好排查。
void setItem(const Value& container, const Value& index, const Value& value);

}  // namespace py
}  // namespace pyui
