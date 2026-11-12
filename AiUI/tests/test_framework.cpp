// 测试框架实现
#include "test_framework.h"

#include <cstdio>
#include <exception>

namespace aiui_test {
namespace {

int g_failures = 0;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

void fail(const char* file, int line, std::string message) {
  ++g_failures;
  throw Failure{file, line, std::move(message)};
}

std::string narrow(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (wchar_t ch : text) out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
  return out;
}

}  // namespace aiui_test

int main() {
  using namespace aiui_test;

  // 无缓冲：崩溃时已跑过的用例不会随缓冲区一起丢失
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  int failed = 0;
  int total = 0;
  for (const TestCase& test : registry()) {
    ++total;
    const int before = g_failures;
    try {
      test.fn();
    } catch (const Failure& f) {
      std::printf("    %s(%d): %s\n", f.file, f.line, f.message.c_str());
    } catch (const std::exception& e) {
      std::printf("    未捕获异常：%s\n", e.what());
    } catch (...) {
      std::printf("    未捕获的未知异常\n");
    }
    const bool ok = g_failures == before;
    if (!ok) ++failed;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", test.name);
  }

  std::printf("\n%d / %d 通过\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
