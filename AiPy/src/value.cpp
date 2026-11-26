#include "aipy/value.h"

#include "aipy/exceptions.h"
#include "aipy/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aipy {


namespace {

const std::string kEmptyString;
const List kEmptyList;

// 异常值用这张方法表当标记：判据是「表指针是不是它」，所以必须在
// 所有用到判定的地方（isException、toString）之前可见。
const MethodEntry kNoMethods[] = {{nullptr, nullptr}};
const MethodTable kExceptionTable = {kNoMethods};

// 数字提升：只要有一边是 float，结果就是 float。Python 的 / 永远返回 float，
// 但 + - * % 保持 int。这里不区分来源，由调用点决定走哪个函数。
double asNumber(const Value& v) { return v.toFloat(); }

bool bothInt(const Value& a, const Value& b) {
  return a.type() == Value::Type::kInt && b.type() == Value::Type::kInt;
}

// 整数算术分支的判定：bool 在 Python 里是 int 的子类型，
// True + True 应当得到整数 2 而不是浮点 2.0（后者 str 出来是 "2.0"）。
bool bothIntOrBool(const Value& a, const Value& b) {
  const auto isIntLike = [](const Value& v) {
    return v.type() == Value::Type::kInt || v.type() == Value::Type::kBool;
  };
  return isIntLike(a) && isIntLike(b);
}

long long intOf(const Value& v) { return v.toInt(); }

// Python 里 bool 是 int 的子类型：True == 1、False == 0、True + True == 2。
// 因此凡是要「按数值处理」的地方都要把 kBool 算进来，否则
// `if checked == 1` 在解释器里为真、编译后为假。
bool asNumberLike(const Value& v) { return v.isNumber() || v.type() == Value::Type::kBool; }

double numberOf(const Value& v) {
  return v.type() == Value::Type::kBool ? (v.toBool() ? 1.0 : 0.0) : v.toFloat();
}

// 浮点转文本。
//
// CPython 的 str(float) 是「最短往返表示」：把结果解析回来必须得到同一个
// double，且在满足这一条件的写法里有效数字最少。所以 str(0.1 + 0.2) 是
// "0.30000000000000004"，而不是 "0.3"——用 %.10g 丢掉末几位之后，那已经是
// 另一个数了，脚本里再做比较或拼进文本都会与 CPython 不一致。
//
// 两件事必须分开做，不能靠一个 %g 摆平：
//   1. 定有效位数：从 1 位往上试，第一个能往返的位数就是它。
//   2. 排版：CPython 只在 decpt <= -4 或 decpt > 16 时用指数记法
//      （decpt 是「小数点前有几位」）。而 %g 的阈值是「指数 >= 精度」，
//      于是 str(1e15) 会被 %g 写成 "1e+15"，CPython 写的是
//      "1000000000000000.0"。两者必须一致，所以第 2 步手写。
std::string formatFloat(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";

  // 负零：-0.0 == 0.0 为真，直接拿绝对值去测往返会把符号丢掉，
  // 而 CPython 的 str(-0.0) 是 "-0.0"。
  const bool negative = std::signbit(value);
  if (value == 0.0) return negative ? "-0.0" : "0.0";
  const double magnitude = negative ? -value : value;

  // 用 %e 而不是 %g 取有效数字：%e 的小数部分长度就等于有效位数，
  // 不会因为数值大小而切换到指数记法，解析起来只有一种形状。
  char buffer[64];
  int significant = 17;
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, magnitude);
    if (std::strtod(buffer, nullptr) == magnitude) {
      significant = precision;
      break;
    }
  }
  std::snprintf(buffer, sizeof(buffer), "%.*e", significant - 1, magnitude);

  // 拆成「数字串 + 十进制指数」。去掉尾随 0：1e15 的 %e 是 "1.000000e+15"，
  // 有效数字串应当是 "1"，多出来的 0 会让 decpt 算错。
  std::string text(buffer);
  const size_t ePos = text.find('e');
  std::string mantissa = text.substr(0, ePos);
  const int exponent = std::atoi(text.c_str() + ePos + 1);

  const size_t dot = mantissa.find('.');
  std::string digits = dot == std::string::npos ? mantissa : mantissa.substr(0, dot);
  if (dot != std::string::npos) digits += mantissa.substr(dot + 1);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

  // decpt = 小数点前的位数。1e15 的 digits 是 "1"、exponent 是 15，
  // 所以小数点要落在第 16 位之后 → decpt = 16。
  const int decpt = exponent + 1;
  std::string out = negative ? "-" : "";

  if (decpt <= -4 || decpt > 16) {
    out += digits[0];
    if (digits.size() > 1) out += "." + digits.substr(1);
    out += 'e';
    const int magnitudeExponent = decpt - 1;
    out += magnitudeExponent < 0 ? '-' : '+';
    std::string exponentText = std::to_string(magnitudeExponent < 0 ? -magnitudeExponent
                                                                    : magnitudeExponent);
    // CPython 的指数至少两位：1e+16、1e-05
    if (exponentText.size() < 2) exponentText.insert(0, "0", 2 - exponentText.size());
    out += exponentText;
    return out;
  }

  if (decpt <= 0) {
    out += "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
  } else if (decpt >= static_cast<int>(digits.size())) {
    out += digits + std::string(static_cast<size_t>(decpt) - digits.size(), '0') + ".0";
  } else {
    out += digits.substr(0, static_cast<size_t>(decpt)) + "." +
           digits.substr(static_cast<size_t>(decpt));
  }
  return out;
}

// 列表/字符串的索引归一化：负数从尾部算，越界返回 false。
bool normalizeIndex(long long index, size_t count, size_t& out) {
  if (index < 0) index += static_cast<long long>(count);
  if (index < 0 || index >= static_cast<long long>(count)) return false;
  out = static_cast<size_t>(index);
  return true;
}

// 比较只在同类里做：数字之间按数值，字符串之间按字节序（UTF-8 的顺序
// 与码点顺序一致，所以中文排序也是对的），其余一律按类型序。
int compareValues(const Value& a, const Value& b) {
  if (asNumberLike(a) && asNumberLike(b)) {
    const double x = numberOf(a);
    const double y = numberOf(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.type() == Value::Type::kString && b.type() == Value::Type::kString) {
    const int result = a.asString().compare(b.asString());
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
  }
  if (a.type() == b.type()) return 0;
  return static_cast<int>(a.type()) < static_cast<int>(b.type()) ? -1 : 1;
}

// 追加一个元素的文本形式（列表 repr 用）。
void appendRepr(std::string& out, const Value& value);

std::string listRepr(const List& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ", ";
    appendRepr(out, items[i]);
  }
  out += "]";
  return out;
}

// Python 的 repr 会给字符串加引号，而 str() 不加。列表与字典的显示
// 走 repr 规则，否则嵌套结构会输出成一坨看不出层级与类型的文本。
void appendRepr(std::string& out, const Value& value) {
  if (value.type() == Value::Type::kString) {
    out += "'" + value.asString() + "'";
    return;
  }
  out += value.toString();
}

std::string dictRepr(const List& keys, const List& values) {
  std::string out = "{";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) out += ", ";
    appendRepr(out, keys[i]);
    out += ": ";
    appendRepr(out, values[i]);
  }
  out += "}";
  return out;
}

}  // namespace

// ── 堆载荷与引用计数 ──
//
// 三个辅助函数集中放这里，不让别的函数直接碰 refs：引用计数出错的表现是
// 「偶尔崩溃 / 内存泄漏」，症状离原因很远，只留一处入口便于核对。

