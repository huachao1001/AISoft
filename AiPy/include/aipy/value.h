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
// ── 计数必须是原子的 ──
//
// 这是**对象回收机制自身的正确性**，不是「支持多线程的代价」。非原子计数
// 的正确性依赖「所有持有者恰好都在同一个线程」这条看不见的约定，而它
// 没有编译期检查、没有运行期检查——两个线程各自持有同一个 list 的句柄时，
// 各自的析构会在同一次 `--refs` 上竞争，可能提前归零（用已释放的内存）
// 或永不归零（泄漏）。两者都是未定义行为，且症状远离出错点。
//
// 实测代价可以忽略（Intel x64，各 900 万次取最好值）：容器句柄拷贝
// 45.8 → 45.9 ms（+0.2%），算术求值 12.1 → 12.1 ms（0%），字符串构造
// 40.2 → 41.0 ms（+2.0%）。`lock inc/dec` 只在缓存行被争用时才贵，
// 单线程独占缓存行时几乎免费。refs 也从 4 字节涨到 4 字节，Value 仍是
// 16 字节（见铁律二）。
//
// 宽限：`memory_order_relaxed` 用于增加。这里不需要顺序保证——拿到句柄
// 的前提是句柄已经存在，而它存在意味着释放还没发生；释放用 acq_rel 保证
// 载荷的写入对最后一个持有者可见。
#pragma once

#include "aipy/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aipy {

class Value;

using List = std::vector<Value>;

// 容器的实际数据。因为 Value 要持有它，而它又反过来装 Value，
// 只能在 Value 定义之后才能给出完整定义（见文件末尾）。
struct ListData;
struct DictData;
struct HeapData;
// kObject 的方法表与私有状态。完整定义在 Value 之后（见「带方法的宿主对象」），
// 这里只需要名字——Value 的签名里出现的是指针。
struct MethodTable;
struct ObjectData;

