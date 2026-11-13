// Windows 后端：文件系统、消息框、Shell、文件选择、线程投递、单实例
//
// 这些能力与窗口绘制无关，单独成文件；window_windows.cpp 里的 WindowsPlatform
// 只做转发，保持该文件专注窗口与消息循环。
#include "backend.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace AiUI {
namespace os {
namespace windows {
namespace {

std::wstring trimTrailingSeparators(std::wstring path) {
  while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
    // 保留 "C:\" 这类根路径的盘符分隔符
    if (path.size() == 3 && path[1] == L':') break;
    path.pop_back();
  }
  return path;
}

// FILETIME 自 1601-01-01 起算，每单位 100ns；统一换算成 Unix 纪元毫秒
int64_t fileTimeToUnixMillis(const FILETIME& ft) {
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  constexpr uint64_t kEpochDiff = 116444736000000000ULL;
  if (value.QuadPart <= kEpochDiff) return 0;
  return static_cast<int64_t>((value.QuadPart - kEpochDiff) / 10000ULL);
}

// Unix 纪元毫秒 → FILETIME；millis <= 0 视为未知，返回全零
FILETIME unixMillisToFileTime(int64_t millis) {
  if (millis <= 0) return FILETIME{};
  constexpr uint64_t kEpochDiff = 116444736000000000ULL;
  ULARGE_INTEGER value{};
  value.QuadPart = static_cast<uint64_t>(millis) * 10000ULL + kEpochDiff;
  FILETIME ft{};
  ft.dwLowDateTime = value.LowPart;
  ft.dwHighDateTime = value.HighPart;
  return ft;
}

// ── 单实例 ──
// 用命名互斥判断首实例，用 WM_COPYDATA 转递参数：
// 不依赖网络、不占端口，跨会话也能正确区分。
constexpr const wchar_t* kLaunchMessageClass = L"AiUI.LaunchPayload";
std::wstring g_instanceId;
std::function<void(const std::wstring&)> g_launchHandler;
HWND g_launchWindow = nullptr;
HANDLE g_instanceMutex = nullptr;

LRESULT CALLBACK launchWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_COPYDATA) {
    auto* data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
    if (data && g_launchHandler) {
      const std::wstring payload(static_cast<const wchar_t*>(data->lpData),
                                 data->cbData / sizeof(wchar_t));
      g_launchHandler(payload);
    }
    return TRUE;
  }
  if (msg == WM_CLOSE) {
    ::DestroyWindow(hwnd);
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring classNameFor(const std::wstring& appId) {
  return kLaunchMessageClass + std::wstring(L".") + appId;
}

// ── 线程投递 ──
// 每个参与投递的窗口线程注册一个隐藏窗口接收私有消息，收到后取出队列执行。
// 用队列而不是 WM_APP + 裸指针，是因为 std::function 无法塞进消息参数。
constexpr UINT kMsgRunCall = WM_APP + 100;

struct UiCallQueue {
  std::mutex mutex;
  std::deque<std::function<void()>> calls;
};

// 按线程区分的队列；TLS 让回调在正确的线程上访问自己的队列
thread_local UiCallQueue* t_callQueue = nullptr;
std::mutex g_queueMapMutex;
std::map<DWORD, UiCallQueue*> g_queueMap;
std::map<DWORD, HWND> g_dispatchWindows;

LRESULT CALLBACK dispatchWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == kMsgRunCall) {
    UiCallQueue* queue = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_queueMapMutex);
      auto it = g_queueMap.find(::GetCurrentThreadId());
      if (it != g_queueMap.end()) queue = it->second;
    }
    if (queue) {
      std::deque<std::function<void()>> pending;
      {
        std::lock_guard<std::mutex> lock(queue->mutex);
        pending.swap(queue->calls);
      }
      for (auto& fn : pending) {
        if (fn) fn();
      }
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 确保当前线程有一个接收投递的隐藏窗口
HWND ensureDispatchWindow() {
  const DWORD tid = ::GetCurrentThreadId();
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    auto it = g_dispatchWindows.find(tid);
    if (it != g_dispatchWindows.end() && ::IsWindow(it->second)) return it->second;
  }

  static const wchar_t* kDispatchClass = L"AiUI.UiDispatch";
  static bool registered = false;
  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dispatchWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kDispatchClass;
    ::RegisterClassExW(&wc);
    registered = true;
  }

  HWND hwnd = ::CreateWindowExW(0, kDispatchClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, inst,
                                nullptr);
  if (!hwnd) return nullptr;

  auto* queue = new UiCallQueue();
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    g_queueMap[tid] = queue;
    g_dispatchWindows[tid] = hwnd;
  }
  t_callQueue = queue;
  return hwnd;
}