namespace {

// 新建一块堆载荷，计数为 1（返回它的那个 Value 持有这一份）。
HeapData* makeHeap() { return new HeapData(); }

// 计数 +1。空指针直接忽略：None/数字没有堆载荷，调用方不必先判断。
//
// relaxed 足够：增加只要求「不丢计数」，不建立任何跨线程先后关系。载荷
// 本身早已构造完毕，读到它的句柄就意味着构造已完成。
void retain(HeapData* heap) {
  if (heap != nullptr) heap->refs.fetch_add(1, std::memory_order_relaxed);
}

// 计数 -1，归零就释放。列表/字典里的元素是 Value，它们的析构会递归
// 释放子节点——这一步与 CPython 的引用计数回收时机一致。
//
// acq_rel 是必需的：最后一个持有者的释放必须看见其他人对载荷的全部写入。
// relaxed 会让「A 线程写完 list 内容、B 线程释放最后一份」读到旧内容。
void release(HeapData* heap) {
  if (heap == nullptr) return;
  if (heap->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  // kObject 的 host 由 pylib 分配，也必须由它释放（锁要解锁、句柄要关）。
  // 放在 delete 之前：destroy 里可能还要读 host 自己。
  if (heap->object.destroy != nullptr && heap->object.host != nullptr) {
    heap->object.destroy(heap->object.host);
    heap->object.host = nullptr;
  }
  delete heap;
}

// 哪些类型把载荷放在堆上（需要增减引用计数）。
//
// 集中成一个函数而不是到处写 `type_ == kString || ...`：新增一种堆类型时
// 漏改一处的症状是「内存泄漏或双重释放」，而这两处判断分散在拷贝、赋值、
// 析构三个函数里，很容易漏。
inline bool isHeapType(Value::Type type) {
  return type == Value::Type::kString || type == Value::Type::kList ||
         type == Value::Type::kDict || type == Value::Type::kObject ||
         type == Value::Type::kDeleted || type == Value::Type::kTuple;
}

}  // namespace

// ── 拷贝/移动/析构 ──
//
// 必须手写：默认版本对 union 里的 heap_ 一无所知，既不会 retain 也不会
// release，结果是「拷贝之后两个句柄共享载荷，但计数还是 1」——先析构的
// 那个会把载荷删掉，另一个就成了悬垂指针。

Value::Value(const Value& other) : type_(other.type_) {
  switch (type_) {
    case Type::kFloat:
      float_ = other.float_;
      break;
    case Type::kHost:
      host_ = other.host_;
      break;
    case Type::kString:
    case Type::kList:
    case Type::kTuple:
    case Type::kDict:
    case Type::kObject:
    case Type::kDeleted:
      heap_ = other.heap_;
      retain(heap_);
      break;
    case Type::kNone:
    case Type::kBool:
    case Type::kInt:
      int_ = other.int_;
      break;
  }
}

Value::Value(Value&& other) noexcept : type_(other.type_) {
  // 移动是「偷走指针」：计数不增不减，把源置成 None 让它的析构不来碰。
  // 这里同样按类型搬，不整体拷 union。
  switch (type_) {
    case Type::kFloat:
      float_ = other.float_;
      break;
    case Type::kHost:
      host_ = other.host_;
      break;
    case Type::kString:
    case Type::kList:
    case Type::kTuple:
    case Type::kDict:
    case Type::kObject:
    case Type::kDeleted:
      heap_ = other.heap_;
      break;
    case Type::kNone:
    case Type::kBool:
    case Type::kInt:
      int_ = other.int_;
      break;
  }
  other.type_ = Type::kNone;
  other.int_ = 0;
}

Value& Value::operator=(const Value& other) {
  if (this == &other) return *this;  // 自赋值：先 release 会把自己的载荷删掉
  // 先 retain 再 release，避免 `a = a 的子元素` 时中间态归零。
  //
  // 用 isHeapType 而不是逐个列类型：漏列一种的症状是「拷贝赋值的那个
  // 句柄不增加计数」，于是先析构的一方把载荷删掉，另一个变成悬垂指针
  // ——堆损坏、随机崩溃。kObject 就踩过这个漏列（它比 kString 晚加）。
  if (isHeapType(other.type_)) {
    retain(other.heap_);
  }
  if (isHeapType(type_)) {
    release(heap_);
  }
  type_ = other.type_;
  switch (type_) {
    case Type::kFloat:
      float_ = other.float_;
      break;
    case Type::kHost:
      host_ = other.host_;
      break;
    case Type::kString:
    case Type::kList:
    case Type::kTuple:
    case Type::kDict:
    case Type::kObject:
    case Type::kDeleted:
      heap_ = other.heap_;
      break;
    case Type::kNone:
    case Type::kBool:
    case Type::kInt:
      int_ = other.int_;
      break;
  }
  return *this;
}

Value& Value::operator=(Value&& other) noexcept {
  if (this == &other) return *this;
  if (isHeapType(type_)) {
    release(heap_);
  }
  type_ = other.type_;
  switch (type_) {
    case Type::kFloat:
      float_ = other.float_;
      break;
    case Type::kHost:
      host_ = other.host_;
      break;
    case Type::kString:
    case Type::kList:
    case Type::kTuple:
    case Type::kDict:
    case Type::kObject:
    case Type::kDeleted:
      heap_ = other.heap_;
      break;
    case Type::kNone:
    case Type::kBool:
    case Type::kInt:
      int_ = other.int_;
      break;
  }
  other.type_ = Type::kNone;
  other.int_ = 0;
  return *this;
}

Value::~Value() {
  if (isHeapType(type_)) {
    release(heap_);
  }
}

// ── 构造 ──

Value::Value(const char* value) : type_(Type::kString) {
  heap_ = makeHeap();
  if (value != nullptr) heap_->text = value;
}

Value::Value(std::string value) : type_(Type::kString) {
  heap_ = makeHeap();
  heap_->text = std::move(value);
}

Value::Value(List value) : type_(Type::kList) {
  heap_ = makeHeap();
  heap_->items = std::move(value);
}

Value Value::list(List items) {
  Value out;
  out.type_ = Type::kList;
  out.heap_ = makeHeap();
  out.heap_->items = std::move(items);
  return out;
}

Value Value::tuple(List items) {
  Value out;
  out.type_ = Type::kTuple;
  out.heap_ = makeHeap();
  out.heap_->items = std::move(items);
  return out;
}

Value Value::deleted(std::string name) {
  Value out;
  out.type_ = Type::kDeleted;
  out.heap_ = makeHeap();
  out.heap_->text = std::move(name);
  return out;
}

std::string Value::deletedName() const {
  if (type_ != Type::kDeleted || heap_ == nullptr) return std::string();
  return heap_->text;
}

Value Value::dict(const List& keys, const List& values) {
  Value out;
  out.type_ = Type::kDict;
  out.heap_ = makeHeap();
  out.heap_->keys = keys;
  out.heap_->values = values;
  return out;
}

Value Value::object(void* host, const MethodTable* table,
                    void (*destroy)(void* host)) {
  Value out;
  out.type_ = Type::kObject;
  out.heap_ = makeHeap();
  out.heap_->object.host = host;
  out.heap_->object.table = table;
  out.heap_->object.destroy = destroy;
  return out;
}

ObjectData* Value::asObject() const {
  if (type_ == Type::kObject && heap_ != nullptr) return &heap_->object;
  return nullptr;
}

// ── 取值 ──

const List& Value::asList() const {
  if ((type_ == Type::kList || type_ == Type::kTuple) && heap_ != nullptr) {
    return heap_->items;
  }
  return kEmptyList;
}

const List& Value::dictKeys() const {
  if (type_ == Type::kDict && heap_ != nullptr) return heap_->keys;
  return kEmptyList;
}

const List& Value::dictValues() const {
  if (type_ == Type::kDict && heap_ != nullptr) return heap_->values;
  return kEmptyList;
}

const std::string& Value::asString() const {
  return type_ == Type::kString && heap_ != nullptr ? heap_->text : kEmptyString;
}

List* Value::mutableList() const {
  if (type_ != Type::kList || heap_ == nullptr) return nullptr;
  return &heap_->items;
}

DictData* Value::mutableDict() const {
  if (type_ != Type::kDict || heap_ == nullptr) return nullptr;
  // HeapData 继承自 DictData，所以这里是合法的向上转型，不是 reinterpret。
  // 返回真实指针而不是副本：调用方会往里写（字典的 insertion order 与
  // 原地修改都要能落回去），给副本就等于把写操作吞掉。
  return static_cast<DictData*>(heap_);
}

long long Value::dictIndex(const Value& key) const {
  if (type_ != Type::kDict || heap_ == nullptr) return -1;
  for (size_t i = 0; i < heap_->keys.size(); ++i) {
    if (eq(heap_->keys[i], key).toBool()) return static_cast<long long>(i);
  }
  return -1;
}

bool Value::toBool() const {
  switch (type_) {
    case Type::kNone:
      return false;
    case Type::kBool:
    case Type::kInt:
      return int_ != 0;
    case Type::kFloat:
      return float_ != 0.0;
    case Type::kString:
      return !asString().empty();
    case Type::kList:
      return heap_ != nullptr && !heap_->items.empty();
    case Type::kTuple:
      return heap_ != nullptr && !heap_->items.empty();
    case Type::kDict:
      return heap_ != nullptr && !heap_->keys.empty();
    case Type::kHost:
      return host_ != nullptr;
    case Type::kObject:
      // 对象总是真：脚本写 `if lock:` 想知道的不是「锁里存了什么」，
      // 而是「这个对象存在」。返回 host 是否非空更贴近资源已就绪的含义。
      return heap_ != nullptr && heap_->object.host != nullptr;
  }
  return false;
}

long long Value::toInt() const {
  switch (type_) {
    case Type::kBool:
    case Type::kInt:
      return int_;
    case Type::kFloat:
      return static_cast<long long>(float_);
    case Type::kString: {
      // int("12") 在脚本里很常见（读输入框再算）。解析失败给 0 而不是崩，
      // 与「输入框里不是数字」这个日常情形对应；调用方要区分就用 get_text。
      const std::string& text = asString();
      if (text.empty()) return 0;
      return std::strtoll(text.c_str(), nullptr, 10);
    }
    case Type::kNone:
    case Type::kList:
    case Type::kDict:
    case Type::kHost:
    case Type::kObject:
      return 0;
  }
  return 0;
}

double Value::toFloat() const {
  switch (type_) {
    case Type::kBool:
    case Type::kInt:
      return static_cast<double>(int_);
    case Type::kFloat:
      return float_;
    case Type::kString: {
      const std::string& text = asString();
      if (text.empty()) return 0.0;
      return std::strtod(text.c_str(), nullptr);
    }
    case Type::kNone:
    case Type::kList:
    case Type::kDict:
    case Type::kHost:
    case Type::kObject:
      return 0.0;
  }
  return 0.0;
}

std::string Value::toString() const {
  switch (type_) {
    case Type::kNone:
      return "None";
    case Type::kBool:
      return int_ != 0 ? "True" : "False";
    case Type::kInt:
      return std::to_string(int_);
    case Type::kFloat:
      return formatFloat(float_);
    case Type::kString:
      return asString();
    case Type::kList:
      return listRepr(asList());
    case Type::kTuple: {
      // CPython 的元组 repr：单元素带尾逗号 `(1,)`，空元组 `()`。
      std::string out = "(";
      const List& items = asList();
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ", ";
        appendRepr(out, items[i]);
      }
      if (items.size() == 1) out += ",";
      out += ")";
      return out;
    }
    case Type::kDict:
      return dictRepr(dictKeys(), dictValues());
    case Type::kHost:
      return "<view>";
    case Type::kObject:
      // 异常对象按 CPython 的 str(exc) 显示消息；没有消息时显示类型名。
      // 其它对象保持不透明：与 Python 的 <threading.Lock object at 0x...>
      // 同一个意图，不去猜 pylib 私有状态的细节。
      if (heap_ != nullptr && heap_->object.table == &kExceptionTable) {
        // CPython 的 str(exc) 就是 args 的内容：没有消息时是空串，不是类型名。
        return heap_->exceptionMessage;
      }
      return "<object>";
  }
  return std::string();
}

