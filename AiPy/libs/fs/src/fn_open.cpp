// `fs.open(path, mode)` —— 文件对象，支持 `with`。
//
// 对应 Python 的 `open()`。返回的是 kObject（带方法表），所以：
//
//   with fs.open("a.txt", "w") as f:
//       f.write("hello")
//
// ── 为什么要有它，read_text/write_text 不够吗 ──
//
// 一次读完整个文件在脚本里是最常见的；但「边算边写」或「读大文件的一部分」
// 需要保持句柄。更重要的是：它是 `with` 在文件场景下的示范——退出必关，
// 包括 return / break / continue（见 vm.h 的 kWithExit）。
//
// ── mode 只认几种 ──
//
// "r"（读）、"w"（覆盖写）、"a"（追加）。二进制模式不提供：AiPy 的字符串
// 是 UTF-8 文本，没有 bytes 类型。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

#include <cstdio>
#include <new>
#include <string>

namespace {

struct FileState {
  std::FILE* file = nullptr;
  std::string path;
};

void destroyFile(void* host) {
  FileState* state = static_cast<FileState*>(host);
  if (state == nullptr) return;
  // 引用计数归零时句柄还没关，说明脚本忘了 close——这里兜底关掉，
  // 否则缓冲区里的内容会丢（写到一半的文件比没写更糟）。
  if (state->file != nullptr) std::fclose(state->file);
  delete state;
}

FileState* stateOf(const aipy::Value& self) {
  const aipy::ObjectData* data = self.asObject();
  if (data == nullptr || data->host == nullptr) return nullptr;
  return static_cast<FileState*>(data->host);
}

aipy::Value readMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (state == nullptr || state->file == nullptr) return aipy::Value();
  std::string data;
  char chunk[16 * 1024];
  for (;;) {
    const std::size_t got = std::fread(chunk, 1, sizeof(chunk), state->file);
    if (got == 0) break;
    data.append(chunk, got);
  }
  if (std::ferror(state->file) != 0) {
    aipy::diagnose(aipy::fs::describeError(state->path, "读取中断"));
    return aipy::Value();
  }
  return aipy::Value(data);
}

aipy::Value writeMethod(const aipy::Value& self, const aipy::Value* args, int count) {
  FileState* state = stateOf(self);
  if (state == nullptr || state->file == nullptr) return aipy::Value(false);
  if (count < 1) return aipy::Value(false);
  const std::string text = args[0].toString();
  if (text.empty()) return aipy::Value(true);
  const std::size_t written =
      std::fwrite(text.data(), 1, text.size(), state->file);
  // 立刻 flush：脚本可能长时间不关文件，「写了但看不见」很难查。
  std::fflush(state->file);
  return aipy::Value(written == text.size());
}

aipy::Value closeMethod(const aipy::Value& self, const aipy::Value*, int) {
  FileState* state = stateOf(self);
  if (state == nullptr || state->file == nullptr) return aipy::Value();
  // 关闭失败（磁盘满）要在 close 上暴露出来，不能静默。
  const bool failed = std::fclose(state->file) != 0;
  state->file = nullptr;
  if (failed) {
    aipy::diagnose(aipy::fs::describeError(state->path, "关闭时写入失败"));
  }
  return aipy::Value();
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
    {"write", &writeMethod},
    {"close", &closeMethod},
    {"__enter__", &enterMethod},
    {"__exit__", &exitMethod},
    {nullptr, nullptr},
};

const aipy::MethodTable kFileTable = {kFileMethods};

aipy::Value openImpl(const aipy::Value* args, int count) {
  // 不给 mode 时按 "r"，与 Python 的 open(path) 一致。
  const std::string path = count >= 1 ? args[0].toString() : std::string();
  const std::string mode = count >= 2 ? args[1].toString() : std::string("r");
  if (path.empty()) return aipy::Value();

  const bool append = mode == "a" || mode == "ab";
  const bool writeMode = mode == "w" || mode == "wb";
  const bool readMode = mode == "r" || mode == "rb";
  if (!append && !writeMode && !readMode) {
    aipy::diagnose(aipy::fs::describeError(
        path, "不支持的 mode（只认 r / w / a）"));
    return aipy::Value();
  }

  void* handle = append || writeMode ? aipy::fs::openForWrite(path, append)
                                     : aipy::fs::openForRead(path);
  if (handle == nullptr) {
    aipy::diagnose(aipy::fs::describeError(path, "打不开"));
    return aipy::Value();
  }

  FileState* state = new (std::nothrow) FileState();
  if (state == nullptr) {
    std::fclose(static_cast<std::FILE*>(handle));
    return aipy::Value();
  }
  state->file = static_cast<std::FILE*>(handle);
  state->path = path;
  return aipy::Value::object(state, &kFileTable, &destroyFile);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, open, openImpl);