void postToUiThreadImpl(std::function<void()> fn) {
  if (!fn) return;
  // 调用者可能在工作线程：目标窗口以主窗口所在线程为准
  HWND target = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    if (!g_dispatchWindows.empty()) target = g_dispatchWindows.begin()->second;
  }
  if (!target) return;
  const DWORD targetTid = ::GetWindowThreadProcessId(target, nullptr);

  UiCallQueue* queue = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    auto it = g_queueMap.find(targetTid);
    if (it != g_queueMap.end()) queue = it->second;
  }
  if (!queue) return;
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->calls.push_back(std::move(fn));
  }
  ::PostMessageW(target, kMsgRunCall, 0, 0);
}

void invokeOnUiThreadImpl(std::function<void()> fn) {
  if (!fn) return;

  HWND target = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    if (!g_dispatchWindows.empty()) target = g_dispatchWindows.begin()->second;
  }
  // 没有投递窗口（尚未建 UI）或本来就在 UI 线程：直接跑，避免自锁
  if (!target || ::GetWindowThreadProcessId(target, nullptr) == ::GetCurrentThreadId()) {
    fn();
    return;
  }

  UiCallQueue* queue = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    auto it = g_queueMap.find(::GetWindowThreadProcessId(target, nullptr));
    if (it != g_queueMap.end()) queue = it->second;
  }
  if (!queue) {
    fn();
    return;
  }

  HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!done) {
    fn();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    // 包一层：跑完置事件，调用方才能知道 UI 侧已生效
    queue->calls.push_back([fn, done]() {
      fn();
      ::SetEvent(done);
    });
  }
  ::PostMessageW(target, kMsgRunCall, 0, 0);
  ::WaitForSingleObject(done, INFINITE);
  ::CloseHandle(done);
}

}  // namespace

// ───────────────────────── 文件系统 ─────────────────────────

// ───────────────────────── 文件流 ─────────────────────────
// 压缩内核要随机读写大文件，直接包一层 HANDLE
class WinFileStream final : public FileStream {
 public:
  explicit WinFileStream(HANDLE handle) : handle_(handle) {}
  ~WinFileStream() override {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }

  int64_t read(void* data, int64_t size) override {
    DWORD got = 0;
    // 单次 ReadFile 上限 DWORD，超过就分块
    if (size > 0x7fffffff) size = 0x7fffffff;
    if (!::ReadFile(handle_, data, static_cast<DWORD>(size), &got, nullptr)) return -1;
    return static_cast<int64_t>(got);
  }

  int64_t write(const void* data, int64_t size) override {
    DWORD put = 0;
    if (size > 0x7fffffff) size = 0x7fffffff;
    if (!::WriteFile(handle_, data, static_cast<DWORD>(size), &put, nullptr)) return -1;
    return static_cast<int64_t>(put);
  }

  bool seek(int64_t offset, SeekOrigin origin, uint64_t* position) override {
    DWORD method = FILE_BEGIN;
    if (origin == SeekOrigin::kCurrent) method = FILE_CURRENT;
    else if (origin == SeekOrigin::kEnd) method = FILE_END;
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    LARGE_INTEGER np{};
    if (!::SetFilePointerEx(handle_, li, &np, method)) return false;
    if (position) *position = static_cast<uint64_t>(np.QuadPart);
    return true;
  }

