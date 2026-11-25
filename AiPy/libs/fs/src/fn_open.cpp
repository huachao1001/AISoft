// `fs.open(path, mode, encoding)` —— 对齐 Python 的文件对象。
//
//   with fs.open("a.txt", "w") as f:
//       f.write("hello\n")
//
// ── 对齐到什么程度 ──
//
// 模式串按 CPython 解析：`r w a x` 之一，可带 `+`（读写）、`b`（二进制）、
// `t`（文本，默认）。文本模式做 universal newlines：读时 `\r\n` 与孤立的
// `\r` 都归一成 `\n`，写时 `\n` 换成本平台换行。这与 `newline=None` 的
// 默认行为一致。
//
// 方法对齐 CPython 的名字与返回值：
//
//   read(size=-1)  readline(size=-1)  readlines()  __iter__
//   write(text)    writelines(lines)  flush()      close()
//   seek(offset, whence=0)            tell()
//   readable()     writable()         seekable()   closed()
//
// 两个语言层面的差异，不是偷懒：
//
// 1. **属性变成了方法**。AiPy 没有属性访问（`a.b` 只认 `self.x` 与
//    `模块.函数`），所以 Python 的 `f.closed` / `f.name` 在这里是
//    `f.closed()` / `f.name()`。
// 2. **失败返回 None，不抛异常**。语言没有异常通道（见 vm.h 文件头）。
//
// ── 为什么整份文件读进内存 ──
//
// 三个理由：
//   a. `for line in f` 本来就会被 VM 物化成一个列表（见 machine.cpp 的
//      kIterInit），流式读取省不下那次拷贝。
//   b. 文本模式的 `read(n)` 里 n 是**字符数**而不是字节数，边读边截断
//      还要处理跨块的多字节字符与跨块的 `\r\n`——那是最容易出错的地方，
//      而它换来的只是「小工具不必常驻大文件」。
//   c. 读、写、seek、tell 读写交错时行为完全可预测：位置就是缓冲区下标，
//      不必在 FILE* 的缓冲与自己的缓冲之间对账。
//
// 代价是大文件占内存，这在注释里说清比藏着好。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"
#include "aipy/utf8.h"

#include <cstdio>
#include <new>
#include <string>

