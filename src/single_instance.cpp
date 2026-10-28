// 单实例聚合：静态动词没有 %*，多选时 shell 会为每个文件各起一个进程（各带 %1）。
// 用命名互斥体分出「收集者」，后到的进程把请求写进收件箱后退出；
// 收集者合并全部路径，只执行一次，避免选十个文件弹十个窗口。
#include "single_instance.h"

namespace inst {
namespace {

constexpr const wchar_t* kMutexName = L"Local\\AIZip.Instance.v2";
constexpr wchar_t kInboxDir[] = L"AIZip.inbox";
constexpr DWORD kSettleMs = 150;    // 每轮等这么久，够 shell 把同批进程拉起来
constexpr DWORD kMaxWaitMs = 2000;  // 上限，避免异常情况一直等

HANDLE g_mutex = nullptr;

std::wstring InboxDir() {
  wchar_t tmp[MAX_PATH]{};
  if (!::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp)) return L"";
  return std::wstring(tmp) + kInboxDir;
}

// 请求落盘：动作一行 + 每个路径一行，复用 SerializeLaunch 的格式
bool WriteRequest(const std::wstring& file, const LaunchRequest& req) {
  HANDLE h = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  const std::wstring buf = SerializeLaunch(req) + L'\n';
  DWORD written = 0;
  const DWORD bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
  const bool ok = ::WriteFile(h, buf.data(), bytes, &written, nullptr) && written == bytes;
  ::CloseHandle(h);
  return ok;
}

bool ReadRequest(const std::wstring& file, LaunchRequest& out) {
  HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size{};
  bool ok = false;
  if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
    std::wstring buf(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
    DWORD read = 0;
    if (::ReadFile(h, buf.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) && read > 0) {
      buf.resize(read / sizeof(wchar_t));
      out = DeserializeLaunch(buf);
      ok = true;
    }
  }
  ::CloseHandle(h);
  return ok;
}

bool Forward(const LaunchRequest& req) {
  const std::wstring dir = InboxDir();
  if (dir.empty()) return false;
  util::EnsureDirectory(dir);
  const std::wstring name = std::to_wstring(::GetCurrentProcessId()) + L"_" +
                            std::to_wstring(::GetTickCount64()) + L".req";
  return WriteRequest(util::JoinPath(dir, name), req);
}

// 取走收件箱里的请求并删除；返回条数。超过 10 秒的残留（崩溃遗留）直接丢弃
int DrainInbox(std::vector<LaunchRequest>& out) {
  const std::wstring dir = InboxDir();
  if (dir.empty()) return 0;

  const std::wstring pattern = util::JoinPath(dir, L"*.req");
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;

  FILETIME nowFt{};
  ::GetSystemTimeAsFileTime(&nowFt);
  ULARGE_INTEGER now{}, written{};
  now.LowPart = nowFt.dwLowDateTime;
  now.HighPart = nowFt.dwHighDateTime;

  int count = 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

    const std::wstring file = util::JoinPath(dir, fd.cFileName);
    written.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    written.HighPart = fd.ftLastWriteTime.dwHighDateTime;

    // FILETIME 单位是 100ns：10 秒 = 1e8
    const bool stale = now.QuadPart > written.QuadPart &&
                       (now.QuadPart - written.QuadPart) > 100000000ull;
    if (!stale) {
      LaunchRequest req;
      if (ReadRequest(file, req) && !req.paths.empty()) {
        out.push_back(std::move(req));
        ++count;
      }
    }
    ::DeleteFileW(file.c_str());
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return count;
}

// 动作以先到的为准，路径依次拼接
void MergeInto(LaunchRequest& into, const LaunchRequest& more) {
  if (more.paths.empty()) return;
  if (into.paths.empty()) into.action = more.action;
  into.paths.insert(into.paths.end(), more.paths.begin(), more.paths.end());
}

}  // namespace

bool Collect(const LaunchRequest& first, LaunchRequest& merged) {
  g_mutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
  if (!g_mutex) {  // 拿不到互斥体就退化成不聚合，功能仍可用
    merged = first;
    return true;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(g_mutex);
    g_mutex = nullptr;
    Forward(first);
    return false;
  }

  // 本进程是这一批的第一个，互斥体持有到进程结束
  merged = first;

  const DWORD start = ::GetTickCount();
  for (;;) {
    ::Sleep(kSettleMs);

    std::vector<LaunchRequest> batch;
    const int got = DrainInbox(batch);
    for (const auto& r : batch) MergeInto(merged, r);

    if (got == 0) break;  // 这一轮没人再来，说明同批进程都投递完了
    if (::GetTickCount() - start >= kMaxWaitMs) break;
  }
  return true;
}

LaunchRequest TakePending() {
  LaunchRequest req;
  std::vector<LaunchRequest> batch;
  if (DrainInbox(batch) == 0) return req;
  req = std::move(batch.front());
  for (size_t i = 1; i < batch.size(); ++i) MergeInto(req, batch[i]);
  return req;
}

void Shutdown() {
  if (g_mutex) {
    ::ReleaseMutex(g_mutex);
    ::CloseHandle(g_mutex);
    g_mutex = nullptr;
  }
}

}  // namespace inst