// ── 异常 ──

Value makeException(const std::string& type, const std::string& message) {
  Value out;
  out.type_ = Value::Type::kObject;
  out.heap_ = makeHeap();
  out.heap_->object.table = &kExceptionTable;
  out.heap_->exceptionType = type;
  out.heap_->exceptionMessage = message;
  return out;
}

bool isException(const Value& value) {
  const ObjectData* data = value.asObject();
  return data != nullptr && data->table == &kExceptionTable;
}

std::string exceptionType(const Value& value) {
  if (!isException(value)) return std::string();
  return value.heap_->exceptionType;
}

std::string exceptionMessage(const Value& value) {
  if (!isException(value)) return std::string();
  return value.heap_->exceptionMessage;
}

bool exceptionMatches(const std::string& thrownType,
                      const std::string& exceptType) {
  // 沿「子类 → 父类」上溯。层数上限兜底：表写错成环时宁可判不匹配，
  // 也不要在 except 匹配里死循环——那会把脚本错误变成程序卡死。
  std::string current = thrownType;
  for (int depth = 0; depth < 64; ++depth) {
    if (current == exceptType) return true;
    const char* parent = exceptionParentOf(current);
    if (parent == nullptr) return false;
    current = parent;
  }
  return false;
}

bool isExceptionTypeName(const std::string& name) {
  return isExceptionClassName(name);
}

// CPython 在 TypeError 消息里用的类型名。
std::string typeName(const Value& value) {
  switch (value.type()) {
    case Value::Type::kNone: return "NoneType";
    case Value::Type::kBool: return "bool";
    case Value::Type::kInt: return "int";
    case Value::Type::kFloat: return "float";
    case Value::Type::kString: return "str";
    case Value::Type::kList: return "list";
    case Value::Type::kTuple: return "tuple";
    case Value::Type::kDict: return "dict";
    case Value::Type::kHost: return "host";
    case Value::Type::kObject: return "object";
    case Value::Type::kDeleted: return "NoneType";
  }
  return "object";
}

// ── 待处理异常 ──
//
// 见 value.h 的解释：值层统一「返回值」，失败靠这条旁路通知执行循环。
// 不用 `std::exception_ptr`：这里只传两个字符串，没有 C++ 异常对象要保全。
namespace {
thread_local bool g_hasPending = false;
thread_local std::string g_pendingType;
thread_local std::string g_pendingMessage;
}  // namespace

void raiseException(const std::string& type, const std::string& message) {
  // 只为 AI 的调用点省一次判空。已经有一个待处理异常时不覆盖：
  // 那说明失败正在向外传播，后来者只是它的副作用。
  if (g_hasPending) return;
  g_hasPending = true;
  g_pendingType = type;
  g_pendingMessage = message;
}

bool hasPendingException() { return g_hasPending; }

bool takePendingException(std::string* type, std::string* message) {
  if (!g_hasPending) return false;
  if (type != nullptr) *type = std::move(g_pendingType);
  if (message != nullptr) *message = std::move(g_pendingMessage);
  clearPendingException();
  return true;
}

void clearPendingException() {
  g_hasPending = false;
  g_pendingType.clear();
  g_pendingMessage.clear();
}

// ── 算术 ──

