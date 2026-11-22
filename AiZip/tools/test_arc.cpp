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
    printf("  %-40ls dir=%d enc=%d size=%llu pack=%llu crc=%ls m=%ls\n", e.path.c_str(),
           e.isDir ? 1 : 0, e.encrypted ? 1 : 0, (unsigned long long)e.size,
           (unsigned long long)e.packSize, e.hasCrc ? util::FormatCrc(e.crc, true).c_str() : L"",
           e.method.c_str());
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

// 格式表自检：确认可创建格式只剩约定的 3 项，且表中每一项都真的能压出来。
// 这条守的是「别把做不到的格式放回界面」——7-Zip 只有 RAR 解码器没有编码器，
// 一旦有人把 RAR 加进表里，用户点确定只会收到「压缩失败」。
static int TestFormats(const std::wstring& dir) {
  int failures = 0;
  auto expect = [&](bool cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
  };

  const wchar_t* kExpected[] = {L"ZIP", L"7Z", L"TAR", L"GZ", L"TAR.GZ"};
  const int expectedCount = static_cast<int>(sizeof(kExpected) / sizeof(kExpected[0]));
  expect(z7::FormatCount() == expectedCount, "format table has exactly the agreed entries");
  for (int i = 0; i < expectedCount && i < z7::FormatCount(); ++i) {
    expect(z7::Formats()[i].name == std::wstring(kExpected[i]), "format order matches");
  }
  // 已下线的格式不能回到表里：界面上没有它们的位置
  expect(z7::FormatIndexByName(L"RAR") < 0, "RAR is not offered");
  expect(z7::FormatIndexByName(L"TAR.BZ2") < 0, "TAR.BZ2 is not offered");

  // 每种格式各压一次：单文件格式用一个文件，其余用同一个文件即可
  const std::wstring src = util::JoinPath(dir, L"fmt-src.txt");
  FILE* f = nullptr;
  if (_wfopen_s(&f, src.c_str(), L"wb") == 0) {
    fputs("aizip format smoke test payload\n", f);
    fclose(f);
  }

  for (int i = 0; i < z7::FormatCount(); ++i) {
    const z7::FormatInfo& info = z7::Formats()[i];
    const std::wstring out = util::JoinPath(dir, std::wstring(L"out-") + info.name + info.ext);
    AiUI::platform()->deleteFile(out);

    arc::CompressOptions o;
    o.archivePath = out;
    o.formatName = info.name;
    o.level = info.maxLevel == 0 ? 0 : 5;
    std::wstring err;
    const bool ok = arc::Compress({src}, o, nullptr, err);
    const bool created =
        util::PathExists(out) && AiUI::platform()->fileSize(out) > 0;
    printf("  %-8ls -> %ls (%ls)\n", info.name, created ? L"created" : L"missing",
           err.empty() ? L"no error" : err.c_str());
    if (!ok || !created) {
      printf("FAIL %ls compresses for real\n", info.name);
      failures++;
    }
  }
  return failures;
}

// 进度自检：7-Zip 的 zip 编码器分多趟走，收尾趟会把完成量从 0 重新计数。
// 这里验证上报给界面的进度只增不减，否则进度条会从 100% 掉回去。
static int TestProgress(const std::wstring& dir) {
  int failures = 0;
  auto expect = [&](bool cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
  };

  // 64 MB 随机数据：够跨过多个编码块，能稳定复现多趟计数
  const std::wstring src = util::JoinPath(dir, L"prog-src.bin");
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, src.c_str(), L"wb") != 0) {
      printf("FAIL cannot create %ls\n", src.c_str());
      return 1;
    }
    std::vector<char> chunk(1 << 20);
    unsigned seed = 12345;
    for (int i = 0; i < 64; ++i) {
      for (size_t k = 0; k < chunk.size(); ++k) {
        seed = seed * 1103515245u + 12345u;
        chunk[k] = static_cast<char>(seed >> 16);
      }
      fwrite(chunk.data(), 1, chunk.size(), f);
    }
    fclose(f);
  }

  const std::wstring out = util::JoinPath(dir, L"prog-out.zip");
  AiUI::platform()->deleteFile(out);

  arc::CompressOptions o;
  o.archivePath = out;
  o.formatName = L"ZIP";
  o.level = 5;
  std::wstring err;

  uint64_t lastCompleted = 0;
  uint64_t lastTotal = 0;
  int calls = 0;
  int regressions = 0;
  const bool ok = arc::Compress(
      {src}, o,
      [&](uint64_t completed, uint64_t total, const std::wstring&) {
        calls++;
        if (completed < lastCompleted) {
          regressions++;
          printf("  REGRESS call %d: %llu -> %llu (total %llu)\n", calls,
                 (unsigned long long)lastCompleted, (unsigned long long)completed,
                 (unsigned long long)total);
        }
        lastCompleted = completed;
        lastTotal = total;
        return true;
      },
      err);

  printf("  calls=%d lastCompleted=%llu lastTotal=%llu err=%ls\n", calls,
         (unsigned long long)lastCompleted, (unsigned long long)lastTotal, err.c_str());
  expect(ok, "compress succeeds with progress reporting");
  expect(calls > 10, "progress callback fires repeatedly");
  expect(regressions == 0, "reported progress never goes backwards");
  expect(lastTotal > 0 && lastCompleted == lastTotal, "final progress reaches total");

  AiUI::platform()->deleteFile(out);
  AiUI::platform()->deleteFile(src);
  return failures;
}

