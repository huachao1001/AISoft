// `fs.read_text(path)` —— 读整个文件，返回文本。
//
// 与 Python 的 `open(path).read()` 同义。失败返回 None：AiPy 没有异常通道，
// 失败只能靠返回值表示（见 io.cpp 文件头）。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

namespace {

aipy::Value readTextImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  const std::string path = args[0].toString();
  std::string content;
  std::string error;
  if (!aipy::fs::readAll(path, &content, &error)) {
    aipy::diagnose(error);
    return aipy::Value();
  }
  return aipy::Value(content);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, read_text, readTextImpl);
