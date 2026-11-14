// 打包器：把「通用安装器 + 清单 + 载荷」合成一个成品安装包
//
// 用法：
//   aiinstaller_pack --installer <AiSetup.exe> --manifest <setup.ini>
//                    --root <文件根目录> --out <产物.exe>
//                    [--icon <icon.ico>] [--verbose]
//
// 顺序很关键：先复制安装器、再改资源、最后追加载荷。
// 反过来不行——UpdateResource 会重排整个 PE 映像，末尾追加的数据会被丢掉。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <io.h>
#include <string>
#include <vector>

#include "AiInstaller/manifest.h"
#include "AiInstaller/package.h"

namespace {

// ── 输出 ──
//
// 不能用 fwprintf 写中文：程序跑在默认 C locale 下，宽字符转字节时会因为
// 目标 locale 表达不了中文而整体失败，连一个字节都不写出去。构建期这个
// 工具的输出要能被日志捕获，所以自己转 UTF-8 再用 fwrite 写；接了控制台
// 时走 WriteConsoleW，避免中文变成乱码。

// 把宽字符转成 UTF-8 字节
std::string toUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

void writeToStream(FILE* stream, const std::wstring& text) {
  if (stream == stderr) ::fflush(stdout);

  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
  DWORD mode = 0;
  // 控制台用宽字符接口，其余（管道、文件）写 UTF-8 字节
  if (handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode)) {
    DWORD written = 0;
    ::WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
  } else {
    const std::string bytes = toUtf8(text);
    ::fwrite(bytes.data(), 1, bytes.size(), stream);
  }
  ::fflush(stream);
}

void writeOut(const std::wstring& text) { writeToStream(stdout, text); }

void writeError(const std::wstring& text) { writeToStream(stderr, text + L"\n"); }

// ── 命令行 ──

struct Options {
  std::wstring installer;
  std::wstring manifest;
  std::wstring root;
  std::wstring out;
  std::wstring icon;
  bool verbose = false;
};

// 支持 "--name value" 与 "--name=value" 两种写法
bool takeValue(const std::vector<std::wstring>& args, size_t& i, const std::wstring& name,
               std::wstring& target) {
  const std::wstring& arg = args[i];
  if (arg == name) {
    if (i + 1 >= args.size()) {
      writeError(L"参数缺少取值：" + name);
      return false;
    }
    target = args[++i];
    return true;
  }
  const std::wstring prefix = name + L"=";
  if (arg.rfind(prefix, 0) == 0) {
    target = arg.substr(prefix.size());
    return true;
  }
  return false;
}

bool parseOptions(const std::vector<std::wstring>& args, Options& out) {
  for (size_t i = 1; i < args.size(); ++i) {
    const std::wstring& arg = args[i];
    if (arg == L"--verbose") {
      out.verbose = true;
      continue;
    }
    if (takeValue(args, i, L"--installer", out.installer)) continue;
    if (takeValue(args, i, L"--manifest", out.manifest)) continue;
    if (takeValue(args, i, L"--root", out.root)) continue;
    if (takeValue(args, i, L"--out", out.out)) continue;
    if (takeValue(args, i, L"--icon", out.icon)) continue;

    writeError(L"未知参数：" + arg + L"\n\n用法：\n  aiinstaller_pack --installer <AiSetup.exe> --manifest <setup.ini> "
               L"--root <目录> --out <产物.exe> [--icon <icon.ico>] [--verbose]");
    return false;
  }

  if (out.installer.empty() || out.manifest.empty() || out.root.empty() || out.out.empty()) {
    writeError(L"缺少必需参数：--installer --manifest --root --out");
    return false;
  }
  return true;
}

// ── 文件读写 ──

bool readWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(handle, &size)) {
    ::CloseHandle(handle);
    return false;
  }
  out.resize(static_cast<size_t>(size.QuadPart));

  size_t offset = 0;
  while (offset < out.size()) {
    DWORD got = 0;
    const size_t left = out.size() - offset;
    const DWORD want = static_cast<DWORD>(left > 0x10000000 ? 0x10000000 : left);
    if (!::ReadFile(handle, out.data() + offset, want, &got, nullptr) || got == 0) {
      ::CloseHandle(handle);
      return false;
    }
    offset += got;
  }
  ::CloseHandle(handle);
  return true;
}

// 取文件大小。打包前要先知道每个文件多长，才能算出偏移写进清单。
bool fileSize(const std::wstring& path, uint64_t& out) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
  out = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  return true;
}

