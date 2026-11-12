// 内核功能验证：打开/枚举/解压/压缩
#include "archive.h"
#include "sevenzip_loader.h"
#include "util.h"
#include "volumes.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void Dump(const arc::Archive& a) {
  printf("entries=%zu\n", a.Entries().size());
  for (const auto& e : a.Entries()) {
    printf("  %-40ls dir=%d size=%llu pack=%llu crc=%ls m=%ls\n", e.path.c_str(), e.isDir ? 1 : 0,
           (unsigned long long)e.size, (unsigned long long)e.packSize,
           e.hasCrc ? util::FormatCrc(e.crc, true).c_str() : L"", e.method.c_str());
  }
}

// 分卷读写自检：命名、收集、清理，不依赖 7-Zip
static int TestVolumes(const std::wstring& dir) {
  int failures = 0;
  auto expect = [&](bool cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
  };

  const std::wstring base = util::JoinPath(dir, L"vtest");
  arc::RemoveVolumes(base);
  expect(arc::CollectVolumes(base + L".001").empty(), "no volumes initially");

  const std::wstring names[] = {L"a", L"b", L"c"};
  for (int i = 1; i <= 3; ++i) {
    const std::wstring path = arc::VolumePath(base, i);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0) {
      fwrite(names[i - 1].c_str(), sizeof(wchar_t), 1, f);
      fclose(f);
    }
  }

  expect(arc::VolumePath(base, 7) == base + L".007", "volume path is zero padded");
  expect(arc::VolumePath(base, 1000) == base + L".1000", "volume path widens past 999");

  const std::vector<std::wstring> found = arc::CollectVolumes(base + L".001");
  expect(found.size() == 3, "collects all three volumes");
  expect(found.empty() || found.front() == base + L".001", "first volume is .001");

  // 从 .002 打开没有意义，必须返回空
  expect(arc::CollectVolumes(base + L".002").empty(), "rejects non-first volume");

  arc::RemoveVolumes(base);
  expect(arc::CollectVolumes(base + L".001").empty(), "removes all volumes");
  expect(!util::PathExists(base + L".001") && !util::PathExists(base + L".003"),
         "volume files are gone");
  return failures;
}

int wmain(int argc, wchar_t** argv) {
  // err 里是中文，printf %ls 在 C locale 下遇到非 ASCII 会静默丢失
  setlocale(LC_ALL, ".UTF-8");
  if (argc < 3) {
    printf("usage: test_arc <open|extract|compress|volumes> ...\n");
    return 2;
  }
  const std::wstring mode = argv[1];

  if (mode == L"volumes") {
    return TestVolumes(argv[2]) == 0 ? 0 : 1;
  }

  // open / extract 支持可选密码：test_arc open <file> [password]
  if (mode == L"open") {
    arc::Archive a;
    const std::wstring pwd = argc > 3 ? argv[3] : L"";
    auto r = a.Open(argv[2], pwd);
    printf("open=%d fmt=%ls err=%ls\n", r.ok,
           z7::FormatNameFromExtension(util::FileExtension(argv[2])), r.error.c_str());
    if (r.ok) Dump(a);
    return r.ok ? 0 : 1;
  }

  if (mode == L"extract") {
    arc::Archive a;
    const std::wstring pwd = argc > 4 ? argv[4] : L"";
    auto r = a.Open(argv[2], pwd);
    if (!r.ok) {
      printf("open failed: %ls\n", r.error.c_str());
      return 1;
    }
    arc::ExtractOptions o;
    o.targetDir = argv[3];
    o.password = pwd;
    std::wstring err;
    const bool ok = a.Extract({}, o,
                              [](uint64_t c, uint64_t t, const std::wstring& p) {
                                printf("\r%llu/%llu %ls", (unsigned long long)c,
                                       (unsigned long long)t, p.c_str());
                                return true;
                              },
                              nullptr, nullptr, err);
    printf("\nextract=%d err=%ls\n", ok, err.c_str());
    return ok ? 0 : 1;
  }

  if (mode == L"compress") {
    arc::CompressOptions o;
    o.archivePath = argv[2];
    std::vector<std::wstring> srcs;
    // --pwd=xxx 指定密码，--format=TAR.GZ 指定容器，--volume=字节数 分卷，
    // 其余为待压缩路径
    for (int i = 3; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a.rfind(L"--pwd=", 0) == 0) {
        o.password = a.substr(6);
      } else if (a.rfind(L"--format=", 0) == 0) {
        o.formatName = a.substr(9);
      } else if (a.rfind(L"--volume=", 0) == 0) {
        o.volumeSize = _wcstoui64(a.c_str() + 9, nullptr, 10);
      } else {
        srcs.push_back(a);
      }
    }
    std::wstring err;
    const bool ok = arc::Compress(srcs, o,
                                  [](uint64_t c, uint64_t t, const std::wstring& p) {
                                    printf("\r%llu/%llu %ls", (unsigned long long)c,
                                           (unsigned long long)t, p.c_str());
                                    return true;
                                  },
                                  err);
    printf("\ncompress=%d err=%ls\n", ok, err.c_str());
    return ok ? 0 : 1;
  }

  printf("unknown mode\n");
  return 2;
}