  bool setSize(uint64_t size) override {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
    return ::SetEndOfFile(handle_) != FALSE;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::unique_ptr<FileStream> openFileStreamImpl(const std::wstring& path, bool openWrite) {
  const DWORD access = openWrite ? GENERIC_WRITE : GENERIC_READ;
  const DWORD share = openWrite ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  const DWORD disposition = openWrite ? CREATE_ALWAYS : OPEN_EXISTING;
  HANDLE handle = ::CreateFileW(path.c_str(), access, share, nullptr, disposition,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  return std::make_unique<WinFileStream>(handle);
}

bool moveFileImpl(const std::wstring& from, const std::wstring& to) {
  return ::MoveFileW(from.c_str(), to.c_str()) != FALSE;
}

std::wstring absolutePathImpl(const std::wstring& path) {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD n = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer,
                                     nullptr);
  if (n == 0 || n >= std::size(buffer)) return path;
  return std::wstring(buffer, n);
}

std::wstring executablePathImpl() {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  return std::wstring(buffer, n);
}

std::vector<std::wstring> commandLineArgsImpl() {
  std::vector<std::wstring> out;
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return out;
  if (argc > 1) out.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
  ::LocalFree(argv);
  return out;
}

std::wstring tempDirImpl() {
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
  if (n == 0 || n > std::size(buffer)) return {};
  return std::wstring(buffer, n);
}

uint32_t processIdImpl() { return ::GetCurrentProcessId(); }

std::wstring appDataDirImpl(const std::wstring& appName) {
  wchar_t buffer[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buffer) != S_OK) return {};
  std::wstring dir = std::wstring(buffer) + L"\\" + appName;
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::vector<DirEntry> listDirectoryImpl(const std::wstring& dir, bool skipHidden) {
  std::vector<DirEntry> out;
  const std::wstring pattern = trimTrailingSeparators(dir) + L"\\*";
  WIN32_FIND_DATAW find{};
  HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) return out;
  do {
    if (find.cFileName[0] == L'.') continue;
    const bool isDir = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (skipHidden) {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
      if (find.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
    }
    DirEntry entry;
    entry.name = find.cFileName;
    entry.fullPath = trimTrailingSeparators(dir) + L"\\" + find.cFileName;
    entry.isDir = isDir;
    entry.attributes = find.dwFileAttributes;
    entry.size = isDir ? 0
                       : (static_cast<uint64_t>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;
    entry.modifiedMillis = fileTimeToUnixMillis(find.ftLastWriteTime);
    out.push_back(std::move(entry));
  } while (::FindNextFileW(handle, &find));
  ::FindClose(handle);
  return out;
}

bool pathExistsImpl(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDirectoryImpl(const std::wstring& path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool createDirectoryImpl(const std::wstring& path) {
  if (path.empty()) return false;
  if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

uint64_t fileSizeImpl(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
  return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

int64_t modifiedTimeImpl(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return fileTimeToUnixMillis(data.ftLastWriteTime);
}

bool setModifiedTimeImpl(const std::wstring& path, int64_t millis) {
  if (millis <= 0) return false;
  HANDLE handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const FILETIME ft = unixMillisToFileTime(millis);
  const BOOL ok = ::SetFileTime(handle, nullptr, nullptr, &ft);
  ::CloseHandle(handle);
  return ok != FALSE;
}

std::string readFileImpl(const std::wstring& path, size_t maxBytes) {
  std::string out;
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return out;

  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
      static_cast<uint64_t>(size.QuadPart) <= maxBytes) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!::ReadFile(handle, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
      out.clear();
    } else {
      out.resize(read);
    }
  }
  ::CloseHandle(handle);
  return out;
}

bool writeFileImpl(const std::wstring& path, const std::string& data) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  DWORD written = 0;
  const bool ok = ::WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written,
                              nullptr) &&
                  written == data.size();
  ::CloseHandle(handle);
  return ok;
}

bool deleteFileImpl(const std::wstring& path) {
  if (isDirectoryImpl(path)) return ::RemoveDirectoryW(path.c_str()) != FALSE;
  return ::DeleteFileW(path.c_str()) != FALSE;
}

// 走 Shell 的回收站接口，不能退回 DeleteFileW：误删后还要能还原
bool moveToTrashImpl(const std::wstring& path) {
  if (!pathExistsImpl(path)) return false;
  std::wstring buffer = path;
  buffer.push_back(L'\0');
  buffer.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = buffer.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return ::SHFileOperationW(&op) == 0;
}

std::wstring specialFolderImpl(SpecialFolder folder) {
  // 下载目录没有对应的 CSIDL 常量，统一走 KnownFolder 接口
  const KNOWNFOLDERID* id = nullptr;
  switch (folder) {
    case SpecialFolder::kDesktop: id = &FOLDERID_Desktop; break;
    case SpecialFolder::kHome: id = &FOLDERID_Profile; break;
    case SpecialFolder::kDocuments: id = &FOLDERID_Documents; break;
    case SpecialFolder::kDownloads: id = &FOLDERID_Downloads; break;
    case SpecialFolder::kTemp: id = &FOLDERID_LocalAppData; break;
    default: return {};
  }
  PWSTR path = nullptr;
  if (FAILED(::SHGetKnownFolderPath(*id, 0, nullptr, &path))) return {};
  std::wstring result = path ? path : L"";
  if (path) ::CoTaskMemFree(path);
  return result;
}

std::vector<DriveInfo> listDrivesImpl() {
  std::vector<DriveInfo> out;
  const DWORD drives = ::GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    std::wstring root;
    root.push_back(static_cast<wchar_t>(L'A' + i));
    root += L":\\";
    const UINT type = ::GetDriveTypeW(root.c_str());
    if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
    const bool removable = (type == DRIVE_REMOVABLE || type == DRIVE_CDROM);
    // 空的读卡器槽位不该出现在树里
    if (removable && !pathExistsImpl(root)) continue;

    DriveInfo info;
    info.path = root;
    info.removable = removable;
    wchar_t volume[MAX_PATH] = {};
    if (::GetVolumeInformationW(root.c_str(), volume, MAX_PATH, nullptr, nullptr, nullptr, nullptr,
                                0) &&
        volume[0]) {
      info.label = std::wstring(volume) + L" (" + root.substr(0, 2) + L")";
    } else {
      info.label = root.substr(0, 2);
    }
    out.push_back(std::move(info));
  }
  return out;
}

// ───────────────────────── 消息框 ─────────────────────────

bool canOwnModalImpl(NativeHandle window) {
  if (!window) return false;
  auto* hwnd = static_cast<HWND>(window);
  return ::IsWindow(hwnd) != FALSE && ::IsIconic(hwnd) == FALSE;
}

MessageBoxResult showMessageBoxImpl(NativeHandle owner, const MessageBoxParams& params) {  UINT flags = 0;
  switch (params.icon) {
    case MessageBoxIcon::kInfo: flags |= MB_ICONINFORMATION; break;
    case MessageBoxIcon::kWarning: flags |= MB_ICONWARNING; break;
    case MessageBoxIcon::kError: flags |= MB_ICONERROR; break;
    case MessageBoxIcon::kQuestion: flags |= MB_ICONQUESTION; break;
    default: break;
  }
  switch (params.buttons) {
    case MessageBoxButtons::kOkCancel: flags |= MB_OKCANCEL; break;
    case MessageBoxButtons::kYesNo: flags |= MB_YESNO; break;
    case MessageBoxButtons::kYesNoCancel: flags |= MB_YESNOCANCEL; break;
    default: flags |= MB_OK; break;
  }
  if (params.defaultButton == MessageBoxResult::kNo) flags |= MB_DEFBUTTON2;

  auto* hwnd = static_cast<HWND>(owner);
  if (!hwnd) hwnd = ::GetForegroundWindow();
  const int result =
      ::MessageBoxW(hwnd, params.message.c_str(), params.title.c_str(), flags);
  switch (result) {
    case IDYES: return MessageBoxResult::kYes;
    case IDNO: return MessageBoxResult::kNo;
    case IDCANCEL: return MessageBoxResult::kCancel;
    default: return MessageBoxResult::kOk;
  }
}

// ───────────────────────── 文件选择 ─────────────────────────

std::wstring pickFolderImpl(NativeHandle owner, const std::wstring& title,
                            const std::wstring& initialDir) {
  auto* hwnd = static_cast<HWND>(owner);
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return {};
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  if (!title.empty()) dialog->SetTitle(title.c_str());
  if (!initialDir.empty()) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
      dialog->SetFolder(item);
      item->Release();
    }
  }