Value add(const Value& a, const Value& b) {
  // bool 也算数字（Python 里 True + True == 2）；bothInt 只认 kInt，
  // 所以 True + True 会走浮点分支得到 2.0——数值相等，类型不同，
  // 而 str(2.0) 是 "2.0"。用 asNumberLike 把它归到整数分支。
  if (asNumberLike(a) && asNumberLike(b)) {
    if (bothIntOrBool(a, b)) return Value(static_cast<long long>(numberOf(a) + numberOf(b)));
    return Value(numberOf(a) + numberOf(b));
  }
  if (a.type() == Value::Type::kString && b.type() == Value::Type::kString) {
    return Value(a.asString() + b.asString());
  }
  if (a.isList() && b.isList()) {
    List out = a.asList();
    const List& right = b.asList();
    out.insert(out.end(), right.begin(), right.end());
    return Value::list(std::move(out));
  }
  if (a.isTuple() && b.isTuple()) {
    List out = a.asList();
    const List& right = b.asList();
    out.insert(out.end(), right.begin(), right.end());
    return Value::tuple(std::move(out));
  }
  // 类型不匹配一律抛 TypeError，文案照抄 CPython：静默把数字转成字符串
  // 会让 `1 + "x"` 在迁移回 CPython 时突然崩，而兼容性是本项目的全部意义。
  if (a.type() == Value::Type::kString) {
    raiseException("TypeError", "can only concatenate str (not \"" +
                                    typeName(b) + "\") to str");
    return Value();
  }
  if (a.isList()) {
    raiseException("TypeError", "can only concatenate list (not \"" +
                                    typeName(b) + "\") to list");
    return Value();
  }
  raiseException("TypeError", "unsupported operand type(s) for +: '" +
                                  typeName(a) + "' and '" + typeName(b) + "'");
  return Value();
}

Value sub(const Value& a, const Value& b) {
  if (!asNumberLike(a) || !asNumberLike(b)) {
    raiseException("TypeError", "unsupported operand type(s) for -: '" +
                                    typeName(a) + "' and '" + typeName(b) + "'");
    return Value();
  }
  if (bothIntOrBool(a, b)) {
    return Value(static_cast<long long>(numberOf(a) - numberOf(b)));
  }
  return Value(numberOf(a) - numberOf(b));
}

Value mul(const Value& a, const Value& b) {
  if (asNumberLike(a) && asNumberLike(b)) {
    if (bothIntOrBool(a, b)) return Value(static_cast<long long>(numberOf(a) * numberOf(b)));
    return Value(numberOf(a) * numberOf(b));
  }
  // "=" * 20 用来画分隔线，列表重复同理
  if (a.type() == Value::Type::kString && b.isNumber()) {
    const long long count = b.toInt();
    if (count <= 0) return Value(std::string());
    std::string out;
    for (long long i = 0; i < count; ++i) out += a.asString();
    return Value(std::move(out));
  }
  if (a.isNumber() && b.type() == Value::Type::kString) return mul(b, a);
  if (a.isList() && b.isNumber()) {
    List out;
    const List& source = a.asList();
    for (long long i = 0; i < b.toInt(); ++i) out.insert(out.end(), source.begin(), source.end());
    return Value::list(std::move(out));
  }
  if (a.isTuple() && b.isNumber()) {
    List out;
    const List& source = a.asList();
    for (long long i = 0; i < b.toInt(); ++i) out.insert(out.end(), source.begin(), source.end());
    return Value::tuple(std::move(out));
  }
  if (a.isNumber() && b.isList()) return mul(b, a);
  if (a.isNumber() && b.isTuple()) return mul(b, a);
  raiseException("TypeError", "unsupported operand type(s) for *: '" +
                                  typeName(a) + "' and '" + typeName(b) + "'");
  return Value();
}

Value div(const Value& a, const Value& b) {
  if (!asNumberLike(a) || !asNumberLike(b)) {
    raiseException("TypeError", "unsupported operand type(s) for /: '" +
                                    typeName(a) + "' and '" + typeName(b) + "'");
    return Value();
  }
  const double divisor = asNumber(b);
  if (divisor == 0.0) {
    // 整数除法与浮点除法的文案不同，与 CPython 一致。
    raiseException("ZeroDivisionError",
                   bothIntOrBool(a, b) ? "division by zero"
                                       : "float division by zero");
    return Value();
  }
  return Value(asNumber(a) / divisor);
}

Value mod(const Value& a, const Value& b) {
  if (!asNumberLike(a) || !asNumberLike(b)) {
    raiseException("TypeError", "unsupported operand type(s) for %: '" +
                                    typeName(a) + "' and '" + typeName(b) + "'");
    return Value();
  }
  if (bothInt(a, b)) {
    const long long divisor = intOf(b);
    if (divisor == 0) {
      raiseException("ZeroDivisionError", "integer modulo by zero");
      return Value();
    }
    // Python 的 % 结果符号跟除数走（-7 % 3 == 2），不是 C 的 -1。
    // 时钟、取模取余这类场景几乎都依赖这一点。
    long long result = intOf(a) % divisor;
    if (result != 0 && ((result < 0) != (divisor < 0))) result += divisor;
    return Value(result);
  }
  const double divisor = asNumber(b);
  if (divisor == 0.0) {
    raiseException("ZeroDivisionError", "float modulo");
    return Value();
  }
  double result = std::fmod(asNumber(a), divisor);
  if (result != 0.0 && ((result < 0.0) != (divisor < 0.0))) result += divisor;
  return Value(result);
}

Value neg(const Value& a) {
  if (!asNumberLike(a)) {
    raiseException("TypeError", "bad operand type for unary -: '" +
                                    typeName(a) + "'");
    return Value();
  }
  if (a.type() == Value::Type::kInt) return Value(-intOf(a));
  return Value(-asNumber(a));
}

Value invert(const Value& a) {
  if (a.type() != Value::Type::kInt && a.type() != Value::Type::kBool) {
    raiseException("TypeError", "bad operand type for unary ~: '" +
                                    typeName(a) + "'");
    return Value();
  }
  return Value(~a.toInt());
}

// ── 比较 ──

Value eq(const Value& a, const Value& b) {
  if (a.type() == Value::Type::kNone || b.type() == Value::Type::kNone) {
    return Value(a.isNone() && b.isNone());
  }
  // Python 里 True == 1、0 == False：bool 是一种整数子类型。
  // 所以「是不是数字」要把 kBool 也算进来，否则脚本里
  // `if checked == 1` 这类写法在解释器与编译产物上结果不同。
  if (asNumberLike(a) && asNumberLike(b)) {
    return Value(numberOf(a) == numberOf(b));
  }
  if (a.type() != b.type()) return Value(false);
  if (a.isList() || a.isTuple()) {
    const List& left = a.asList();
    const List& right = b.asList();
    if (left.size() != right.size()) return Value(false);
    for (size_t i = 0; i < left.size(); ++i) {
      if (!eq(left[i], right[i]).toBool()) return Value(false);
    }
    return Value(true);
  }
  if (a.isDict()) {
    // 字典比较不看顺序：{'a':1,'b':2} == {'b':2,'a':1}
    const List& keys = a.dictKeys();
    if (keys.size() != b.dictKeys().size()) return Value(false);
    for (const Value& key : keys) {
      const long long index = b.dictIndex(key);
      if (index < 0) return Value(false);
      if (!eq(a.dictValues()[static_cast<size_t>(a.dictIndex(key))],
              b.dictValues()[static_cast<size_t>(index)])
               .toBool()) {
        return Value(false);
      }
    }
    return Value(true);
  }
  return Value(compareValues(a, b) == 0);
}

Value ne(const Value& a, const Value& b) { return lnot(eq(a, b)); }
Value lt(const Value& a, const Value& b) { return Value(compareValues(a, b) < 0); }
Value le(const Value& a, const Value& b) { return Value(compareValues(a, b) <= 0); }
Value gt(const Value& a, const Value& b) { return Value(compareValues(a, b) > 0); }
Value ge(const Value& a, const Value& b) { return Value(compareValues(a, b) >= 0); }

// ── 逻辑与位 ──

Value land(const Value& a, const Value& b) { return a.toBool() ? b : a; }
Value lor(const Value& a, const Value& b) { return a.toBool() ? a : b; }
Value lnot(const Value& a) { return Value(!a.toBool()); }

Value landLazy(const Value& a, const std::function<Value()>& b) {
  return a.toBool() ? b() : a;
}
Value lorLazy(const Value& a, const std::function<Value()>& b) {
  return a.toBool() ? a : b();
}
Value iff(const Value& condition, const std::function<Value()>& ifTrue,
          const std::function<Value()>& ifFalse) {
  return condition.toBool() ? ifTrue() : ifFalse();
}

