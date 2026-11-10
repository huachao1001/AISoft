// 内核功能验证：打开/枚举/解压/压缩
#include "archive.h"
#include "sevenzip_loader.h"
#include "util.h"

#include <clocale>
#include <cstdio>
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

int wmain(int argc, wchar_t** argv) {
  // err 里是中文，printf %ls 在 C locale 下遇到非 ASCII 会静默丢失
  setlocale(LC_ALL, ".UTF-8");
  if (argc < 3) {
    printf("usage: test_arc <open|extract|compress> ...\n");
    return 2;
  }
  const std::wstring mode = argv[1];

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
    // --pwd=xxx 指定密码，--format=TAR.GZ 指定容器，其余为待压缩路径
    for (int i = 3; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a.rfind(L"--pwd=", 0) == 0) {
        o.password = a.substr(6);
      } else if (a.rfind(L"--format=", 0) == 0) {
        o.formatName = a.substr(9);
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