// 加密包自检：把「口令问题」和「数据问题」区分开，且必须能问到口令。
//
// 守两条曾经踩过的坑：
//   1. 加密包里套加密包（或只解一个条目）时，界面没传密码回调，内核拿不到
//      口令，只报「部分文件解压失败」——用户没有任何地方能输入密码。
//   2. 口令输错时同样只报「部分文件解压失败」，看不出该重新输一遍。
static int TestEncrypted(const std::wstring& dir) {
  int failures = 0;
  auto expect = [&](bool cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
  };

  const std::wstring payload = util::JoinPath(dir, L"enc-payload.txt");
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, payload.c_str(), L"wb") != 0) {
      printf("FAIL cannot create payload\n");
      return 1;
    }
    fputs("secret\n", f);
    fclose(f);
  }

  // ZIP 与 7Z 各压一个加密包：两种容器报「口令错」的途径不同
  // （ZIP 有专门的结果码，7Z 只表现为 CRC 失配），两条路都要覆盖。
  struct Case {
    const wchar_t* format;
    const wchar_t* file;
  };
  const Case cases[] = {{L"ZIP", L"enc-a.zip"}, {L"7Z", L"enc-b.7z"}};
  const std::wstring password = L"p@ss-123";

  for (const Case& c : cases) {
    const std::wstring archivePath = util::JoinPath(dir, c.file);
    AiUI::platform()->deleteFile(archivePath);

    arc::CompressOptions o;
    o.archivePath = archivePath;
    o.formatName = c.format;
    o.password = password;
    std::wstring err;
    if (!arc::Compress({payload}, o, nullptr, err)) {
      printf("FAIL %ls: compress encrypted failed: %ls\n", c.format, err.c_str());
      failures++;
      continue;
    }

    const std::wstring outDir = util::JoinPath(dir, std::wstring(L"enc-out-") + c.file);
    util::RemoveDirectoryTree(outDir);

    {
      arc::Archive a;
      if (!a.Open(archivePath).ok) {
        printf("FAIL %ls: cannot reopen\n", c.format);
        failures++;
        continue;
      }

      // ① 没有密码回调：必须说清是密码问题，而不是含糊的「部分文件失败」
      arc::ExtractOptions eo;
      eo.targetDir = outDir;
      util::EnsureDirectory(outDir);
      const arc::ExtractResult r1 = a.Extract({}, eo, nullptr, nullptr, nullptr);
      printf("  %ls no-callback -> ok=%d canceled=%d err=%ls\n", c.format, r1.ok ? 1 : 0,
             r1.canceled ? 1 : 0, r1.error.c_str());
      expect(!r1.ok && !r1.canceled && r1.error.find(L"密码") != std::wstring::npos,
             "missing password is reported as a password problem");
      expect(a.Password().empty(), "no password is remembered after a failed attempt");

      // ② 密码回调给了正确口令：必须成功，并把口令记在这个包上
      util::RemoveDirectoryTree(outDir);
      util::EnsureDirectory(outDir);
      int asked = 0;
      const arc::ExtractResult r2 = a.Extract(
          {}, eo, nullptr, nullptr,
          [&](std::wstring& out) {
            asked++;
            out = password;
            return true;
          });
      printf("  %ls ask-correct -> ok=%d asked=%d err=%ls\n", c.format, r2.ok ? 1 : 0, asked,
             r2.error.c_str());
      expect(r2.ok, "correct password typed at the prompt succeeds");
      expect(asked == 1, "password is asked exactly once");
      expect(a.Password() == password, "working password is remembered on the archive");
      expect(util::PathExists(util::JoinPath(outDir, L"enc-payload.txt")),
             "encrypted payload really lands on disk");

      // ③ 口令记忆生效：再解一次不该再问
      util::RemoveDirectoryTree(outDir);
      util::EnsureDirectory(outDir);
      asked = 0;
      const arc::ExtractResult r3 = a.Extract(
          {}, eo, nullptr, nullptr,
          [&](std::wstring& out) {
            asked++;
            out = password;
            return true;
          });
      printf("  %ls remembered -> ok=%d asked=%d err=%ls\n", c.format, r3.ok ? 1 : 0, asked,
             r3.error.c_str());
      expect(r3.ok && asked == 0, "remembered password is reused without prompting");

      // ④ 回调取消（返回 false）：必须报 canceled，而不是失败——调用方据此
      //    静默收场，不该再弹「解压失败」。这条正是用户反馈的那个多余弹窗。
      arc::Archive fresh;
      if (fresh.Open(archivePath).ok) {
        util::RemoveDirectoryTree(outDir);
        util::EnsureDirectory(outDir);
        const arc::ExtractResult r4 = fresh.Extract(
            {}, eo, nullptr, nullptr,
            [](std::wstring&) { return false; });
        printf("  %ls declined -> ok=%d canceled=%d err=%ls\n", c.format, r4.ok ? 1 : 0,
               r4.canceled ? 1 : 0, r4.error.c_str());
        expect(!r4.ok && r4.canceled, "declining the prompt is reported as canceled");
        expect(r4.error.empty(), "canceling produces no error message");
      }
    }
  }

  AiUI::platform()->deleteFile(payload);
  return failures;
}

