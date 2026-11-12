// 单实例聚合：静态动词没有 %*，多选时系统会为每个文件各起一个进程（各带 %1）。
// 用平台层的单实例锁分出「收集者」，后到的进程把请求写进收件箱后退出；
// 收集者合并全部路径，只执行一次，避免选十个文件弹十个窗口。
// 锁只在收集窗口内持有，收集完就释放——这样对话框开着时再右键仍能独立开一个操作。
#include "single_instance.h"

#include "util.h"
#include "AiUI/platform.h"

#include <vector>

namespace inst {
namespace {

constexpr const wchar_t* kInstanceId = L"AIZip.Instance.v2";
constexpr const wchar_t* kInboxDirName = L"AIZip.inbox";
constexpr int kSettleMs = 150;    // 每轮等这么久，够系统把同批进程拉起来
constexpr int kMaxWaitMs = 2000;  // 上限，避免异常情况一直等
constexpr int64_t kStaleMillis = 10000;  // 超过这么久的是崩溃遗留
std::wstring inboxDir() {
  const std::wstring tmp = AiUI::platform()->tempDir();
  if (tmp.empty()) return {};
  return util::JoinPath(tmp, kInboxDirName);
}

// 请求落盘：动作一行 + 每个路径一行，复用 SerializeLaunch 的格式
bool writeRequest(const std::wstring& file, const LaunchRequest& req) {
  // 统一按 UTF-8 存，跨平台一致
  const std::string payload = util::toUtf8(SerializeLaunch(req) + L'\n');
  return AiUI::platform()->writeFile(file, payload);
}

bool readRequest(const std::wstring& file, LaunchRequest& out) {
  const std::string raw = AiUI::platform()->readFile(file, 1 << 20);
  if (raw.empty()) return false;
  out = DeserializeLaunch(util::fromUtf8(raw));
  return true;
}

bool forward(const LaunchRequest& req) {
  const std::wstring dir = inboxDir();
  if (dir.empty()) return false;
  util::EnsureDirectory(dir);
  const std::wstring name = std::to_wstring(AiUI::platform()->processId()) + L"_" +
                            std::to_wstring(AiUI::platform()->monotonicMillis()) + L".req";
  return writeRequest(util::JoinPath(dir, name), req);
}

// 取走收件箱里的请求并删除；返回条数。过期残留（崩溃遗留）直接丢弃
int drainInbox(std::vector<LaunchRequest>& out) {
  const std::wstring dir = inboxDir();
  if (dir.empty()) return 0;

  const int64_t now = AiUI::platform()->monotonicMillis();
  int count = 0;
  for (const auto& entry : AiUI::platform()->listDirectory(dir, false)) {
    if (entry.isDir) continue;
    const std::wstring ext = util::FileExtension(entry.name);
    if (ext != L".req") continue;

    const std::wstring file = entry.fullPath;
    // 时间戳只用于判断残留；平台给不出时按 0 处理，不会误删刚写好的
    const bool stale = entry.modifiedMillis > 0 && now > 0 &&
                       entry.modifiedMillis + kStaleMillis < now;
    if (!stale) {
      LaunchRequest req;
      if (readRequest(file, req) && !req.paths.empty()) {
        out.push_back(std::move(req));
        ++count;
      }
    }
    AiUI::platform()->deleteFile(file);
  }
  return count;
}

// 动作以先到的为准，路径依次拼接
void mergeInto(LaunchRequest& into, const LaunchRequest& more) {
  if (more.paths.empty()) return;
  if (into.paths.empty()) into.action = more.action;
  into.paths.insert(into.paths.end(), more.paths.begin(), more.paths.end());
}

}  // namespace

bool Collect(const LaunchRequest& first, LaunchRequest& merged) {
  auto* platform = AiUI::platform();
  if (!platform->claimSingleInstance(kInstanceId)) {
    // 已有收集者在跑：把请求交过去，本进程可以直接退出
    forward(first);
    return false;
  }

  merged = first;

  // 等一小会儿，把同时拉起的那批进程收齐
  const int64_t start = platform->monotonicMillis();
  for (;;) {
    platform->sleepMillis(kSettleMs);

    std::vector<LaunchRequest> batch;
    const int got = drainInbox(batch);
    for (const auto& r : batch) mergeInto(merged, r);

    if (got == 0 || platform->monotonicMillis() - start >= kMaxWaitMs) break;
  }

  // 收集结束立刻释放：对话框开着时再右键，新进程能独立成为收集者
  return true;
}

}  // namespace inst