  std::wstring result;
  if (SUCCEEDED(dialog->Show(hwnd))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = path;
        ::CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

namespace {

// filters 是「显示名|通配符」成对出现，转成 OPENFILENAME 需要的双 \0 结尾串
std::wstring buildFilterString(const std::vector<std::wstring>& filters) {
  std::wstring out;
  for (const auto& item : filters) {
    out += item;
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

}  // namespace

std::wstring pickSaveFileImpl(NativeHandle owner, const std::wstring& title,
                              const std::wstring& initialDir, const std::wstring& defaultName,
                              const std::wstring& defaultExt,
                              const std::vector<std::wstring>& filters) {
  wchar_t file[MAX_PATH] = {};
  if (!defaultName.empty()) wcscpy_s(file, defaultName.c_str());

  std::wstring filterBuffer = buildFilterString(filters);
  std::wstring titleCopy = title;
  std::wstring dirCopy = initialDir;
  std::wstring extCopy = defaultExt;

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(owner);
  ofn.lpstrFilter = filterBuffer.empty() ? nullptr : filterBuffer.c_str();
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = titleCopy.empty() ? nullptr : titleCopy.c_str();
  ofn.lpstrInitialDir = dirCopy.empty() ? nullptr : dirCopy.c_str();
  ofn.lpstrDefExt = extCopy.empty() ? nullptr : extCopy.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (!::GetSaveFileNameW(&ofn)) return {};
  return std::wstring(file);
}

std::wstring pickOpenFileImpl(NativeHandle owner, const std::wstring& title,
                              const std::wstring& initialDir,
                              const std::vector<std::wstring>& filters) {
  wchar_t file[MAX_PATH] = {};
  std::wstring filterBuffer = buildFilterString(filters);
  std::wstring titleCopy = title;
  std::wstring dirCopy = initialDir;

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(owner);
  ofn.lpstrFilter = filterBuffer.empty() ? nullptr : filterBuffer.c_str();
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = titleCopy.empty() ? nullptr : titleCopy.c_str();
  ofn.lpstrInitialDir = dirCopy.empty() ? nullptr : dirCopy.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

  if (!::GetOpenFileNameW(&ofn)) return {};
  return std::wstring(file);
}

// ───────────────────────── Shell ─────────────────────────

bool openWithSystemImpl(const std::wstring& path) {  const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
  // ShellExecute 的返回值 > 32 才算成功
  return reinterpret_cast<INT_PTR>(result) > 32;
}

bool showPropertiesImpl(NativeHandle owner, const std::wstring& path) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.hwnd = static_cast<HWND>(owner);
  info.lpVerb = L"properties";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOW;
  return ::ShellExecuteExW(&info) != FALSE;
}

// ───────────────────────── 弹出菜单 ─────────────────────────

int showPopupMenuImpl(NativeHandle owner, const Point& position,
                      const std::vector<MenuItem>& items) {
  if (items.empty()) return -1;
  HMENU menu = ::CreatePopupMenu();
  if (!menu) return -1;

  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    if (item.separator) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    UINT flags = MF_STRING;
    if (!item.enabled) flags |= MF_GRAYED;
    if (item.checked) flags |= MF_CHECKED;
    ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(i + 1), item.label.c_str());
  }

  auto* hwnd = static_cast<HWND>(owner);
  if (!hwnd) hwnd = ::GetForegroundWindow();
  ::SetForegroundWindow(hwnd);
  const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x,
                                        position.y, 0, hwnd, nullptr);
  // 菜单结束后补一条空消息：否则下次点别处菜单不会消失
  if (hwnd) ::PostMessageW(hwnd, WM_NULL, 0, 0);
  ::DestroyMenu(menu);
  return command == 0 ? -1 : static_cast<int>(command) - 1;
}