namespace {

struct FileState {
  std::string path;
  // 文件内容。文本模式下已归一成 `\n`，写回磁盘时才换成本平台换行；
  // 二进制模式下是原始字节。
  std::string content;
  std::size_t position = 0;
  bool readable = false;
  bool writable = false;
  bool binary = false;
  bool append = false;
  bool closed = false;
  // 与磁盘不一致。flush/close 才真正落盘——这样脚本连续 write 一百次
  // 也只写一次磁盘。
  bool dirty = false;
};

FileState* stateOf(const aipy::Value& self) {
  const aipy::ObjectData* data = self.asObject();
  if (data == nullptr || data->host == nullptr) return nullptr;
  return static_cast<FileState*>(data->host);
}

// 落盘。成功返回 true；失败时已写过诊断。
bool flushState(FileState* state) {
  if (state == nullptr || !state->dirty) return true;
  std::string error;
  // 文本模式交给 writeAll 做换行转换：这里的内容统一是 `\n`。
  if (!aipy::fs::writeAll(state->path, state->content, false, &error,
                          !state->binary)) {
    aipy::diagnose(error);
    return false;
  }
  state->dirty = false;
  return true;
}

bool requireOpen(const FileState* state, const char* method) {
  if (state == nullptr) {
    aipy::diagnose(std::string("文件对象已失效（") + method + "）");
    return false;
  }
  if (state->closed) {
    aipy::diagnose(std::string("对已关闭的文件调用 ") + method + "()");
    return false;
  }
  return true;
}

bool requireReadable(const FileState* state, const char* method) {
  if (!requireOpen(state, method)) return false;
  if (!state->readable) {
    aipy::diagnose(std::string("文件不是以读模式打开的，不能 ") + method + "()");
    return false;
  }
  return true;
}

bool requireWritable(const FileState* state, const char* method) {
  if (!requireOpen(state, method)) return false;
  if (!state->writable) {
    aipy::diagnose(std::string("文件不是以写模式打开的，不能 ") + method + "()");
    return false;
  }
  return true;
}

// 从 position 取 n 个「单位」的长度。文本模式按字符（码点），二进制按字节；
// n < 0 表示到末尾。
std::size_t takeLength(const FileState* state, long long count) {
  const std::size_t remaining = state->content.size() - state->position;
  if (count < 0) return remaining;
  const auto wanted = static_cast<std::size_t>(count);
  if (state->binary) return wanted < remaining ? wanted : remaining;
  // 文本模式按字符：先按字符数换算出字节数，再夹到剩余长度。
  const std::string_view tail(state->content.data() + state->position, remaining);
  const std::size_t bytes = aipy::utf8PrefixByChars(tail, wanted);
  return bytes < remaining ? bytes : remaining;
}

aipy::Value readMethod(const aipy::Value& self, const aipy::Value* args, int count) {
  FileState* state = stateOf(self);
  if (!requireReadable(state, "read")) return aipy::Value();
  const long long wanted = count >= 1 ? args[0].toInt() : -1LL;
  const std::size_t length = takeLength(state, wanted);
  std::string out = state->content.substr(state->position, length);
  state->position += length;
  // 到末尾返回空串而不是 None：CPython 的 read() 在 EOF 就是 ""。
  return aipy::Value(std::move(out));
}

// 读一行，保留行尾的 `\n`（CPython 的行为）。size 限制的是字符数。
aipy::Value readlineMethod(const aipy::Value& self, const aipy::Value* args,
                           int count) {
  FileState* state = stateOf(self);
  if (!requireReadable(state, "readline")) return aipy::Value();
  if (state->position >= state->content.size()) return aipy::Value(std::string());

  const long long limit = count >= 1 ? args[0].toInt() : -1LL;
  const std::size_t newline = state->content.find('\n', state->position);
  std::size_t end = newline == std::string::npos ? state->content.size() : newline + 1;

  if (limit >= 0) {
    // 按字符限制截断，但不要越过行尾。
    const std::size_t span = end - state->position;
    const std::string_view tail(state->content.data() + state->position, span);
    const auto wanted = static_cast<std::size_t>(limit);
    std::size_t allowed = state->binary ? span : aipy::utf8PrefixByChars(tail, wanted);
    if (allowed < span) end = state->position + allowed;
  }

  std::string out = state->content.substr(state->position, end - state->position);
  state->position = end;
  return aipy::Value(std::move(out));
}

aipy::Value readlinesMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (!requireReadable(state, "readlines")) return aipy::Value();
  aipy::Value::List lines;
  while (state->position < state->content.size()) {
    const std::size_t newline = state->content.find('\n', state->position);
    const std::size_t end =
        newline == std::string::npos ? state->content.size() : newline + 1;
    lines.push_back(aipy::Value(
        state->content.substr(state->position, end - state->position)));
    state->position = end;
  }
  return aipy::Value::list(std::move(lines));
}

// `for line in f` 走这里。VM 的迭代协议要求返回列表（见 value.cpp 的
// iterate），所以它就是「剩余的全部行」。
aipy::Value iterMethod(const aipy::Value& self, const aipy::Value*, int) {
  return readlinesMethod(self, nullptr, 0);
}

aipy::Value writeMethod(const aipy::Value& self, const aipy::Value* args, int count) {
  FileState* state = stateOf(self);
  if (!requireWritable(state, "write")) return aipy::Value();
  if (count < 1) {
    aipy::diagnose("write 需要 1 个参数");
    return aipy::Value();
  }
  const std::string text = args[0].toString();

  // 追加模式下每次写都落在末尾（CPython 的 "a" 就是这样）。
  if (state->append) state->position = state->content.size();

  // seek 越过末尾后写入要补零字节——与 CPython 一致。
  if (state->position > state->content.size()) {
    state->content.resize(state->position, '\0');
  }

  // 覆盖写入：写入段替换等长的旧内容；写的内容比剩下的长就整体增长。
  // 不能用「先截到末尾再追加」——那样 `seek(0)` 后写两个字符会把整个文件
  // 清掉。
  const std::size_t remaining = state->content.size() - state->position;
  const std::size_t shadowed =
      text.size() < remaining ? text.size() : remaining;
  state->content.replace(state->position, shadowed, text);
  state->position += text.size();
  state->dirty = true;

  // CPython 的 write 返回**写入的字符数**，不是字节数。
  return aipy::Value(static_cast<long long>(state->binary ? text.size()
                                                         : aipy::utf8Length(text)));
}

aipy::Value writelinesMethod(const aipy::Value& self, const aipy::Value* args,
                             int count) {
  FileState* state = stateOf(self);
  if (!requireWritable(state, "writelines")) return aipy::Value();
  if (count < 1) {
    aipy::diagnose("writelines 需要 1 个参数（列表）");
    return aipy::Value();
  }
  // 与 CPython 一样**不加**换行：调用方给的行要自带 `\n`。
  for (const aipy::Value& line : aipy::iterate(args[0])) {
    const aipy::Value item(line);
    writeMethod(self, &item, 1);
  }
  return aipy::Value();
}

