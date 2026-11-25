// 子进程的实现。
//
// ── 引号规则 ──
//
// Windows 没有 argv，只接受一条命令行字符串，由 CRT 再拆回参数。规则是
// 「含空格/引号就整体加引号，内部引号双写、引号前后的反斜杠加倍」。这条
// 规则写错的典型症状是「路径里有空格时 linker 报找不到文件」，而不是
// 进程起不来——所以不能靠试。
//
// ── 为什么两个管道要同时读 ──
//
// 只读一个、等另一个，会在对方写满管道缓冲（约 4 KB）时互相死锁。
// link.exe 的输出可能上千行，必须并发读。
#include "pcc/backend/process.h"

#include <windows.h>

#include <map>

namespace pcc {
namespace {

std::wstring toWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &wide[0], length);
  return wide;
}

std::string toUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::string();
  std::string text(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), &text[0], length,
                        nullptr, nullptr);
  return text;
}

// 按 CRT 的解析规则给一个参数加引号。
std::wstring quoteArgument(const std::wstring& argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      // 引号前的反斜杠要加倍，再加一个转义引号。
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(ch);
  }
  // 结尾反斜杠要加倍，否则会把收尾引号转义掉。
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

// 父进程环境 + 增量覆盖。值与键都按 UTF-16 处理。
std::wstring buildEnvironment(const std::vector<std::string>& overrides) {
  std::map<std::wstring, std::wstring> values;
  if (LPWCH block = ::GetEnvironmentStringsW()) {
    for (LPWCH at = block; *at != L'\0';) {
      const std::wstring entry = at;
      at += entry.size() + 1;
      // `=C:=...` 这类以 '=' 开头的伪变量跳过：它不能被覆盖。
      const std::size_t split = entry.find(L'=');
      if (split == std::wstring::npos || split == 0) continue;
      values[entry.substr(0, split)] = entry.substr(split + 1);
    }
    ::FreeEnvironmentStringsW(block);
  }
  for (const std::string& entry : overrides) {
    const std::wstring wide = toWide(entry);
    const std::size_t split = wide.find(L'=');
    if (split == std::wstring::npos) continue;
    values[wide.substr(0, split)] = wide.substr(split + 1);
  }

  std::wstring block;
  for (const auto& item : values) {
    block += item.first;
    block.push_back(L'=');
    block += item.second;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

// 读走管道里当前可读的数据。返回 false 表示管道已关闭。
bool drainAvailable(HANDLE pipe, std::string* out) {
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return false;
    }
    if (available == 0) return true;

    char buffer[4096];
    const DWORD want = available < sizeof(buffer)
                           ? available
                           : static_cast<DWORD>(sizeof(buffer));
    DWORD got = 0;
    if (!::ReadFile(pipe, buffer, want, &got, nullptr) || got == 0) {
      return false;
    }
    out->append(buffer, got);
  }
}

}  // namespace

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         const std::vector<std::string>& environment) {
  ProcessResult result;
  if (arguments.empty()) return result;

  std::wstring commandLine;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i > 0) commandLine.push_back(L' ');
    commandLine += quoteArgument(toWide(arguments[i]));
  }

  SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE outRead = nullptr;
  HANDLE outWrite = nullptr;
  HANDLE errRead = nullptr;
  HANDLE errWrite = nullptr;
  if (!::CreatePipe(&outRead, &outWrite, &attributes, 0)) return result;
  if (!::CreatePipe(&errRead, &errWrite, &attributes, 0)) {
    ::CloseHandle(outRead);
    ::CloseHandle(outWrite);
    return result;
  }
  ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup;
  ::ZeroMemory(&startup, sizeof(startup));
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = outWrite;
  startup.hStdError = errWrite;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process;
  ::ZeroMemory(&process, sizeof(process));

  std::wstring environmentBlock = buildEnvironment(environment);
  std::wstring mutableLine = commandLine;
  // CREATE_UNICODE_ENVIRONMENT 不可省：环境块是 UTF-16，缺这个标志系统会
  // 按 ANSI 解析，CreateProcess 直接报 ERROR_INVALID_PARAMETER(87)。
  const BOOL created = ::CreateProcessW(
      nullptr, &mutableLine[0], nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, &environmentBlock[0],
      nullptr, &startup, &process);
  ::CloseHandle(outWrite);
  ::CloseHandle(errWrite);

  if (!created) {
    result.lastError = ::GetLastError();
    ::CloseHandle(outRead);
    ::CloseHandle(errRead);
    return result;
  }
  result.started = true;

  // 两个管道轮流排空，直到都到 EOF（子进程退出 + 写端已关闭）。
  for (;;) {
    const bool outOpen = drainAvailable(outRead, &result.out);
    const bool errOpen = drainAvailable(errRead, &result.err);
    if (!outOpen && !errOpen) break;
    // 阻塞等一小段：纯自旋会白烧一个核，等太久又会攒满管道缓冲。
    ::WaitForSingleObject(process.hProcess, 20);
  }

  ::CloseHandle(outRead);
  ::CloseHandle(errRead);
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 0;
  ::GetExitCodeProcess(process.hProcess, &exitCode);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(process.hThread);

  result.exitCode = static_cast<int>(exitCode);
  return result;
}

}  // namespace pcc