// 把一个文件原样追加到输出尾部。
//
// 分块而不是整读：载荷可能有几百 MB，整读会把内存撑爆；而且这里只是搬运，
// 不需要看内容。
bool appendFile(const std::wstring& path, std::vector<uint8_t>& out) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  std::vector<uint8_t> buffer(1 << 20);
  for (;;) {
    DWORD got = 0;
    if (!::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
      ::CloseHandle(handle);
      return false;
    }
    if (got == 0) break;
    out.insert(out.end(), buffer.data(), buffer.data() + got);
  }
  ::CloseHandle(handle);
  return true;
}

bool writeWholeFile(const std::wstring& path, const std::vector<uint8_t>& data) {  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  size_t offset = 0;
  while (offset < data.size()) {
    DWORD put = 0;
    const size_t left = data.size() - offset;
    const DWORD want = static_cast<DWORD>(left > 0x10000000 ? 0x10000000 : left);
    if (!::WriteFile(handle, data.data() + offset, want, &put, nullptr) || put == 0) {
      ::CloseHandle(handle);
      return false;
    }
    offset += put;
  }
  ::CloseHandle(handle);
  return true;
}

std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// ── 图标改写 ──

// .ico 文件结构：ICONDIR(6) + N 个 ICONDIRENTRY(16) + 各尺寸的位图数据。
// Windows 取图标不是读 RT_ICON 里的原始 .ico，而是顺着 RT_GROUP_ICON 那张
// 索引表找到编号对应的 RT_ICON。所以必须两样都写：位图按编号存、索引表
// 指向这些编号。直接把整个 .ico 塞进一个资源是取不到图标的。
struct IconImage {
  uint8_t width = 0;
  uint8_t height = 0;
  uint8_t colorCount = 0;
  uint16_t planes = 0;
  uint16_t bitCount = 0;
  std::vector<uint8_t> data;
};

bool parseIco(const std::vector<uint8_t>& ico, std::vector<IconImage>& out) {
  if (ico.size() < 6) return false;
  if (readU16(ico.data()) != 0) return false;
  if (readU16(ico.data() + 2) != 1) return false;  // 1 = 图标

  const uint16_t count = readU16(ico.data() + 4);
  if (count == 0) return false;
  if (ico.size() < 6 + static_cast<size_t>(count) * 16) return false;

  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* entry = ico.data() + 6 + static_cast<size_t>(i) * 16;
    IconImage image;
    image.width = entry[0];
    image.height = entry[1];
    image.colorCount = entry[2];
    image.planes = readU16(entry + 4);
    image.bitCount = readU16(entry + 6);

    const uint32_t bytes = readU32(entry + 8);
    const uint32_t offset = readU32(entry + 12);
    if (offset > ico.size() || bytes > ico.size() - offset) return false;

    image.data.assign(ico.begin() + offset, ico.begin() + offset + bytes);
    out.push_back(std::move(image));
  }
  return true;
}

BOOL CALLBACK enumResName(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  (void)module;
  (void)type;
  auto* names = reinterpret_cast<std::vector<std::wstring>*>(param);
  // 整数 id 用 MAKEINTRESOURCE 表示，指针高位为零，用这个判断
  if (reinterpret_cast<ULONG_PTR>(name) >> 16) {
    names->push_back(name);
  }
  return TRUE;
}