Point cursorPositionImpl() {
  POINT pt{};
  ::GetCursorPos(&pt);
  return Point{pt.x, pt.y};
}

void bringToFrontImpl(NativeHandle window) {
  if (auto* hwnd = static_cast<HWND>(window)) ::SetForegroundWindow(hwnd);
}

// ───────────────────────── 右键菜单注册 ─────────────────────────
// 写 HKCU\Software\Classes，不需要管理员权限

bool regSetString(const std::wstring& subKey, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG r = ::RegSetValueExW(key, name, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

bool regReadString(const std::wstring& subKey, const wchar_t* name, std::wstring& out) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buffer[1024] = {};
  DWORD size = sizeof(buffer);
  DWORD type = 0;
  const LONG r =
      ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
  ::RegCloseKey(key);
  if (r != ERROR_SUCCESS || type != REG_SZ) return false;
  out.assign(buffer, size / sizeof(wchar_t));
  while (!out.empty() && out.back() == L'\0') out.pop_back();
  return true;
}

bool regDeleteValue(const std::wstring& subKey, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
    // 值不存在等同于已清理
    return true;
  }
  const LONG r = ::RegDeleteValueW(key, name);
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool regDeleteTree(const std::wstring& subKey) {
  const LONG r = ::RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
  if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) return true;
  return r == ERROR_SUCCESS;
}