Value contains(const Value& container, const Value& item) {
  if (container.type() == Value::Type::kString) {
    // 子串查找而不是字符查找：脚本里 `"a" in text` 的直觉就是子串
    return Value(container.asString().find(item.toString()) != std::string::npos);
  }
  if (container.isDict()) return Value(container.dictIndex(item) >= 0);
  for (const Value& element : container.asList()) {
    if (eq(element, item).toBool()) return Value(true);
  }
  return Value(false);
}

// 位运算只接受整数（Python 里 bool 是 int 子类型，所以也算）。
// 不做 toInt() 的静默截断：`3.7 & 1` 在 CPython 是 TypeError，
// 静默取整会让脚本迁移回去时行为突变。
namespace {

bool isIntegerLike(const Value& v) {
  return v.type() == Value::Type::kInt || v.type() == Value::Type::kBool;
}

Value bitError(const char* op, const Value& a, const Value& b) {
  raiseException("TypeError", std::string("unsupported operand type(s) for ") +
                                  op + ": '" + typeName(a) + "' and '" +
                                  typeName(b) + "'");
  return Value();
}

}  // namespace

Value bitAnd(const Value& a, const Value& b) {
  if (!isIntegerLike(a) || !isIntegerLike(b)) return bitError("&", a, b);
  return Value(a.toInt() & b.toInt());
}
Value bitOr(const Value& a, const Value& b) {
  if (!isIntegerLike(a) || !isIntegerLike(b)) return bitError("|", a, b);
  return Value(a.toInt() | b.toInt());
}
Value bitXor(const Value& a, const Value& b) {
  if (!isIntegerLike(a) || !isIntegerLike(b)) return bitError("^", a, b);
  return Value(a.toInt() ^ b.toInt());
}
Value shiftLeft(const Value& a, const Value& b) {
  if (!isIntegerLike(a) || !isIntegerLike(b)) return bitError("<<", a, b);
  if (b.toInt() < 0) {
    raiseException("ValueError", "negative shift count");
    return Value();
  }
  return Value(a.toInt() << b.toInt());
}
Value shiftRight(const Value& a, const Value& b) {
  if (!isIntegerLike(a) || !isIntegerLike(b)) return bitError(">>", a, b);
  if (b.toInt() < 0) {
    raiseException("ValueError", "negative shift count");
    return Value();
  }
  return Value(a.toInt() >> b.toInt());
}

// ── 内建 ──

Value toNumber(const Value& v) { return Value(v.toFloat()); }
Value toInteger(const Value& v) { return Value(v.toInt()); }
Value toText(const Value& v) { return Value(v.toString()); }
Value boolOf(const Value& v) { return Value(v.toBool()); }

Value length(const Value& v) {
  switch (v.type()) {
    case Value::Type::kString:
      // 按码点算，不按字节：中文一个字就是 1，与 Python 一致
      return Value(static_cast<long long>(utf8Length(v.asString())));
    case Value::Type::kList:
      return Value(static_cast<long long>(v.asList().size()));
    case Value::Type::kTuple:
      return Value(static_cast<long long>(v.asList().size()));
    case Value::Type::kDict:
      return Value(static_cast<long long>(v.dictKeys().size()));
    default:
      return Value(0LL);
  }
}

Value absolute(const Value& v) {
  if (v.type() == Value::Type::kInt) return Value(intOf(v) < 0 ? -intOf(v) : intOf(v));
  return Value(std::fabs(v.toFloat()));
}

Value roundValue(const Value& v) {
  // 必须与 CPython 一致：Python 的 round 用「银行家舍入」（half to even），
  // round(0.5) == 0、round(1.5) == 2、round(2.5) == 2。
  //
  // 这里曾用 std::round（四舍五入远离零），理由是「用户对界面上数字的
  // 预期」；但那会让同一份脚本在解释器与编译产物上算出不同结果——而
  // 「两边一致」是这个库的硬性承诺。界面想要的取整可以在脚本里写
  // `int(x + 0.5)`，语言语义不该为观感让步。
  //
  // std::nearbyint 在默认的 FE_TONEAREST（就近取偶）下就是银行家舍入。
  if (v.type() == Value::Type::kInt) return Value(v.toInt());
  return Value(static_cast<long long>(std::nearbyint(v.toFloat())));
}

Value roundDigits(const Value& v, const Value& digits) {
  const int places = static_cast<int>(digits.toInt());
  const double value = v.toFloat();

  // 为什么不用 nearbyint(x * 10^n) / 10^n：
  // 那会先把 x 乘一遍，把**本来就存在的表示误差放大**再取整。round(2.675, 2)
  // 在 CPython 里是 2.67，因为 2.675 的精确值是 2.67499999...；而
  // 2.675*100 = 267.49999999999997 再取偶得 267 → 2.67，这条侥幸对了，
  // 但 1.335 会得到 1.34（CPython 是 1.33），196 组实测里错了 7 组。
  //
  // 正确的做法是「对 double 的精确十进制值做十进制舍入」——这正是 CPython
  // 的 float.__round__ 干的事，也正是 printf("%.*f") 在 FE_TONEAREST 下的
  // 行为。借它一次，再用 strtod 解析回 double，216 组实测与 CPython 全等。
  //
  // 代价是一次 malloc（printf 内部）与两次格式转换。round() 不在热路径上，
  // 换来的是「脚本里每次 round 都和解释器一致」，值得。
  //
  // 位数范围：printf 的精度参数过大没有意义（double 只有约 17 位有效数字），
  // 但也不能让用户传个 10^9 去触发巨量输出。CPython 对 ndigits 有截断，
  // 这里按同样的思路夹到 [-308, 308]。
  int clamped = places;
  if (clamped > 308) clamped = 308;
  if (clamped < -308) clamped = -308;

  char buffer[400];
  std::snprintf(buffer, sizeof(buffer), "%.*f", clamped, value);
  return Value(static_cast<double>(std::strtod(buffer, nullptr)));
}

Value sumOf(const Value& items) {
  double total = 0.0;
  bool allInt = true;
  long long intTotal = 0;
  for (const Value& item : iterate(items)) {
    if (item.type() != Value::Type::kInt) allInt = false;
    total += item.toFloat();
    intTotal += item.toInt();
  }
  if (allInt) return Value(intTotal);
  return Value(total);
}

Value clone(const Value& v) {
  if (v.isList()) return Value::list(v.asList());
  if (v.isDict()) return Value::dict(v.dictKeys(), v.dictValues());
  return v;
}

Value at(const Value& container, const Value& index) {
  if (container.isDict()) return dictGet(container, index);

  const long long position = index.toInt();
  if (container.type() == Value::Type::kString) {
    const std::string& text = container.asString();
    size_t offset = 0;
    if (!normalizeIndex(position, utf8Length(text), offset)) {
      raiseException("IndexError", "string index out of range");
      return Value();
    }
    // 按字符取：切出的是完整码点，不会把一个汉字劈成两半
    const size_t begin = utf8OffsetOfChar(text, offset);
    const size_t finish = utf8NextBoundary(text, begin);
    return Value(text.substr(begin, finish - begin));
  }
  const List& list = container.asList();
  size_t offset = 0;
  if (!normalizeIndex(position, list.size(), offset)) {
    raiseException("IndexError", container.isTuple() ? "tuple index out of range"
                                                     : "list index out of range");
    return Value();
  }
  return list[offset];
}