class Value {
 public:
  // 生成的代码用它写字面量列表：aipy::Value::List{a, b}
  using List = aipy::List;

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
    kHost,
    // 带方法的宿主对象：`threading.Lock()` 返回它。
    //
    // 与 kHost 的区别是**方法**：kHost 只是「原样存、原样带回去」的不透明
    // 指针，脚本不能对它调任何方法；kObject 带一张方法表，`lock.acquire()`
    // 才成立。表由 pylib 注册（见 MethodTable），VM 只负责按名字转发，
    // 所以这里不含任何具体能力——符合「核心 VM 只认 Python 语法」。
    kObject,
    // 元组。与列表共用 HeapData::items 载荷，但类型独立：CPython 里
    // `(1,2) == [1,2]` 是 False，混成一体会在比较与打印上静默出错。
    kTuple,
    // 集合。载荷复用 HeapData::items，保持插入序并去重。
    // 不用哈希表：脚本里的集合通常只有几个元素，线性查重更快也更省内存；
    // 代价是打印顺序是插入序，而 CPython 按哈希序（已在 LANGUAGE.md 记录）。
    kSet,
    // 字节串。载荷复用 HeapData::text。必须与字符串分开：CPython 里
    // `b"abc" == "abc"` 是 False，且下标返回 int、迭代返回 int。
    kBytes,
    // 「这个槽没有值」——`del x` 之后的状态。CPython 里再读会 NameError，
    // 而 None 是合法的值，两者必须能区分。载荷是变量名（堆字符串），
    // 报错时要把它写进消息。
    kDeleted,
  };

  // `= default` 会把 union 成员留成未初始化值。type_ 有默认值所以类型判断
  // 不受影响，但拷贝/移动构造在 kNone/kBool/kInt 分支会读 other.int_——
  // 读未初始化的成员是 UB，Release 下被优化暴露出「子线程返回时崩」
  // （0xC0000409），Debug 却能跑。显式清零既消除 UB，也不增加大小。
  Value() : type_(Type::kNone), int_(0) {}
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
  Value(Host value) : type_(Type::kHost), host_(value) {}
  Value(List value);  // 见 py_value.cpp：包装成新的 HeapData

  Type type() const { return type_; }
  bool isNone() const { return type_ == Type::kNone; }
  bool isNumber() const { return type_ == Type::kInt || type_ == Type::kFloat; }
  bool isDict() const { return type_ == Type::kDict; }
  bool isList() const { return type_ == Type::kList; }
  bool isTuple() const { return type_ == Type::kTuple; }
  bool isSet() const { return type_ == Type::kSet; }
  bool isBytes() const { return type_ == Type::kBytes; }
  // 序列：列表或元组。索引、切片、len、迭代、`+`/`*` 都按同一套处理，
  // 差别只在不支持原地修改（元组不可变）。
  bool isSequence() const { return type_ == Type::kList || type_ == Type::kTuple; }
  // 列表或字典：`for x in y` 与 `len(y)` 都接受两种容器
  bool isContainer() const {
    return type_ == Type::kList || type_ == Type::kDict ||
           type_ == Type::kTuple || type_ == Type::kSet;
  }

  Host asHost() const { return type_ == Type::kHost ? host_ : nullptr; }
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
  static Value tuple(List items);
  // 集合：内部按加入顺序去重。
  static Value set(List items);
  // 字节串。与 string 分开存放：`b"abc" == "abc"` 在 CPython 里是 False。
  static Value bytes(std::string text);
  static Value dict(const List& keys, const List& values);
  // 「已删除」哨兵：`del x` 之后槽里放它，再读会抛 NameError。名字用于报错。
  static Value deleted(std::string name);
  bool isDeleted() const { return type_ == Type::kDeleted; }
  // 被删除的变量名；不是 kDeleted 时返回空串。
  std::string deletedName() const;
  // 带方法的宿主对象（`threading.Lock()`）。table 决定能调哪些方法，
  // destroy 在引用计数归零时调用（锁靠它释放系统资源）。
  static Value object(void* host, const MethodTable* table,
                      void (*destroy)(void* host));
  // 对象的方法表；不是 kObject 时返回 nullptr。
  // 返回的 ObjectData* 指向堆载荷，生命周期与这个 Value 的句柄一致。
  ObjectData* asObject() const;
  // 堆载荷的地址，用来判断「是不是同一个对象」（`is` 与默认的 `==`）。
  // 返回 const void* 而不是 HeapData*：外部只需要比地址，不该看见载荷布局。
  const void* identity() const { return heap_; }
  // 字典取键的下标；没有这个键返回 -1。键比较用 eq 的语义。
  long long dictIndex(const Value& key) const;
  // 可变的容器视图。指向的数据由 shared_ptr 持有，所以 const 方法也能
  // 安全地返回可变指针——这恰好对应 Python 的「引用语义」：拿着同一个
  // 容器对象改内容，与这个 Value 句柄本身是不是 const 无关。
  List* mutableList() const;
  DictData* mutableDict() const;

  // 异常载荷（见 value.cpp 的「异常」）。用 friend 而不是公有成员：
  // 那两个字段是 VM 的内部表示，不该成为宿主可见的 API 面。
  friend Value makeException(const std::string& type, const std::string& message);
  friend std::string exceptionType(const Value& value);
  friend std::string exceptionMessage(const Value& value);
  friend Value makeBuiltinClass(const std::string& name);
  friend bool isBuiltinClass(const Value& value);
  friend std::string builtinClassName(const Value& value);
  // 函数值与异常同构：载荷是语言语义，不该成为宿主可见的 API 面。
  friend Value makeFunction(int index, List captures);
  friend int functionIndexOf(const Value& value);
  friend const List& functionCaptures(const Value& value);
  // 类对象与实例：同样是 kObject 的一种，表指针就是判据。
  friend Value makeClass(std::string name, List bases, List memberKeys,
                         List memberValues);
  friend Value makeInstance(const Value& cls);
  friend bool isClass(const Value& value);
  friend bool isInstance(const Value& value);
  friend std::string classNameOf(const Value& value);
  friend const List& classBases(const Value& value);
  friend const List& classMemberKeys(const Value& value);
  friend const List& classMemberValues(const Value& value);
  // 实例的字段（名字与值平行，保持插入序）。非实例返回静态空表。
  friend List& mutableInstanceKeys(const Value& value);
  friend List& mutableInstanceValues(const Value& value);
  friend const List& instanceKeys(const Value& value);
  friend const List& instanceValues(const Value& value);
  friend Value instanceClass(const Value& value);
  // 已绑定实例的方法值（`obj.method` 的结果）：独立标记表，载荷是函数号
  // 与 self。调用时把 self 补成第一个位置实参。
  friend Value makeBoundMethod(int index, Value self);
  friend bool isBoundMethod(const Value& value);
  friend Value boundMethodSelf(const Value& value);
  // `super()`：self + 当前类，查找从当前类的基类开始。
  friend Value makeSuper(const Value& self, const Value& currentClass);
  friend bool isSuper(const Value& value);
  friend Value superSelf(const Value& value);
  friend Value superClassOf(const Value& value);
  // `@staticmethod` / `@classmethod` 的包装值。它们本身不可调用，只在被
  // 访问时决定「要不要绑定接收者、绑定谁」——那正是描述符语义，必须由
  // VM 的类查找来做。被包装的函数复用 boundSelf 字段，Value 仍是 16 字节。
  friend Value makeStaticMethod(const Value& function);
  friend Value makeClassMethod(const Value& function);
  friend bool isStaticMethod(const Value& value);
  friend bool isClassMethod(const Value& value);
  friend Value staticMethodFunction(const Value& value);
  friend Value classMethodFunction(const Value& value);
  // 生成器值：与函数值同构（kObject + 专用标记表），载荷是函数号 +
  // 已绑好实参的局部量 + 暂停现场。
  friend Value makeGenerator(int functionIndex, List locals);
  friend Value makeCoroutine(int functionIndex, List locals);
  friend bool isCoroutineObject(const Value& value);
  friend bool isGenerator(const Value& value);
  friend int generatorFunctionIndex(const Value& value);
  friend int generatorPcOf(const Value& value);
  friend bool generatorDone(const Value& value);
  friend bool generatorStarted(const Value& value);
  friend const List& generatorLocals(const Value& value);
  friend const List& generatorStack(const Value& value);
  friend void setGeneratorState(const Value& value, int pc, List locals,
                                List stack, bool started, bool done);

 private:
  // ── 紧凑表示 ──
  //
  // 这个结构体被 VM 的每次 push/pop 拷贝。原型实测：96 字节时 300 万次
  // 循环 805 ms，16 字节时 1.11 ms。所以不能有 std::string 成员（它一个人
  // 就 32 字节），也不能有 std::shared_ptr（它一个人就 16 字节）。
  //
  // 布局（16 字节）：
  //   [0]  Type (uint8) + 7 字节填充
  //   [8]  int_ / float_ / host_ / heap_ 四选一的 8 字节载荷
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
    Host host_;
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

