// `fs.append_text(path, text)` —— 追加写入，返回是否成功。
//
// 与 Python 的 `open(path, "a").write(text)` 同义。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

namespace {

aipy::Value appendTextImpl(const aipy::Value* args, int count) {
  if (count < 2) return aipy::Value(false);
  const std::string path = args[0].toString();
  const std::string content = args[1].toString();
  std::string error;
  if (!aipy::fs::writeAll(path, content, true, &error)) {
    aipy::diagnose(error);
    return aipy::Value(false);
  }
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, append_text, appendTextImpl);