Value slice(const Value& container, const Value& start, const Value& end) {
  if (container.type() == Value::Type::kString) {
    const std::string& text = container.asString();
    const long long count = static_cast<long long>(utf8Length(text));
    long long from = start.isNone() ? 0 : start.toInt();
    long long to = end.isNone() ? count : end.toInt();
    if (from < 0) from += count;
    if (to < 0) to += count;
    if (from < 0) from = 0;
    if (to > count) to = count;
    if (to <= from) return Value(std::string());
    const size_t begin = utf8OffsetOfChar(text, static_cast<size_t>(from));
    const size_t finish = utf8OffsetOfChar(text, static_cast<size_t>(to));
    return Value(text.substr(begin, finish - begin));
  }

  const List& list = container.asList();
  const long long count = static_cast<long long>(list.size());
  long long from = start.isNone() ? 0 : start.toInt();
  long long to = end.isNone() ? count : end.toInt();
  if (from < 0) from += count;
  if (to < 0) to += count;
  if (from < 0) from = 0;
  if (to > count) to = count;
  if (to <= from) {
    return container.isTuple() ? Value::tuple(List{}) : Value::list(List{});
  }
  List sliced(list.begin() + static_cast<long long>(from),
              list.begin() + static_cast<long long>(to));
  return container.isTuple() ? Value::tuple(std::move(sliced))
                             : Value::list(std::move(sliced));
}

// 带步长的切片。Python 的 `x[a:b:c]` 里 c 可以是负数（反向），
// 而默认的 slice() 只处理正向、步长 1。
//
// 为什么单独一个函数而不是把 step 变成 slice 的第四个参数：AOT 路线生成的
// 代码全是 slice(a, b)，加参数会让那条路线的所有调用点都改一遍，而它
// 根本用不到 step（pyuic 生成时直接把 step 展开成循环）。分成两个函数，
// 各自的调用点都不必动。
Value sliceStep(const Value& container, const Value& start, const Value& end,
                const Value& step) {
  if (step.isNone()) return slice(container, start, end);
  const long long stride = step.toInt();
  if (stride == 0) return Value();
  if (stride == 1) return slice(container, start, end);

  const bool isText = container.type() == Value::Type::kString;
  const long long count = isText ? static_cast<long long>(utf8Length(container.asString()))
                                 : static_cast<long long>(container.asList().size());

  // Python 的切片边界规则（等价于 slice.indices()）：
  //   stride > 0：默认 start=0、stop=count，钳到 [0, count]
  //   stride < 0：默认 start=count-1、stop=-1（-1 表示「越过头部」），钳到 [-1, count-1]
  long long from = 0;
  long long to = count;
  if (stride > 0) {
    from = start.isNone() ? 0 : start.toInt();
    to = end.isNone() ? count : end.toInt();
    if (from < 0) from += count;
    if (to < 0) to += count;
    if (from < 0) from = 0;
    if (to > count) to = count;
  } else {
    from = start.isNone() ? count - 1 : start.toInt();
    // 注意这里不能用「end.isNone() ? count : end.toInt()」：负步长下
    // 缺省 stop 是「-1 之后」，与显式写 -1 不是一回事。
    to = end.isNone() ? -1 : end.toInt();
    if (!start.isNone() && from < 0) from += count;
    if (!end.isNone() && to < 0) to += count;
    if (from >= count) from = count - 1;
    if (to < -1) to = -1;
  }

  if (isText) {
    const std::string& text = container.asString();
    std::string out;
    if (stride > 0) {
      for (long long i = from; i < to; i += stride) {
        const size_t begin = utf8OffsetOfChar(text, static_cast<size_t>(i));
        const size_t finish = utf8NextBoundary(text, begin);
        out.append(text, begin, finish - begin);
      }
    } else {
      for (long long i = from; i > to; i += stride) {
        if (i < 0) break;
        const size_t begin = utf8OffsetOfChar(text, static_cast<size_t>(i));
        const size_t finish = utf8NextBoundary(text, begin);
        out.append(text, begin, finish - begin);
      }
    }
    return Value(std::move(out));
  }

  const List& list = container.asList();
  List out;
  if (stride > 0) {
    for (long long i = from; i < to; i += stride) {
      out.push_back(list[static_cast<size_t>(i)]);
    }
  } else {
    for (long long i = from; i > to; i += stride) {
      if (i < 0) break;
      out.push_back(list[static_cast<size_t>(i)]);
    }
  }
  return container.isTuple() ? Value::tuple(std::move(out))
                             : Value::list(std::move(out));
}

Value power(const Value& a, const Value& b) {
  if (!asNumberLike(a) || !asNumberLike(b)) {
    raiseException("TypeError", "unsupported operand type(s) for ** or pow(): '" +
                                    typeName(a) + "' and '" + typeName(b) + "'");
    return Value();
  }
  // 整数底数 + 非负整数指数 → 整数结果。
  //
  // 不能一律返回 float：CPython 里 2 ** 10 是 int 1024，而 str() 会立刻
  // 暴露差异（"1024" vs "1024.0"），把它当列表下标也不同。负指数必须走
  // float（2 ** -1 == 0.5），这是 CPython 的规则。
  //
  // 溢出：CPython 的 int 无界，这里用 long long，指数过大时会溢出。
  // 超过约 9.2e18 的结果在产物里不可靠，属于已知差异（见 docs/SUPPORT.md）。
  if (bothIntOrBool(a, b) && intOf(b) >= 0) {
    long long result = 1;
    const long long base = intOf(a);
    for (long long i = intOf(b); i > 0; --i) result *= base;
    return Value(result);
  }
  return Value(std::pow(a.toFloat(), b.toFloat()));
}

Value floorDiv(const Value& a, const Value& b) {
  if (!asNumberLike(a) || !asNumberLike(b)) {
    raiseException("TypeError", "unsupported operand type(s) for //: '" +
                                    typeName(a) + "' and '" + typeName(b) + "'");
    return Value();
  }
  const double divisor = b.toFloat();
  if (divisor == 0.0) {
    raiseException("ZeroDivisionError",
                   bothInt(a, b) ? "integer division or modulo by zero"
                                 : "float floor division by zero");
    return Value();
  }
  // 向负无穷取整：C 的整数除法是向零截断，-7/2 会得到 -3，
  // 而 Python 是 -4。分页、网格坐标这类计算几乎都依赖后者。
  const double result = std::floor(a.toFloat() / divisor);
  if (bothInt(a, b)) return Value(static_cast<long long>(result));
  return Value(result);
}

Value isNone(const Value& v) { return Value(v.isNone()); }

Value isSame(const Value& a, const Value& b) {
  // 容器按「是不是同一个对象」判断，其余类型 Python 的 is 语义本来就按值
  // 相等（小整数与字符串驻留），用 eq 近似是对的，且更可预测。
  if (a.isList() && b.isList()) return Value(a.mutableList() == b.mutableList());
  if (a.isDict() && b.isDict()) return Value(a.mutableDict() == b.mutableDict());
  return eq(a, b);
}

std::vector<Value> iterate(const Value& container) {
  std::vector<Value> out;
  if (container.type() == Value::Type::kString) {
    // 按字符遍历：`for ch in text` 在脚本里就是逐字，中文一个汉字一个元素。
    // 按字节遍历会得到一堆半个字的乱码片段。
    const std::string& text = container.asString();
    for (size_t offset = 0; offset < text.size();) {
      const size_t next = utf8NextBoundary(text, offset);
      out.emplace_back(text.substr(offset, next - offset));
      offset = next;
    }
    return out;
  }
  if (container.isDict()) return container.dictKeys();
  if (container.type() == Value::Type::kObject) {
    // Python 的迭代协议：对象自己提供 `__iter__`。VM 仍然不知道「文件」是
    // 什么——它只按名字转发，表由 pylib 提供（与 callMethod 同一条原则）。
    // 返回的仍然物化成列表：kIterInit 本来就是这么建的栈（见 machine.cpp）。
    const ObjectData* data = container.asObject();
    if (data == nullptr || data->table == nullptr) return out;
    for (const MethodEntry* entry = data->table->methods;
         entry != nullptr && entry->name != nullptr; ++entry) {
      if (std::string(entry->name) != "__iter__") continue;
      const Value produced = entry->fn(container, nullptr, 0);
      if (produced.isList()) return produced.asList();
      if (produced.isDict()) return produced.dictKeys();
      if (produced.type() == Value::Type::kString) return iterate(produced);
      return out;
    }
    return out;
  }
  return container.asList();
}

// ── 容器原地操作 ──