aipy::Value flushMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (!requireOpen(state, "flush")) return aipy::Value();
  flushState(state);
  return aipy::Value();
}

aipy::Value seekMethod(const aipy::Value& self, const aipy::Value* args, int count) {
  FileState* state = stateOf(self);
  if (!requireOpen(state, "seek")) return aipy::Value();
  if (count < 1) {
    aipy::diagnose("seek 需要 offset 参数");
    return aipy::Value();
  }
  const long long offset = args[0].toInt();
  const long long whence = count >= 2 ? args[1].toInt() : 0LL;

  long long base = 0;
  if (whence == 0) {
    base = 0;
  } else if (whence == 1) {
    base = static_cast<long long>(state->position);
  } else if (whence == 2) {
    base = static_cast<long long>(state->content.size());
  } else {
    aipy::diagnose("seek 的 whence 只认 0 / 1 / 2");
    return aipy::Value();
  }

  const long long target = base + offset;
  if (target < 0) {
    aipy::diagnose("seek 到了文件开头之前");
    return aipy::Value();
  }
  // 允许越过末尾（与 CPython 一致）：之后的 read 返回空串，write 补零。
  state->position = static_cast<std::size_t>(target);
  return aipy::Value(static_cast<long long>(state->position));
}

aipy::Value tellMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (!requireOpen(state, "tell")) return aipy::Value();
  return aipy::Value(static_cast<long long>(state->position));
}

aipy::Value closeMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (state == nullptr || state->closed) return aipy::Value();
  flushState(state);
  state->closed = true;
  // 释放缓冲区：已关闭的对象不该继续占着一份文件内容。
  std::string().swap(state->content);
  state->position = 0;
  return aipy::Value();
}

// 布尔查询三个。Python 里它们是属性，这里是方法（语言没有属性访问）。
aipy::Value readableMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  return aipy::Value(state != nullptr && state->readable);
}

aipy::Value writableMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  return aipy::Value(state != nullptr && state->writable);
}

aipy::Value seekableMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  return aipy::Value(state != nullptr);
}

aipy::Value closedMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  return aipy::Value(state == nullptr || state->closed);
}

aipy::Value nameMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (state == nullptr) return aipy::Value();
  return aipy::Value(state->path);
}

// `__enter__` 返回接收者本身：`with fs.open(...) as f` 里的 f 就是这个文件
// 对象。用 host 重新包一个 Value 会造出第二份堆载荷，两份各自 delete，
// 结果是 double free（threading 的 Lock 踩过这个坑）。
aipy::Value enterMethod(const aipy::Value& self, const aipy::Value*, int) {
  return self;
}

aipy::Value exitMethod(const aipy::Value& self, const aipy::Value*, int) {
  return closeMethod(self, nullptr, 0);
}

const aipy::MethodEntry kFileMethods[] = {
    {"read", &readMethod},
    {"readline", &readlineMethod},
    {"readlines", &readlinesMethod},
    {"__iter__", &iterMethod},
    {"write", &writeMethod},
    {"writelines", &writelinesMethod},
    {"seek", &seekMethod},
    {"tell", &tellMethod},
    {"flush", &flushMethod},
    {"close", &closeMethod},
    {"readable", &readableMethod},
    {"writable", &writableMethod},
    {"seekable", &seekableMethod},
    {"closed", &closedMethod},
    {"name", &nameMethod},
    {"__enter__", &enterMethod},
    {"__exit__", &exitMethod},
    {nullptr, nullptr},
};

const aipy::MethodTable kFileTable = {kFileMethods};

void destroyFile(void* host) {
  FileState* state = static_cast<FileState*>(host);
  if (state == nullptr) return;
  // 引用计数归零时还没 close，说明脚本忘了——这里兜底落盘并关闭，
  // 否则缓冲区里的内容会丢（写到一半的文件比没写更糟）。
  flushState(state);
  delete state;
}

// ── 模式串解析 ──

struct Mode {
  char primary = '\0';  // r / w / a / x
  bool plus = false;
  bool binary = false;
};