// 文件名编码自检：没有 UTF-8 标志位的 ZIP 用系统 OEM 代码页存名字。
//
// 这里守的是「进程的活动代码页」：清单里一旦声明 activeCodePage=UTF-8，
// GetOEMCP() 就变成 65001，GBK 名字会被整串当成 UTF-8 解，中文全成乱码。
// 断言本身与机器代码页无关——压出来的名字必须能原样读回来。
static int TestNameEncoding(const std::wstring& dir) {
  int failures = 0;
  auto expect = [&](bool cond, const char* what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
  };

  // 直接盯住根因：没有 UTF-8 标志位的 ZIP 名字按 OEM 代码页解，而清单里的
  // activeCodePage=UTF-8 会把进程的 OEM 代码页整体改成 65001，于是 GBK 名字
  // 被当 UTF-8 解成乱码。自压自解测不出这个——自己写出的包带 UTF-8 标志，
  // 怎么都能读回来；真正坏的是别人做的老包。所以这里断言代码页本身。
  printf("  process OEM code page = %u\n", ::GetOEMCP());
  expect(::GetOEMCP() != CP_UTF8, "manifest does not force the process code page to UTF-8");

  const std::wstring name = L"中文名字-测试.txt";
  const std::wstring src = util::JoinPath(dir, name);
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, src.c_str(), L"wb") != 0) {
      printf("FAIL cannot create %ls\n", src.c_str());
      return 1;
    }
    fputs("encoding\n", f);
    fclose(f);
  }

  const std::wstring out = util::JoinPath(dir, L"enc-name.zip");
  AiUI::platform()->deleteFile(out);

  arc::CompressOptions o;
  o.archivePath = out;
  o.formatName = L"ZIP";
  o.level = 0;
  std::wstring err;
  if (!arc::Compress({src}, o, nullptr, err)) {
    printf("FAIL compress failed: %ls\n", err.c_str());
    return 1;
  }

  arc::Archive a;
  if (!a.Open(out).ok) {
    printf("FAIL cannot reopen\n");
    return 1;
  }
  expect(a.Entries().size() == 1, "one entry");
  const std::wstring readBack = a.Entries().empty() ? std::wstring() : a.Entries()[0].path;
  printf("  wrote=[%ls] read=[%ls]\n", name.c_str(), readBack.c_str());
  expect(readBack == name, "non-ASCII file name survives a zip round trip");

  // 解压落地后的名字也要一致：一个文件用两种名字表示会让人以为是两份
  const std::wstring outDir = util::JoinPath(dir, L"enc-name-out");
  util::RemoveDirectoryTree(outDir);
  arc::ExtractOptions eo;
  eo.targetDir = outDir;
  const arc::ExtractResult r = a.Extract({}, eo, nullptr, nullptr, nullptr);
  expect(r.ok, "extracts");
  expect(util::PathExists(util::JoinPath(outDir, name)), "landed file keeps the same name");

  util::RemoveDirectoryTree(outDir);
  AiUI::platform()->deleteFile(out);
  AiUI::platform()->deleteFile(src);
  return failures;
}

