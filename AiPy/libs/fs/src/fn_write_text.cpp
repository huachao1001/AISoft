// `fs.write_text(path, text)` —— 覆盖写入，返回是否成功。
//
// 与 Python 的 `open(path, "w").write(text)` 同义。返回 True/False 而不是
// 抛异常，理由见 io.cpp 文件头。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

namespace {

aipy::Value writeTextImpl(const aipy::Value* args, int count) {
  if (count < 2) return aipy::Value(false);
  const std::string path = args[0].toString();
  const std::string content = args[1].toString();
  std::string error;
  if (!aipy::fs::writeAll(path, content, false, &error)) {
    aipy::diagnose(error);
    return aipy::Value(false);
  }
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, write_text, writeTextImpl);