// 按 CPython 的规则解析：`rwax` 恰好一个，`+` 至多一个，`b`/`t` 至多一个
// 且互斥。`t` 是默认值，显式写出来等价于不写。
bool parseMode(const std::string& text, Mode* out, std::string* error) {
  if (text.empty()) {
    *error = "mode 不能为空";
    return false;
  }
  bool sawText = false;
  for (char ch : text) {
    switch (ch) {
      case 'r':
      case 'w':
      case 'a':
      case 'x':
        if (out->primary != '\0') {
          *error = "mode 里只能有一个 r / w / a / x";
          return false;
        }
        out->primary = ch;
        break;
      case '+':
        if (out->plus) {
          *error = "mode 里只能有一个 +";
          return false;
        }
        out->plus = true;
        break;
      case 'b':
        if (out->binary || sawText) {
          *error = "mode 里 b 和 t 不能同时出现，也不能重复";
          return false;
        }
        out->binary = true;
        break;
      case 't':
        if (out->binary || sawText) {
          *error = "mode 里 b 和 t 不能同时出现，也不能重复";
          return false;
        }
        sawText = true;
        break;
      default:
        *error = std::string("mode 里有无法识别的字符 '") + ch + "'";
        return false;
    }
  }
  if (out->primary == '\0') {
    *error = "mode 必须包含 r / w / a / x 之一";
    return false;
  }
  return true;
}

// 只接受 UTF-8。AiPy 的字符串就是 UTF-8，别的编码要整体转码——那是另一件事，
// 悄悄按错误的编码解释只会得到乱码，所以明确拒绝。
bool acceptEncoding(const std::string& encoding, std::string* error) {
  if (encoding.empty()) return true;
  std::string lowered;
  lowered.reserve(encoding.size());
  for (char ch : encoding) {
    lowered.push_back(static_cast<char>(
        (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch));
  }
  if (lowered == "utf-8" || lowered == "utf8" || lowered == "u8" ||
      lowered == "utf_8") {
    return true;
  }
  *error = "只支持 utf-8 编码（AiPy 的字符串就是 UTF-8），收到的是 " + encoding;
  return false;
}

aipy::Value openImpl(const aipy::Value* args, int count) {
  const std::string path = count >= 1 ? args[0].toString() : std::string();
  const std::string modeText =
      count >= 2 ? args[1].toString() : std::string("r");
  const std::string encoding = count >= 3 ? args[2].toString() : std::string();
  if (path.empty()) {
    aipy::diagnose("open 需要文件路径");
    return aipy::Value();
  }

  Mode mode;
  std::string error;
  if (!parseMode(modeText, &mode, &error)) {
    aipy::diagnose(aipy::fs::describeError(path, error));
    return aipy::Value();
  }
  if (!acceptEncoding(encoding, &error)) {
    aipy::diagnose(aipy::fs::describeError(path, error));
    return aipy::Value();
  }

  FileState* state = new (std::nothrow) FileState();
  if (state == nullptr) return aipy::Value();
  state->path = path;
  state->binary = mode.binary;
  state->readable = (mode.primary == 'r') || mode.plus;
  state->writable = (mode.primary != 'r') || mode.plus;
  state->append = mode.primary == 'a';

  const bool text = !mode.binary;
  // 打开阶段的失败要在这里就报出来，与 CPython 一样：open 拿不到文件时
  // 后面每个 read 都会失败，早报比晚报有用。
  bool loaded = true;
  switch (mode.primary) {
    case 'r':
      loaded = aipy::fs::readAll(path, &state->content, &error, text);
      break;
    case 'a': {
      bool isDirectory = false;
      if (aipy::fs::pathExists(path, &isDirectory) && isDirectory) {
        error = aipy::fs::describeError(path, "那是一个目录");
        loaded = false;
        break;
      }
      // 不存在就创建空文件（CPython 的 "a" 会创建）。
      if (!aipy::fs::pathExists(path, nullptr)) {
        loaded = aipy::fs::writeAll(path, std::string(), true, &error, text);
      } else {
        loaded = aipy::fs::readAll(path, &state->content, &error, text);
      }
      state->position = state->content.size();
      break;
    }
    case 'w':
      // 立即截断：CPython 的 "w" 在 open 时就清空，不等第一次 write。
      loaded = aipy::fs::writeAll(path, std::string(), false, &error, text);
      break;
    case 'x': {
      void* handle = aipy::fs::openExclusive(path, mode.plus);
      if (handle == nullptr) {
        error = aipy::fs::describeError(
            path, "文件已存在，x 模式不覆盖（要覆盖请用 w）");
        loaded = false;
        break;
      }
      std::fclose(static_cast<std::FILE*>(handle));
      break;
    }
    default:
      loaded = false;
      error = "无法识别的模式";
      break;
  }

  if (!loaded) {
    aipy::diagnose(error);
    delete state;
    return aipy::Value();
  }
  return aipy::Value::object(state, &kFileTable, &destroyFile);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, open, openImpl);