// ── 带方法的宿主对象 ──
//
// `threading.Lock()` 这类东西是「一块 pylib 私有的状态 + 一组方法」。核心 VM
// 不认识锁、不认识文件句柄，所以它只提供**转发机制**：
//
//   Value::object(data, &kLockMethods)     pylib 造对象
//   lock.acquire()                          VM 查表转发到 pylib 的函数
//
// 方法表与 host 指针都由 pylib 提供，`aipy` 里没有一行具体能力的代码。
struct ObjectData;

// 一个方法。`self` 是**接收者 Value 本身**（不只是 ObjectData）：
// `__enter__` 要返回自身（`with lock as l:` 的 l 就是锁），只给 ObjectData
// 就没法构造一个新的句柄指向同一个对象——用 host 重新包一个 Value 会产生
// **第二份 HeapData**，两份都负责 delete 同一个 host，结果是 double free。
using ObjectMethod = Value (*)(const Value& self, const Value* args, int count);

// 一张方法表。`methods` 是 {名字, 函数} 数组，以 name == nullptr 结尾。
// 用数组而不是 std::map：脚本里的对象方法只有个位数，线性比较更快、更省
// 体积，而且顺序稳定。
struct MethodEntry {
  const char* name;
  ObjectMethod fn;
};

struct MethodTable {
  const MethodEntry* methods;
};