// 生效对象 → 注册表里挂动词的位置。Windows 的约定：
//   "*"        所有文件
//   "Directory" 所有目录
//   ".zip"     该后缀的文件
std::vector<std::wstring> classRootsFor(const std::vector<std::wstring>& targets) {
  std::vector<std::wstring> roots;
  if (targets.empty()) {
    roots.push_back(L"*");
    roots.push_back(L"Directory");
    return roots;
  }
  for (const auto& target : targets) {
    if (target == L"*") roots.push_back(L"*");
    else if (target == L"Directory") roots.push_back(L"Directory");
    else roots.push_back(L"SystemFileAssociations\\" + target);
  }
  return roots;
}

// 排除后缀 → AQS 条件表达式。Windows 的通配符动词没法"减去某几类"，
// 只能靠 AppliesTo 在弹出时过滤；空串表示不加条件。
//   NOT (System.FileExtension:=".zip" OR System.FileExtension:=".7z")
std::wstring appliesToFor(const ShellVerb& verb, const std::wstring& root) {
  // 条件只对通配符类有意义：指定后缀的位置本来就不会命中排除项
  if (verb.excludeTargets.empty() || root != L"*") return {};
  std::wstring expr = L"NOT (";
  for (size_t i = 0; i < verb.excludeTargets.size(); ++i) {
    if (i > 0) expr += L" OR ";
    expr += L"System.FileExtension:=\"" + verb.excludeTargets[i] + L"\"";
  }
  expr += L")";
  return expr;
}

std::wstring registerShellVerbsImpl(const std::vector<ShellVerb>& verbs) {
  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return L"无法取得程序路径";

  for (const auto& verb : verbs) {
    for (const auto& root : classRootsFor(verb.targets)) {
      const std::wstring base = L"Software\\Classes\\" + root + L"\\shell\\" + verb.id;
      const std::wstring cmdKey = base + L"\\command";

      if (!regSetString(base, nullptr, verb.label)) return L"写入菜单文字失败：" + base;
      if (!regSetString(base, L"Icon", exe)) return L"写入图标失败：" + base;
      // 位置显式对齐：旧版本写过的 Top 必须清掉，否则菜单顺序和当前配置对不上
      if (verb.topLevel) {
        if (!regSetString(base, L"Position", L"Top")) return L"写入菜单位置失败：" + base;
      } else if (!regDeleteValue(base, L"Position")) {
        return L"清理菜单位置失败：" + base;
      }
      // 同理：排除项变化后旧的 AppliesTo 必须重写或清掉，否则过滤条件还停在旧值
      const std::wstring appliesTo = appliesToFor(verb, root);
      if (appliesTo.empty()) {
        if (!regDeleteValue(base, L"AppliesTo")) return L"清理菜单条件失败：" + base;
      } else if (!regSetString(base, L"AppliesTo", appliesTo)) {
        return L"写入菜单条件失败：" + base;
      }
      // 静态动词只认 %1，没有 %*。多选时系统会为每个选中项各起一次进程，
      // 各带一个 %1，聚合交给程序内的单实例逻辑。
      // Single 表示多选时不显示这一项；Player 才会在多选时出现。
      const wchar_t* multiSelect = verb.singleSelectionOnly ? L"Single" : L"Player";
      if (!regSetString(base, L"MultiSelectModel", multiSelect)) {
        return L"写入多选模式失败：" + base;
      }
      // command 为空表示"只把路径传给自己"，用于交给程序自己决定怎么处理
      std::wstring cmd = L"\"" + exe + L"\"";
      if (!verb.command.empty()) cmd += L" " + verb.command;
      cmd += L" \"%1\"";
      if (!regSetString(cmdKey, nullptr, cmd)) return L"写入命令行失败：" + cmdKey;
    }
  }

  // 通知资源管理器刷新关联缓存
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return L"";
}