// 把图标换进 exe 副本。
//
// 先删掉原有的图标组，再按新图写位图与索引表：留着旧组的话，资源管理器
// 可能按编号顺序取到旧图标，换图标就时灵时不灵。
bool applyIcon(const std::wstring& exePath, const std::wstring& iconPath) {
  std::vector<uint8_t> ico;
  if (!readWholeFile(iconPath, ico)) {
    writeError(L"读不出图标：" + iconPath);
    return false;
  }

  std::vector<IconImage> images;
  if (!parseIco(ico, images)) {
    writeError(L"图标文件格式不对：" + iconPath);
    return false;
  }

  // 收集旧图标组的名字，稍后逐个删除
  std::vector<std::wstring> oldGroups;
  HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (module) {
    ::EnumResourceNamesW(module, RT_GROUP_ICON, &enumResName,
                         reinterpret_cast<LONG_PTR>(&oldGroups));
    ::FreeLibrary(module);
  }

  HANDLE update = ::BeginUpdateResourceW(exePath.c_str(), FALSE);
  if (!update) {
    writeError(L"无法打开资源句柄：" + exePath);
    return false;
  }

  const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);

  for (const auto& name : oldGroups) {
    ::UpdateResourceW(update, RT_GROUP_ICON, name.c_str(), lang, nullptr, 0);
  }

  // 位图按 1..N 编号，索引表用同样的编号。0 保留给旧资源，避开。
  for (size_t i = 0; i < images.size(); ++i) {
    const WORD id = static_cast<WORD>(i + 1);
    ::UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(id), lang,
                      const_cast<uint8_t*>(images[i].data.data()),
                      static_cast<DWORD>(images[i].data.size()));
  }

  // 索引表：GRPICONDIR(6) + N 个 GRPICONDIRENTRY(14)
  std::vector<uint8_t> group;
  group.resize(6);
  group[0] = 0;
  group[1] = 0;
  group[2] = 1;  // type = icon
  group[3] = 0;
  group[4] = static_cast<uint8_t>(images.size() & 0xFF);
  group[5] = static_cast<uint8_t>((images.size() >> 8) & 0xFF);

  for (size_t i = 0; i < images.size(); ++i) {
    const IconImage& image = images[i];
    const WORD id = static_cast<WORD>(i + 1);
    group.push_back(image.width);
    group.push_back(image.height);
    group.push_back(image.colorCount);
    group.push_back(0);
    group.push_back(static_cast<uint8_t>(image.planes & 0xFF));
    group.push_back(static_cast<uint8_t>((image.planes >> 8) & 0xFF));
    group.push_back(static_cast<uint8_t>(image.bitCount & 0xFF));
    group.push_back(static_cast<uint8_t>((image.bitCount >> 8) & 0xFF));
    const uint32_t bytes = static_cast<uint32_t>(image.data.size());
    for (int b = 0; b < 4; ++b) group.push_back(static_cast<uint8_t>((bytes >> (b * 8)) & 0xFF));
    group.push_back(static_cast<uint8_t>(id & 0xFF));
    group.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
  }

  // 编号 1 = 主图标组。资源管理器取的就是编号最小的那个组。
  const bool ok = ::UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang,
                                    const_cast<uint8_t*>(group.data()),
                                    static_cast<DWORD>(group.size())) != FALSE;

  if (!::EndUpdateResourceW(update, FALSE)) {
    writeError(L"提交图标资源失败：" + exePath);
    return false;
  }
  return ok;
}

// ── 版本资源 ──

// 生成 VS_VERSIONINFO。
//
// 资源管理器「属性 → 详细信息」显示的产品名与公司名就是它。不改的话，
// 用户看到的是「通用安装器」，与自己装的应用对不上。
//
// 结构是三层对齐的：根 → StringFileInfo → 语言块 → 若干个字符串表项。
// 每层都要按 4 字节对齐，漏一处就会读出乱码。
class VersionBlob {
 public:
  void begin(const std::wstring& fileVersion, const std::wstring& productVersion) {
    // 固定头部：签名、结构版本、文件版本、产品版本
    putWord(0);              // wLength 占位，最后回填
    putWord(52);             // wValueLength：整个 VS_FIXEDFILEINFO 的字节数
    putWord(0);              // wType：0 = 二进制
    putString(L"VS_VERSION_INFO");
    align();
    putDword(0xFEEF04BD);    // dwSignature
    putDword(0x00010000);    // dwStrucVersion
    putVersion(fileVersion);
    putVersion(productVersion);
    putDword(0x3F);          // dwFileFlagsMask
    putDword(0);             // dwFileFlags
    putDword(0x40004);       // dwFileOS = VOS_NT_WINDOWS32
    putDword(1);             // dwFileType = VFT_APP
    putDword(0);             // dwFileSubtype
    putDword(0);             // dwFileDateMS
    putDword(0);             // dwFileDateLS
    align();
  }

  void beginStringFileInfo() {
    stringInfoOffset_ = bytes_.size();
    putWord(0);              // wLength 占位
    putWord(0);              // wValueLength
    putWord(1);              // wType：1 = 文本
    putString(L"StringFileInfo");
    align();
  }

  void beginLanguageBlock(const std::wstring& lang) {
    langOffset_ = bytes_.size();
    putWord(0);
    putWord(0);
    putWord(1);
    putString(lang);
    align();
  }

  void addString(const std::wstring& key, const std::wstring& value) {
    if (value.empty()) return;
    const size_t start = bytes_.size();
    putWord(0);
    // wValueLength 是字符数（含结尾 0），不是字节数
    putWord(static_cast<WORD>(value.size() + 1));
    putWord(1);
    putString(key);
    align();
    for (const wchar_t c : value) putWord(static_cast<WORD>(c));
    putWord(0);
    align();
    patchLength(start, bytes_.size() - start);
  }