// pylib 私有的对象状态。`host` 由 pylib 解释，`table` 决定能调哪些方法，
// `destroy` 在引用计数归零时调用（锁要在这里释放系统资源）。
struct ObjectData {
  void* host = nullptr;
  const MethodTable* table = nullptr;
  void (*destroy)(void* host) = nullptr;
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
// refs 是侵入式引用计数。原子类型是必需的——见文件头「计数必须是原子的」。
// 它仍是 4 字节，不改变 Value 的 16 字节预算。
struct HeapData : DictData {
  std::string text;        // kString
  List items;              // kList
  ObjectData object;       // kObject
  // 异常对象专用载荷（kObject 里的一种）。放进这里而不是 ObjectData：
  // ObjectData 是 pylib 的私有状态（void* host），而异常是语言语义，
  // VM 要直接读写这两个字段。
  std::string exceptionType;
  std::string exceptionMessage;
  // 函数值专用载荷（kObject 里的一种，判据是方法表指针）。同样放在这里：
  // pylib 的 host 装不下「函数号 + 捕获的变量」这类语言语义。
  //
  // 函数号是 Program::functions 的下标；捕获是闭包读到的外层变量值。
  // `Value` 仍只有 16 字节，载荷全在这块堆上。
  int functionIndex = -1;
  List captures;
  // 实例与类对象专用载荷（kObject 里的一种，判据同样是方法表指针）。
  //
  // 实例：`instanceOf` 是类对象的 Value（共享同一份堆载荷），`fields` 是
  // 实例自己的名字→值（保持插入序，与 Python 的 __dict__ 一致）。
  // 类对象：`className` 是类名，`bases` 是基类 Value 列表，`methods` 是
  // 类命名空间里的名字→值（函数值或普通属性），`fields` 不用。
  std::string className;
  List bases;
  List memberKeys;
  List memberValues;
  Value instanceOf;
  // 已绑定实例的方法值用：self（函数号复用 functionIndex）。
  Value boundSelf;
  // `super()` 的结果：self 与「当前类」（从它的基类开始查找）。
  Value superClass;
  // 生成器专用载荷（kObject 里的一种）。functionIndex 复用上面的字段。
  //
  // 暂存现场：pc、局部量、以及这一帧在值栈上的那一段。为什么连值栈也要存：
  // `yield` 可能出现在 for 循环体里，那时栈上正压着 [列表, 下标]，只存 pc
  // 会在恢复时丢掉循环状态，症状是嵌套 for + yield 每次都从头开始。
  int generatorPc = 0;
  bool generatorDone = false;
  bool generatorStarted = false;
  // 协程（`async def` 的产物）：与生成器同构，只差一个标记——
  // `await` 用它来区分「驱动协程」与「迭代生成器」。
  bool isCoroutineObject = false;
  List generatorLocals;
  List generatorStack;
  std::atomic<std::uint32_t> refs{1};  // 1 表示「刚创建，调用方已持有」
};

// ── 异常 ──
//
// 异常值是 `kObject` 的一种特例：载荷是类型名 + 消息，放在 HeapData 里。
// 为什么不加一个 `Type::kException`：`Value` 的 16 字节预算是铁律，每个新
// 类型都要占 tag 空间和心智负担，而异常本来就是一个「带两个字段的对象」。
//
// 用一个专用方法表当标记，`table == &kExceptionTable` 就是判据——不需要
// 额外标志位。
Value makeException(const std::string& type, const std::string& message);
// 是不是异常值。
bool isException(const Value& value);

// ── 内建类值（`int`、`str`、`ValueError` 这些类本身）──
//
// 与脚本类（kClassTable）的区别：内建类没有成员表、不能实例化脚本级
// 子类、也没有 MRO。它只需要三件事：
//   * 显示成 `<class 'int'>`（CPython 的默认 repr）；
//   * `is` 按名字判等——`type(1) is int` 必须是 True；
//   * `isinstance` 拿它当第二个参数做类型判断。
//
// 异常类型也走这套（`<class 'ValueError'>`），因为它们在脚本里的用法
// 完全一样：打印、`is` 比较、传给 isinstance。
Value makeBuiltinClass(const std::string& name);
bool isBuiltinClass(const Value& value);
std::string builtinClassName(const Value& value);

// ── 函数值（一等函数 / lambda）──
//
// `g = f` 与 `f = lambda x: x + 1` 要求函数能存进变量。函数值走 kObject
// 加专用方法表当标记，与异常同构——`Value` 仍是 16 字节。
//
// captures 是闭包捕获的外层变量值。顶层函数与不捕获的 lambda 传空列表：
// 它们的名字全在模块级全局槽里，不需要闭包。
Value makeFunction(int index, List captures = List());
bool isFunction(const Value& value);
// 函数在 Program::functions 里的下标；不是函数值时返回 -1。
int functionIndexOf(const Value& value);
// 捕获的变量值；不是函数值时返回空列表。
const List& functionCaptures(const Value& value);
// 取类型名 / 消息。不是异常时返回空串。
std::string exceptionType(const Value& value);
std::string exceptionMessage(const Value& value);

// 异常类之间的继承关系判定：`except Exception` 要能接住 ValueError。
//
// 为什么用一张固定的小表而不是让 pylib 注册：异常类属于**语言语义**
// （CPython 的异常层级也是解释器内置的），不是「具体能力」。放到 pylib
// 里会让「except 能不能接住」取决于链接了哪些库。
bool exceptionMatches(const std::string& thrownType,
                      const std::string& exceptType);

// `except X:` 里的 X 是不是一个已知的异常类名。编译期用它拒绝拼错的名字。
bool isExceptionTypeName(const std::string& name);

// CPython 在 TypeError 消息里用的类型名（"int" / "str" / "NoneType" …）。
std::string typeName(const Value& value);

// ── 待处理异常 ──
//
// 值层的函数（`add` / `at` / `toInteger` …）签名统一是「返回值」，没有
// 第二返回值可用。改成到处传 `std::string* error` 会动到上百个调用点，
// 而 CPython 的语义是「失败即抛出」——两者形状不同。
//
// 所以这里走一条线程局部的旁路：值层失败时 `raiseException(...)` 记下类型
// 与消息、仍然返回一个占位值；执行循环在每条可能失败的指令后问一次
// `takePendingException`，有就展开栈去找 except。这与 CPython 的
// 「异常保存在线程状态里」是同一个结构。
//
// 为什么是线程局部：两个线程各跑各的脚本，一个的失败不能让另一个看见。
void raiseException(const std::string& type, const std::string& message);
bool hasPendingException();
// 取出并清除。没有待处理异常时返回 false，且不动两个出参。
bool takePendingException(std::string* type, std::string* message);
void clearPendingException();

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
// 带步长的切片（支持负步长反向）。AOT 路线不用它，VM 的 SLICE 用它。
Value sliceStep(const Value& container, const Value& start, const Value& end,
                const Value& step);
// `x[start:end] = items` / `x[start:end:step] = items`。原地改列表，
// 没有返回值（失败时经 raiseException 旁路通知 VM）。
void assignSlice(const Value& container, const Value& start, const Value& end,
                 const Value& step, const Value& items);

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

// ── 字符串内建 ──
//
// 这些是 Python 语言本身的方法（`"abc".upper()`），不是界面能力，
// 所以在值层：AiPy 不装 pyui 也必须能用。
Value upper(const Value& text);
Value lower(const Value& text);
Value strip(const Value& text);
Value replace(const Value& text, const Value& from, const Value& to);
Value split(const Value& text, const Value& separator);
Value join(const Value& separator, const Value& items);
Value startsWith(const Value& text, const Value& prefix);
Value endsWith(const Value& text, const Value& suffix);
Value findIn(const Value& text, const Value& needle);
Value countIn(const Value& text, const Value& needle);
// 只支持 {} 与 {0}：完整的格式迷你语言不值得为脚本场景实现
Value formatText(const Value& text, const Value& args);
Value textOfChar(const Value& codepoint);

// ── 其它内建 ──
Value minimum(const Value& a, const Value& b);
Value maximum(const Value& a, const Value& b);

// ── 排序 / 拼接（sorted / reversed / zip / enumerate）──
//
// 都在值层而不是内建里，因为它们是纯容器操作，与具体能力无关。
// 与 `list.sort()` 共用同一套比较规则（见 sortList）。
Value sortedCopy(const Value& items, const Value& key, bool reverse);
Value reversedCopy(const Value& items);
Value zipOf(const Value& left, const Value& right);
Value enumerateOf(const Value& items, const Value& start);

// ── 编码 / 进制（chr / ord / hex / bin / oct）──
Value chrOf(const Value& codepoint);
Value ordOf(const Value& text);
Value intToBase(const Value& number, int base);

// ── 聚合（any / all）──
Value anyOf(const Value& items);
Value allOf(const Value& items);

// CPython 的 repr()：字符串带引号、容器递归。与 toString 的区别就在这里。
std::string reprOf(const Value& value);

// dict(pairs) / tuple(iterable)
Value dictFromPairs(const Value& pairs);
// 字符串 → 数字，只认整串都是数字（"12abc" 返回 None 而不是 12）
// 文本 → 整数；整串必须是整数，失败返回 None（调用方抛 ValueError）。
Value parseInteger(const Value& text);
// 文本 → 浮点；整串必须是数字，失败返回 None。
Value parseFloat(const Value& text);
// int(text, base)：base 为 2..36。文本非法时抛 ValueError，与 CPython 一致。
Value integerFromBase(const Value& text, long long base);
// 码点 → UTF-8 字符
Value charFromCode(const Value& codepoint);
// 调试输出。默认写 stderr；宿主可以装自己的 sink（GUI 程序里 stderr
// 没有窗口可写，Windows 上要用 OutputDebugString 才看得见）。
// 用钩子而不是 #ifdef：语言层不该知道任何平台。
using LogSink = std::function<void(const std::string&)>;
void setLogSink(LogSink sink);
void log(const Value& value);

// 原样写出，**不追加换行**。log() 的宿主实现会补一个 '\n'，而
// `print(x, end="")` 要求结尾完全可控，所以另开一条通道。
// 不设 sink 时写 stderr，与 log 的默认一致。
using WriteSink = std::function<void(const std::string&)>;
void setWriteSink(WriteSink sink);
void writeRaw(const std::string& text);

// 诊断输出：pylib 的失败原因走这里，**不走 log**。
//
// 为什么必须与 `log` 分开：`print` 用的就是 log，而 launcher 把 log 接到
// stdout（应用产物要与 `python script.py` 一样只看程序输出）。pylib 的
// 「读不到 a.txt: 文件或目录不存在」如果也走 log，就会混进程序输出——
// 脚本自己 print 的内容和库的诊断分不开，重定向到文件时更糟。
//
// 默认写 stderr（命令行下看得见），GUI 宿主可以换成 OutputDebugString。
using DiagnosticSink = std::function<void(const std::string&)>;
void setDiagnosticSink(DiagnosticSink sink);
void diagnose(const std::string& text);


}