void append(const Value& list, const Value& item) {
  if (List* items = list.mutableList()) items->push_back(item);
}

void extend(const Value& list, const Value& items) {
  List* target = list.mutableList();
  if (target == nullptr) return;
  const std::vector<Value> source = iterate(items);
  target->insert(target->end(), source.begin(), source.end());
}

void insertAt(const Value& list, const Value& index, const Value& item) {
  List* items = list.mutableList();
  if (items == nullptr) return;
  long long position = index.toInt();
  if (position < 0) position += static_cast<long long>(items->size());
  if (position < 0) position = 0;
  if (position > static_cast<long long>(items->size())) position = items->size();
  items->insert(items->begin() + position, item);
}

Value popAt(const Value& list, const Value& index) {
  List* items = list.mutableList();
  if (items == nullptr || items->empty()) return Value();
  long long position = index.isNone() ? static_cast<long long>(items->size()) - 1 : index.toInt();
  if (position < 0) position += static_cast<long long>(items->size());
  if (position < 0 || position >= static_cast<long long>(items->size())) return Value();
  Value out = (*items)[static_cast<size_t>(position)];
  items->erase(items->begin() + position);
  return out;
}

void clearContainer(const Value& container) {
  if (List* items = container.mutableList()) items->clear();
  if (DictData* dict = container.mutableDict()) {
    dict->keys.clear();
    dict->values.clear();
  }
}

void sortList(const Value& list) {
  List* items = list.mutableList();
  if (items == nullptr) return;
  std::stable_sort(items->begin(), items->end(),
                   [](const Value& a, const Value& b) { return compareValues(a, b) < 0; });
}

void reverseList(const Value& list) {
  if (List* items = list.mutableList()) std::reverse(items->begin(), items->end());
}

// ── 字典 ──

Value dictGet(const Value& dict, const Value& key) {
  const long long index = dict.dictIndex(key);
  if (index < 0) {
    // CPython 的 KeyError 消息是 repr(key)——字符串会带引号，而 str() 不带。
    std::string message;
    appendRepr(message, key);
    raiseException("KeyError", message);
    return Value();
  }
  return dict.dictValues()[static_cast<size_t>(index)];
}

Value dictGetOr(const Value& dict, const Value& key, const Value& fallback) {
  const long long index = dict.dictIndex(key);
  if (index < 0) return fallback;
  return dict.dictValues()[static_cast<size_t>(index)];
}

void dictSet(const Value& dict, const Value& key, const Value& value) {
  DictData* data = dict.mutableDict();
  if (data == nullptr) return;
  const long long index = dict.dictIndex(key);
  if (index >= 0) {
    data->values[static_cast<size_t>(index)] = value;
    return;
  }
  // 新键追加在末尾：Python 3.7+ 的字典保持插入顺序，界面上看得到这个差别。
  data->keys.push_back(key);
  data->values.push_back(value);
}

Value dictKeysOf(const Value& dict) { return Value::list(dict.dictKeys()); }
Value dictValuesOf(const Value& dict) { return Value::list(dict.dictValues()); }

Value dictItemsOf(const Value& dict) {
  const List& keys = dict.dictKeys();
  const List& values = dict.dictValues();
  List out;
  for (size_t i = 0; i < keys.size(); ++i) out.push_back(Value::list(List{keys[i], values[i]}));
  return Value::list(std::move(out));
}

Value dictPop(const Value& dict, const Value& key, const Value& fallback) {
  DictData* data = dict.mutableDict();
  if (data == nullptr) return fallback;
  const long long index = dict.dictIndex(key);
  if (index < 0) return fallback;
  const size_t atIndex = static_cast<size_t>(index);
  Value out = data->values[atIndex];
  data->keys.erase(data->keys.begin() + index);
  data->values.erase(data->values.begin() + index);
  return out;
}

void dictUpdate(const Value& target, const Value& other) {
  if (!other.isDict()) return;
  const List& keys = other.dictKeys();
  const List& values = other.dictValues();
  for (size_t i = 0; i < keys.size(); ++i) dictSet(target, keys[i], values[i]);
}

Value dictFrom(const Value& keys, const Value& values) {
  List keyList;
  List valueList;
  const std::vector<Value> keyItems = iterate(keys);

  // 两种形态：
  //   dict_from(["a", "b"], [1, 2])      —— 两个平行列表
  //   dict_from([["a", 1], ["b", 2]])    —— 键值对列表（items() 的输出）
  // 后者靠 values 为 None 来区分，所以不能在 keys 里就假定是平行列表。
  if (values.isNone()) {
    for (const Value& item : keyItems) {
      if (item.isList() && item.asList().size() == 2) {
        keyList.push_back(item.asList()[0]);
        valueList.push_back(item.asList()[1]);
      } else {
        keyList.push_back(item);
      }
    }
    return Value::dict(keyList, valueList);
  }

  keyList = keyItems;
  valueList = iterate(values);
  return Value::dict(keyList, valueList);
}

// ── 下标赋值 ──

void setItem(const Value& container, const Value& index, const Value& value) {
  if (DictData* dict = container.mutableDict()) {
    dictSet(container, index, value);
    return;
  }
  List* items = container.mutableList();
  if (items == nullptr) return;

  long long position = index.toInt();
  if (position < 0) position += static_cast<long long>(items->size());
  if (position < 0) return;
  // 越界时按 Python 的直觉扩容：脚本里 `out[i] = x` 累加结果很常见，
  // 报「下标越界」在这里没有收益，因为容器是我们自己刚建的。
  if (position >= static_cast<long long>(items->size())) {
    items->resize(static_cast<size_t>(position) + 1);
  }
  (*items)[static_cast<size_t>(position)] = value;
}

// ── 字符串内建 ──
//
// 这些是 Python 语言本身的方法（"abc".upper()），不是界面能力，
// 所以放在值层：AiPy 不装 pyui 也必须能用它们。
//
// 列表方法（append/extend/pop …）也在本文件：容器语义，与脚本 API 无关。

Value upper(const Value& text) {
  std::string out = text.toString();
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  }
  return Value(std::move(out));
}

Value lower(const Value& text) {
  std::string out = text.toString();
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return Value(std::move(out));
}

Value strip(const Value& text) {
  const std::string& source = text.toString();
  size_t begin = 0;
  size_t end = source.size();
  auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (begin < end && space(static_cast<unsigned char>(source[begin]))) ++begin;
  while (end > begin && space(static_cast<unsigned char>(source[end - 1]))) --end;
  return Value(source.substr(begin, end - begin));
}

Value replace(const Value& text, const Value& from, const Value& to) {
  std::string out = text.toString();
  const std::string needle = from.toString();
  if (needle.empty()) return Value(std::move(out));
  const std::string replacement = to.toString();
  size_t position = 0;
  while ((position = out.find(needle, position)) != std::string::npos) {
    out.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
  return Value(std::move(out));
}

Value split(const Value& text, const Value& separator) {
  const std::string source = text.toString();
  const std::string needle = separator.toString();
  Value::List parts;
  if (needle.empty()) {
    // 不传分隔符时按空白切：这是 Python 的 split() 无参语义，
    // 也是「从一行词里取第一个」最常见的用法。
    std::string current;
    for (char c : source) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (!current.empty()) {
          parts.emplace_back(std::move(current));
          current.clear();
        }
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) parts.emplace_back(std::move(current));
    return Value::list(std::move(parts));
  }

  size_t start = 0;
  for (;;) {
    const size_t hit = source.find(needle, start);
    if (hit == std::string::npos) {
      parts.emplace_back(source.substr(start));
      break;
    }
    parts.emplace_back(source.substr(start, hit - start));
    start = hit + needle.size();
  }
  return Value::list(std::move(parts));
}

Value join(const Value& separator, const Value& items) {
  const std::string glue = separator.toString();
  std::string out;
  bool first = true;
  for (const Value& item : iterate(items)) {
    if (!first) out += glue;
    // 非字符串元素按 str() 处理：`" ".join([1, 2])` 在 Python 里会抛错，
    // 但脚本里更常见的意图是「拼成文本」，报错在这里没有收益。
    out += item.toString();
    first = false;
  }
  return Value(std::move(out));
}

