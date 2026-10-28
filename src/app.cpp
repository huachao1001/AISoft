#include "app.h"

#include "dlg_compress.h"
#include "dlg_extract.h"
#include "dlg_input.h"
#include "util.h"

#include <shellapi.h>

App g_app;

// ───────────────────────── 外部启动请求 ─────────────────────────

namespace {

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
  return ::CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

// 打开压缩包；头部加密的包需要密码，失败时弹框询问后重试
bool OpenArchiveWithPassword(arc::Archive& a, const std::wstring& path, HWND owner,
                             arc::OpenResult& r) {
  r = a.Open(path);
  if (r.ok) return true;

  // 头部加密（-mhe=on）的 7z 在无密码时连目录都读不出来，只能提示用户输入
  std::wstring pwd;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!ui::PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true)) return false;
    r = a.Open(path, pwd);
    if (r.ok) return true;
  }
  return false;
}

// 本程序没有主窗口，对话框一律没有 owner。
// 绝不能用 GetForegroundWindow() 当 owner：右键菜单触发时前台是桌面/任务栏，
// 模态循环会把它 EnableWindow(FALSE)，程序一退出就永久锁死整台机器。
constexpr HWND kNoOwner = nullptr;

}  // namespace

LaunchRequest ParseLaunchArgs() {
  LaunchRequest req;
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return req;

  bool actionGiven = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (EqualsNoCase(a, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else if (!a.empty() && a[0] != L'-') {
      req.paths.push_back(a);
    }
  }
  ::LocalFree(argv);

  // 没有动作开关时按后缀推断，这样资源管理器里「打开方式」选 AIZip 也能用
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

std::wstring SerializeLaunch(const LaunchRequest& req) {
  std::wstring s = (req.action == LaunchRequest::Action::kCompress) ? L"-compress" : L"-extract";
  for (const auto& p : req.paths) {
    s += L'\n';
    s += p;
  }
  return s;
}

LaunchRequest DeserializeLaunch(const std::wstring& packed) {
  LaunchRequest req;
  size_t pos = 0;
  bool actionGiven = false;
  while (pos <= packed.size()) {
    const size_t nl = packed.find(L'\n', pos);
    const std::wstring line =
        packed.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
    pos = (nl == std::wstring::npos) ? packed.size() + 1 : nl + 1;
    if (line.empty()) continue;
    if (EqualsNoCase(line, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else {
      req.paths.push_back(line);
    }
  }
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

void AppDispatchLaunch(const LaunchRequest& req) {
  const HWND owner = kNoOwner;

  if (req.action == LaunchRequest::Action::kCompress) {
    if (req.paths.empty()) {
      ::MessageBoxW(owner, L"没有可压缩的文件。", L"AIZip", MB_OK | MB_ICONINFORMATION);
      return;
    }
    ui::ShowCompressDialog(owner, req.paths);
    return;
  }

  // ── 解压 ──
  if (req.paths.empty()) {
    ::MessageBoxW(owner, L"没有指定压缩包。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (req.paths.size() > 1) {
    ::MessageBoxW(owner, L"一次只能解压一个压缩包。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring& path = req.paths[0];
  if (!arc::Archive::IsArchiveFile(path) || !util::PathExists(path)) {
    ::MessageBoxW(owner, (L"不是可识别的压缩包：\n" + path).c_str(), L"AIZip",
                  MB_OK | MB_ICONWARNING);
    return;
  }

  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, owner, r)) {
    if (!r.error.empty()) {
      ::MessageBoxW(owner, (L"无法打开压缩包：\n" + path + L"\n\n" + r.error).c_str(), L"AIZip",
                    MB_OK | MB_ICONERROR);
    }
    return;
  }
  g_app.archivePath = path;
  ui::ShowExtractDialog(owner);
}