  void endLanguageBlock() {
    if (langOffset_) patchLength(langOffset_, bytes_.size() - langOffset_);
  }

  void endStringFileInfo() {
    if (stringInfoOffset_) patchLength(stringInfoOffset_, bytes_.size() - stringInfoOffset_);
  }

  std::vector<uint8_t> finish() {
    patchLength(0, bytes_.size());
    return bytes_;
  }

 private:
  void putWord(uint16_t value) {
    bytes_.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  }

  void putDword(uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
  }

  void putString(const std::wstring& text) {
    for (const wchar_t c : text) putWord(static_cast<uint16_t>(c));
    putWord(0);
  }

  // 每个结构都从 4 字节边界开始
  void align() {
    while (bytes_.size() % 4) bytes_.push_back(0);
  }

  // "1.2.3" → 四个 16 位分量
  void putVersion(const std::wstring& text) {
    unsigned parts[4] = {0, 0, 0, 0};
    size_t index = 0;
    unsigned value = 0;
    bool any = false;
    for (const wchar_t c : text) {
      if (c >= L'0' && c <= L'9') {
        value = value * 10 + static_cast<unsigned>(c - L'0');
        any = true;
      } else if (c == L'.' && index < 3) {
        parts[index++] = value;
        value = 0;
        any = false;
      }
    }
    if (any && index < 4) parts[index] = value;
    for (const unsigned part : parts) putWord(static_cast<uint16_t>(part));
  }

  void patchLength(size_t offset, size_t length) {
    const uint16_t value = static_cast<uint16_t>(length);
    bytes_[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes_[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }

  std::vector<uint8_t> bytes_;
  size_t stringInfoOffset_ = 0;
  size_t langOffset_ = 0;
};

BOOL CALLBACK enumResLang(HMODULE module, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR param) {
  (void)module;
  (void)type;
  (void)name;
  reinterpret_cast<std::vector<WORD>*>(param)->push_back(lang);
  return TRUE;
}

// 把旧的版本块删掉，再写新的。
//
// 只删「确实存在」的那些：UpdateResource 删除一个不存在的资源会返回 87，
// 并且整笔事务随之作废 —— 后续写入和提交都会以 1359（内部错误）失败。
// 所以要先按语言枚举，不能想当然地写死几个语言 id 去删。
bool applyVersionInfo(const std::wstring& exePath, const AiInstaller::Manifest& manifest,
                      const std::wstring& originalName) {
  VersionBlob blob;
  blob.begin(manifest.version, manifest.version);

  blob.beginStringFileInfo();
  // 0x0804 = 简体中文，0x04B0 = Unicode
  blob.beginLanguageBlock(L"080404B0");
  blob.addString(L"CompanyName", manifest.publisher.empty() ? manifest.displayName
                                                            : manifest.publisher);
  blob.addString(L"FileDescription", manifest.displayName + L" 安装程序");
  blob.addString(L"FileVersion", manifest.version);
  blob.addString(L"InternalName", originalName);
  blob.addString(L"OriginalFilename", originalName);
  blob.addString(L"ProductName", manifest.displayName);
  blob.addString(L"ProductVersion", manifest.version);
  blob.endLanguageBlock();
  blob.endStringFileInfo();

  const std::vector<uint8_t> data = blob.finish();

  HANDLE update = ::BeginUpdateResourceW(exePath.c_str(), FALSE);
  if (!update) {
    writeError(L"版本信息：无法打开资源句柄，错误 " +
               std::to_wstring(::GetLastError()));
    return false;
  }

  const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);

  // 先枚举现有的版本资源语言，再逐个删：删不存在的会让事务失效。
  std::vector<WORD> existingLangs;
  HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (module) {
    ::EnumResourceLanguagesW(module, RT_VERSION, MAKEINTRESOURCEW(1), &enumResLang,
                             reinterpret_cast<LONG_PTR>(&existingLangs));
    ::FreeLibrary(module);
  }
  for (const WORD existing : existingLangs) {
    if (existing == lang) continue;  // 主语言稍后直接被覆盖
    ::UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(1), existing, nullptr, 0);
  }