std::wstring unregisterShellVerbsImpl(const std::vector<ShellVerb>& verbs) {
  for (const auto& verb : verbs) {
    for (const auto& root : classRootsFor(verb.targets)) {
      regDeleteTree(L"Software\\Classes\\" + root + L"\\shell\\" + verb.id);
    }
  }
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return L"";
}

bool areShellVerbsRegisteredImpl(const std::vector<ShellVerb>& verbs) {
  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return false;

  for (const auto& verb : verbs) {
    for (const auto& root : classRootsFor(verb.targets)) {
      const std::wstring base = L"Software\\Classes\\" + root + L"\\shell\\" + verb.id;
      std::wstring cmd;
      if (!regReadString(base + L"\\command", nullptr, cmd)) return false;
      // 程序换位置后旧路径失效、动作参数变化后旧命令失效，都视为未注册
      std::wstring expectedCmd = L"\"" + exe + L"\"";
      if (!verb.command.empty()) expectedCmd += L" " + verb.command;
      expectedCmd += L" \"%1\"";
      if (cmd != expectedCmd) return false;

      // 位置与当前配置不一致（例如旧版留下的 Top）也视为未注册，交给注册流程纠正
      std::wstring position;
      const bool hasPosition = regReadString(base, L"Position", position);
      if (hasPosition != verb.topLevel) return false;
      if (hasPosition && position != L"Top") return false;

      // 排除条件同理：不一致就重写，免得菜单过滤还停在旧的后缀表
      const std::wstring expected = appliesToFor(verb, root);
      std::wstring actual;
      if (regReadString(base, L"AppliesTo", actual) != !expected.empty()) return false;
      if (!expected.empty() && actual != expected) return false;

      // 多选模式也要校对：旧版一律写 Player，改配置后必须重写
      std::wstring multiSelect;
      if (!regReadString(base, L"MultiSelectModel", multiSelect)) return false;
      if (multiSelect != (verb.singleSelectionOnly ? L"Single" : L"Player")) return false;
    }
  }
  return true;
}

// ───────────────────────── 进程与线程 ─────────────────────────

bool isUiThreadImpl() {
  HWND target = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    if (!g_dispatchWindows.empty()) target = g_dispatchWindows.begin()->second;
  }
  if (!target) return true;
  return ::GetWindowThreadProcessId(target, nullptr) == ::GetCurrentThreadId();
}

void sleepMillisImpl(int ms) {
  if (ms > 0) ::Sleep(static_cast<DWORD>(ms));
}

float scaleFactorImpl() {
  HDC dc = ::GetDC(nullptr);
  if (!dc) return 1.0f;
  const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
  ::ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi / 96.0f : 1.0f;
}

// ───────────────────────── 线程投递 ─────────────────────────

void prepareUiThreadDispatch() { ensureDispatchWindow(); }

void shutdownUiThreadDispatch() {
  const DWORD tid = ::GetCurrentThreadId();
  HWND hwnd = nullptr;
  UiCallQueue* queue = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_queueMapMutex);
    auto it = g_dispatchWindows.find(tid);
    if (it != g_dispatchWindows.end()) {
      hwnd = it->second;
      g_dispatchWindows.erase(it);
    }
    auto qit = g_queueMap.find(tid);
    if (qit != g_queueMap.end()) {
      queue = qit->second;
      g_queueMap.erase(qit);
    }
  }
  if (hwnd) ::DestroyWindow(hwnd);
  delete queue;
  t_callQueue = nullptr;
}

void postToUiThread(std::function<void()> fn) { postToUiThreadImpl(std::move(fn)); }

void invokeOnUiThread(std::function<void()> fn) { invokeOnUiThreadImpl(std::move(fn)); }

// ───────────────────────── 单实例 ─────────────────────────

bool claimSingleInstanceImpl(const std::wstring& appId) {
  const std::wstring mutexName = L"Local\\AiUI.SingleInstance." + appId;
  HANDLE mutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (!mutex) return true;
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(mutex);
    return false;
  }
  if (g_instanceMutex) ::CloseHandle(g_instanceMutex);
  g_instanceMutex = mutex;
  g_instanceId = appId;

  // 注册接收窗口，另一个实例靠它把参数递过来
  const std::wstring className = classNameFor(appId);
  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  static std::set<std::wstring> registeredClasses;
  if (registeredClasses.find(className) == registeredClasses.end()) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = launchWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = className.c_str();
    ::RegisterClassExW(&wc);
    registeredClasses.insert(className);
  }
  g_launchWindow = ::CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                     nullptr, inst, nullptr);
  return true;
}