int wmain(int argc, wchar_t** argv) {
  // err 里是中文，printf %ls 在 C locale 下遇到非 ASCII 会静默丢失
  setlocale(LC_ALL, ".UTF-8");
  if (argc < 3) {
    printf("usage: test_arc <open|extract|compress|volumes|formats|progress|encrypted|names> ...\n");
    return 2;
  }
  const std::wstring mode = argv[1];

  if (mode == L"volumes") {
    return TestVolumes(argv[2]) == 0 ? 0 : 1;
  }

  if (mode == L"formats") {
    return TestFormats(argv[2]) == 0 ? 0 : 1;
  }

  if (mode == L"progress") {
    return TestProgress(argv[2]) == 0 ? 0 : 1;
  }

  if (mode == L"encrypted") {
    return TestEncrypted(argv[2]) == 0 ? 0 : 1;
  }

  if (mode == L"names") {
    return TestNameEncoding(argv[2]) == 0 ? 0 : 1;
  }

  // open / extract 支持可选密码：test_arc open <file> [password]
  if (mode == L"open") {
    arc::Archive a;
    const std::wstring pwd = argc > 3 ? argv[3] : L"";
    auto r = a.Open(argv[2], pwd);
    // 格式以真正打开它的 handler 为准，扩展名只在 handler 没有名字时兜底
    const std::wstring fmt = a.FormatName();
    printf("open=%d needsPwd=%d fmt=%ls err=%ls\n", r.ok, r.needsPassword,
           fmt.empty() ? z7::FormatNameFromExtension(util::FileExtension(argv[2])) : fmt.c_str(),
           r.error.c_str());
    if (r.ok) Dump(a);
    return r.ok ? 0 : 1;
  }

  if (mode == L"extract") {
    arc::Archive a;
    // 位置参数：argv[4] 可选口令（不以 -- 开头才当口令）。
    // 交互式输入用 --ask=<密码> 模拟，测「问一次 → 重试」的路径。
    std::wstring pwd;
    std::wstring askPwd;
    bool hasAsk = false;
    for (int i = 4; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg.rfind(L"--ask=", 0) == 0) {
        askPwd = arg.substr(6);
        hasAsk = true;
      } else if (arg.rfind(L"--", 0) != 0) {
        pwd = arg;
      }
    }
    auto r = a.Open(argv[2], pwd);
    if (!r.ok) {
      printf("open failed: %ls\n", r.error.c_str());
      return 1;
    }
    arc::ExtractOptions o;
    o.targetDir = argv[3];
    o.password = pwd;
    arc::PasswordFn ask;
    if (hasAsk) {
      ask = [askPwd](std::wstring& out) {
        out = askPwd;
        return !askPwd.empty();
      };
    }
    const arc::ExtractResult res = a.Extract({}, o,
                                             [](uint64_t c, uint64_t t, const std::wstring& p) {
                                               printf("\r%llu/%llu %ls", (unsigned long long)c,
                                                      (unsigned long long)t, p.c_str());
                                               return true;
                                             },
                                             nullptr, ask);
    printf("\nextract=%d canceled=%d err=%ls\n", res.ok ? 1 : 0, res.canceled ? 1 : 0,
           res.error.c_str());
    return res.ok ? 0 : 1;
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