  const bool ok = ::UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(1), lang,
                                    const_cast<uint8_t*>(data.data()),
                                    static_cast<DWORD>(data.size())) != FALSE;
  const DWORD updateError = ok ? 0 : ::GetLastError();

  // discard=true 时用 EndUpdateResource 直接丢弃，失败就当没改
  if (!::EndUpdateResourceW(update, FALSE)) {
    writeError(L"版本信息：提交资源失败，错误 " + std::to_wstring(::GetLastError()));
    return false;
  }
  if (!ok) {
    writeError(L"版本信息：写入资源失败，错误 " + std::to_wstring(updateError));
    return false;
  }
  return true;
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void appendU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const std::vector<std::wstring> args(argv, argv + argc);

  Options options;
  if (!parseOptions(args, options)) return 2;

  // 1. 读源清单：应用工程里那一份，只有 path，没有 offset/size
  std::vector<uint8_t> manifestBytes;
  if (!readWholeFile(options.manifest, manifestBytes)) {
    writeError(L"读不出清单：" + options.manifest);
    return 1;
  }

  AiInstaller::Manifest manifest;
  std::wstring error;
  const std::string manifestText(manifestBytes.begin(), manifestBytes.end());
  if (!AiInstaller::parseManifest(utf8ToWide(manifestText), manifest, error)) {
    writeError(L"清单无效：" + error);
    return 1;
  }
  if (manifest.files.empty()) {
    writeError(L"清单里没有任何文件");
    return 1;
  }

  // 2. 先落一份安装器副本：原件永远不动，同一个安装器可以给多个应用用
  std::vector<uint8_t> image;
  if (!readWholeFile(options.installer, image)) {
    writeError(L"读不出安装器：" + options.installer);
    return 1;
  }
  if (!writeWholeFile(options.out, image)) {
    writeError(L"写不出产物：" + options.out);
    return 1;
  }

  // 3. 改资源必须发生在追加载荷之前：
  //    UpdateResource 会重写整个 PE 映像，新追加的尾部会被它丢掉。
  const std::wstring originalName = options.out.substr(options.out.find_last_of(L"\\/") + 1);
  if (!options.icon.empty() && !applyIcon(options.out, options.icon)) {
    writeError(L"图标替换失败，继续生成安装包");
  }
  if (!applyVersionInfo(options.out, manifest, originalName)) {
    writeError(L"版本信息写入失败，继续生成安装包");
  }

  // 4. 重新读回改过的副本，后面接清单与载荷
  if (!readWholeFile(options.out, image)) {
    writeError(L"改完资源后读不回产物：" + options.out);
    return 1;
  }

  // 5. 清单紧接 exe。文件表里的 offset 相对载荷区起点（清单之后），
  //    所以要先算好每个文件的偏移，再写清单。
  //
  // 读端按「exe → 清单 → 载荷 」理解这个布局（见 package.h），
  // 这里必须一致：顺序反了，安装时按偏移取到的就是清单开头的字节。
  const uint64_t dataOffset = image.size();
  uint64_t offset = 0;
  for (auto& file : manifest.files) {
    const std::wstring source = joinPath(options.root, file.path);
    uint64_t size = 0;
    if (!fileSize(source, size)) {
      writeError(L"读不出待打包文件：" + source);
      return 1;
    }
    file.offset = offset;
    file.size = size;
    offset += size;

    if (options.verbose) {
      writeOut(L"  " + file.path + L"  " + std::to_wstring(size) + L" 字节\n");
    }
  }

  const std::string manifestUtf8 = wideToUtf8(AiInstaller::formatManifest(manifest));
  image.insert(image.end(), manifestUtf8.begin(), manifestUtf8.end());

  // 6. 逐个追加载荷
  uint64_t appended = 0;
  for (const auto& file : manifest.files) {
    const std::wstring source = joinPath(options.root, file.path);
    if (!appendFile(source, image)) {
      writeError(L"追加失败：" + source);
      return 1;
    }
    appended += file.size;
  }
  if (appended != offset) {
    writeError(L"载荷长度与清单不符");
    return 1;
  }

  // 7. Trailer：魔数 + 格式版本 + 清单长度 + 数据区起点与长度
  image.insert(image.end(), AiInstaller::kPackageMagic,
               AiInstaller::kPackageMagic + sizeof(AiInstaller::kPackageMagic));
  appendU32(image, AiInstaller::kPackageFormatVersion);
  appendU32(image, static_cast<uint32_t>(manifestUtf8.size()));
  appendU64(image, dataOffset);
  appendU64(image, manifestUtf8.size() + offset);

  if (!writeWholeFile(options.out, image)) {
    writeError(L"写不出产物：" + options.out);
    return 1;
  }

  if (options.verbose) {
    writeOut(L"已生成 " + options.out + L"（" +
             std::to_wstring(static_cast<unsigned long long>(image.size())) + L" 字节，" +
             std::to_wstring(manifest.files.size()) + L" 个文件）\n");
  }
  return 0;
}