void setLaunchPayloadHandlerImpl(std::function<void(const std::wstring&)> fn) {
  g_launchHandler = std::move(fn);
}

bool forwardToRunningInstanceImpl(const std::wstring& appId, const std::wstring& payload) {
  const std::wstring className = classNameFor(appId);
  HWND target = ::FindWindowW(className.c_str(), nullptr);
  if (!target) return false;

  COPYDATASTRUCT data{};
  data.dwData = 1;
  data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
  data.lpData = const_cast<wchar_t*>(payload.c_str());
  ::SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
  // 对端可能被最小化：先还原再置前，否则用户看不到反应
  if (::IsIconic(target)) ::ShowWindow(target, SW_RESTORE);
  ::SetForegroundWindow(target);
  return true;
}

// ───────────────────────── 托盘图标 ─────────────────────────

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 200;
constexpr UINT kTrayIconId = 1;
constexpr const wchar_t* kTrayClassName = L"AiUI.TrayHost";
// 托盘依附一个隐藏窗口：Shell_NotifyIcon 的回调消息必须有个窗口接收，
// 而这个窗口不需要可见，也就不进任务栏。
class WindowsTray final : public PlatformTray {
 public:
  WindowsTray(PlatformTrayHost& host, HINSTANCE instance) : host_(host), instance_(instance) {}

  ~WindowsTray() override {
    hide();
    if (hwnd_) ::DestroyWindow(hwnd_);
  }

  bool ensureWindow() {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WindowsTray::proc;
    wc.hInstance = instance_;
    wc.lpszClassName = kTrayClassName;
    // 类可能已被别的托盘实例注册过，重复注册失败不算错误
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(0, kTrayClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              instance_, this);
    return hwnd_ != nullptr;
  }

  bool show(const std::wstring& tooltip) override {
    if (!ensureWindow()) return false;
    tooltip_ = tooltip;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    // 图标沿用窗口后端那一套取图逻辑，避免托盘和任务栏图标不一致
    data.hIcon = os::windows::loadAppIcon(::GetSystemMetrics(SM_CXSMICON));
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

    if (!::Shell_NotifyIconW(visible_ ? NIM_MODIFY : NIM_ADD, &data)) {
      if (data.hIcon) ::DestroyIcon(data.hIcon);
      return false;
    }
    if (data.hIcon) ::DestroyIcon(data.hIcon);
    visible_ = true;
    return true;
  }

  void hide() override {
    if (!visible_ || !hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    visible_ = false;
  }

  void setTooltip(const std::wstring& tooltip) override {
    if (!visible_) {
      tooltip_ = tooltip;
      return;
    }
    tooltip_ = tooltip;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
  }

 private:
  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
      auto* self = static_cast<WindowsTray*>(
          reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<WindowsTray*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == kTrayCallbackMessage) {
      self->onCallback(LOWORD(lParam));
      return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void onCallback(UINT event) {
    switch (event) {
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
        host_.onTrayActivate();
        break;
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU:
        openMenu();
        break;
      default:
        break;
    }
  }

  // 菜单每次现取：应用改条目不用重新注册图标
  void openMenu() {
    const std::vector<MenuItem> items = host_.trayMenu();
    if (items.empty()) return;

    POINT cursor{};
    ::GetCursorPos(&cursor);
    // 菜单结束后要点别处才消失，先把自己提到前台
    HWND owner = ::GetForegroundWindow();
    if (!owner) owner = hwnd_;
    const int index = showPopupMenuImpl(owner, Point{cursor.x, cursor.y}, items);
    if (index >= 0) host_.onTrayMenuCommand(index);
  }

  PlatformTrayHost& host_;
  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  bool visible_ = false;
  std::wstring tooltip_;
};

}  // namespace

std::unique_ptr<PlatformTray> createTrayIconImpl(PlatformTrayHost& host) {
  return std::make_unique<WindowsTray>(host, ::GetModuleHandleW(nullptr));
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