Value startsWith(const Value& text, const Value& prefix) {
  const std::string source = text.toString();
  const std::string needle = prefix.toString();
  return Value(source.size() >= needle.size() && source.compare(0, needle.size(), needle) == 0);
}

Value endsWith(const Value& text, const Value& suffix) {
  const std::string source = text.toString();
  const std::string needle = suffix.toString();
  return Value(source.size() >= needle.size() &&
               source.compare(source.size() - needle.size(), needle.size(), needle) == 0);
}

Value findIn(const Value& text, const Value& needle) {
  const std::string& source = text.toString();
  const std::string target = needle.toString();
  const size_t hit = source.find(target);
  if (hit == std::string::npos) return Value(-1LL);
  // 返回**字符**下标而不是字节下标：脚本拿去切片时两者必须一致，
  // 中文文本里混用会出现「找到的位置切出来是半个字」。
  return Value(static_cast<long long>(utf8Length(std::string_view(source).substr(0, hit))));
}

Value countIn(const Value& text, const Value& needle) {
  const std::string& source = text.toString();
  const std::string target = needle.toString();
  if (target.empty()) return Value(0LL);
  long long count = 0;
  size_t position = 0;
  while ((position = source.find(target, position)) != std::string::npos) {
    ++count;
    position += target.size();
  }
  return Value(count);
}

Value textOfChar(const Value& codepoint) {
  return Value(utf8Encode(static_cast<std::uint32_t>(codepoint.toInt())));
}

// str.format() 的最小实现：只支持 {} 与 {0} {1} 这类按位置替换。
// 不做格式规格（{:.2f}）——那要一整套格式迷你语言，而脚本里
// 「先 round 再 str」已经够用，硬塞半个实现反而会让人以为支持全了。
Value formatText(const Value& text, const Value& args) {
  const std::string& pattern = text.toString();
  const std::vector<Value> values = iterate(args);
  std::string out;
  size_t index = 0;
  size_t next = 0;
  while (index < pattern.size()) {
    const size_t open = pattern.find('{', index);
    if (open == std::string::npos) {
      out += pattern.substr(index);
      break;
    }
    const size_t close = pattern.find('}', open + 1);
    if (close == std::string::npos) {
      // 没有配对的 }：原样保留剩下的部分，不要吞掉用户文本
      out += pattern.substr(index);
      break;
    }
    out += pattern.substr(index, open - index);
    const std::string spec = pattern.substr(open + 1, close - open - 1);
    if (spec.empty()) {
      if (next < values.size()) out += values[next++].toString();
    } else {
      const auto explicitIndex = static_cast<size_t>(std::strtoull(spec.c_str(), nullptr, 10));
      if (explicitIndex < values.size()) out += values[explicitIndex].toString();
    }
    index = close + 1;
  }
  return Value(std::move(out));
}

Value minimum(const Value& a, const Value& b) { return lt(a, b).toBool() ? a : b; }
Value maximum(const Value& a, const Value& b) { return gt(a, b).toBool() ? a : b; }

// 严格解析整串：必须从头到尾都是这个数字（允许首尾空白与下划线分隔符）。
// 失败返回 false，调用方据此抛 ValueError——这与 CPython 的
// `int("12abc")` 报错一致，而 strtoll 会静默停在 "a" 处返回 12。
namespace {

bool parseIntegerText(const std::string& source, long long* out) {
  // 去掉首尾空白（CPython 允许 "  12  "）。
  std::size_t begin = 0;
  std::size_t end = source.size();
  while (begin < end && (source[begin] == ' ' || source[begin] == '\t' ||
                         source[begin] == '\n' || source[begin] == '\r' ||
                         source[begin] == '\f' || source[begin] == '\v')) {
    ++begin;
  }
  while (end > begin && (source[end - 1] == ' ' || source[end - 1] == '\t' ||
                         source[end - 1] == '\n' || source[end - 1] == '\r' ||
                         source[end - 1] == '\f' || source[end - 1] == '\v')) {
    --end;
  }
  if (begin >= end) return false;

  std::string digits;
  digits.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    const char ch = source[i];
    // 下划线只能夹在两个数字之间（CPython 的规则），先简化成「不能在首尾」。
    if (ch == '_') {
      if (i == begin || i + 1 >= end) return false;
      continue;
    }
    digits.push_back(ch);
  }

  const char* start = digits.c_str();
  char* stop = nullptr;
  const long long value = std::strtoll(start, &stop, 10);
  if (stop == start) return false;
  // 剩余还有字符（"12abc" 的 "abc"、「12.5」的 ".5"）就整串失败。
  while (*stop != '\0' && (*stop == ' ' || *stop == '\t')) ++stop;
  if (*stop != '\0') return false;
  *out = value;
  return true;
}

bool parseFloatText(const std::string& source, double* out) {
  std::size_t begin = 0;
  std::size_t end = source.size();
  while (begin < end && (source[begin] == ' ' || source[begin] == '\t' ||
                         source[begin] == '\n' || source[begin] == '\r' ||
                         source[begin] == '\f' || source[begin] == '\v')) {
    ++begin;
  }
  while (end > begin && (source[end - 1] == ' ' || source[end - 1] == '\t' ||
                         source[end - 1] == '\n' || source[end - 1] == '\r' ||
                         source[end - 1] == '\f' || source[end - 1] == '\v')) {
    --end;
  }
  if (begin >= end) return false;

  std::string digits;
  digits.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    if (source[i] == '_') {
      if (i == begin || i + 1 >= end) return false;
      continue;
    }
    digits.push_back(source[i]);
  }

  const char* start = digits.c_str();
  char* stop = nullptr;
  const double value = std::strtod(start, &stop);
  if (stop == start) return false;
  if (*stop != '\0') return false;
  *out = value;
  return true;
}

}  // namespace

Value parseInteger(const Value& text) {
  long long value = 0;
  if (!parseIntegerText(text.toString(), &value)) return Value();
  return Value(value);
}

Value parseFloat(const Value& text) {
  double value = 0.0;
  if (!parseFloatText(text.toString(), &value)) return Value();
  return Value(value);
}

Value charFromCode(const Value& codepoint) {
  return Value(utf8Encode(static_cast<std::uint32_t>(codepoint.toInt())));
}

// ── 调试输出 ──
//
// 默认写 stderr。GUI 程序在 Windows 上没有 stderr 窗口，所以宿主（桥接层）
// 装一个 OutputDebugString 的实现。语言层不写 #ifdef：它不该知道任何平台。

namespace {

LogSink& logSink() {
  static LogSink sink = [](const std::string& text) {
    std::fprintf(stderr, "[aipy] %s\n", text.c_str());
  };
  return sink;
}

}  // namespace

void setLogSink(LogSink sink) {
  if (sink != nullptr) {
    logSink() = std::move(sink);
    return;
  }
  logSink() = [](const std::string& text) {
    std::fprintf(stderr, "[aipy] %s\n", text.c_str());
  };
}

void log(const Value& value) { logSink()(value.toString()); }

// ── 诊断输出 ──
//
// 与 logSink 分开一套，理由见 value.h：print 走 log，而 print 的输出是
// **程序输出**；pylib 的失败原因是**诊断**，不该混进去。

namespace {

DiagnosticSink& diagnosticSink() {
  static DiagnosticSink sink = [](const std::string& text) {
    std::fprintf(stderr, "[aipy] %s\n", text.c_str());
  };
  return sink;
}

}  // namespace

void setDiagnosticSink(DiagnosticSink sink) {
  if (sink != nullptr) {
    diagnosticSink() = std::move(sink);
    return;
  }
  diagnosticSink() = [](const std::string& text) {
    std::fprintf(stderr, "[aipy] %s\n", text.c_str());
  };
}

void diagnose(const std::string& text) { diagnosticSink()(text); }


}
