// 文件管理器右键菜单处理器（Windows：进程内 COM 组件）
//
// 为什么需要它：注册表静态动词的文字在注册那一刻就定死了，做不出
// 「压缩到 rnd.zip」这种带文件名的文案。只有进程内的 IContextMenu 才能在
// 弹出菜单的瞬间按实际选中的路径算文字。
//
// 适用范围：Win10 直接显示在右键第一层；Win11 的紧凑菜单只接受带包标识
// （MSIX/稀疏包）的 IExplorerCommand，未打包的 Win32 程序一律归入
// 「显示更多选项」，7-Zip、WinRAR 同样如此。要进第一层必须加稀疏包，
// 代价是签名与管理员权限，本项目选择不加。
//
// 这个 DLL 会被加载进资源管理器的进程里，所以：
//   - 不分配长期资源，不启动线程，不链接会做全局构造的重型库
//   - 任何失败都安静返回：崩了就是崩资源管理器
//   - 只做「算出几项菜单」这一件事，真正的活交给宿主 exe
//
// 菜单内容由应用实现的 aiuiBuildShellMenu 决定，本文件不含任何业务规则。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "AiUI/shell_menu.h"

#include "aiui_shell_menu_ids.h"

#include <new>
#include <string>
#include <vector>

namespace {

// 菜单项命令 id：从 0 起，最大项数由 QueryContextMenu 的可用区间决定
constexpr UINT kMaxItems = 32;

// 活着的处理器对象数：DllCanUnloadNow 据此判断能不能被卸载
LONG g_objectCount = 0;

HINSTANCE g_module = nullptr;

bool IsDirectoryPath(const std::wstring& path) {
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring Quote(const std::wstring& s) {
  std::wstring out = L"\"";
  out += s;
  out += L"\"";
  return out;
}

// 宿主 exe 的路径。注册时写进 CLSID\AppPath，避免依赖「exe 与 dll 同目录」
// 这种脆弱假设——用户完全可能把 exe 挪到别处。
std::wstring AppPath() {
  const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + AIUI_SHELL_MENU_CLSID +
                           L"\\AppPath";
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = sizeof(buffer);
  if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer,
                     &size) == ERROR_SUCCESS &&
      buffer[0] != L'\0') {
    return buffer;
  }
  // 兜底：组件与 exe 由同一个程序释放，通常在同一目录
  wchar_t dllPath[MAX_PATH] = {};
  if (!::GetModuleFileNameW(g_module, dllPath, MAX_PATH)) return {};
  ::PathRemoveFileSpecW(dllPath);
  std::wstring exe = dllPath;
  exe += L"\\";
  exe += AIUI_SHELL_MENU_EXE_NAME;
  return ::PathFileExistsW(exe.c_str()) ? exe : std::wstring();
}

// ── 处理器本体 ──

class ContextMenu final : public IContextMenu, public IShellExtInit {
 public:
  ContextMenu() { ::InterlockedIncrement(&g_objectCount); }

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IContextMenu) {
      *ppv = static_cast<IContextMenu*>(this);
    } else if (riid == IID_IShellExtInit) {
      *ppv = static_cast<IShellExtInit*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // 记录选中的路径，后面算菜单用
  IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override {
    selection_.clear();
    if (!data) return E_INVALIDARG;

    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fmt, &medium))) return S_OK;  // 拿不到就当空选择

    if (auto* drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal))) {
      const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring path(len, L'\0');
        ::DragQueryFileW(drop, i, path.data(), len + 1);
        AiUI::ShellSelectionItem item;
        item.isDirectory = IsDirectoryPath(path);
        item.path = std::move(path);
        selection_.push_back(std::move(item));
      }
      ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    return S_OK;
  }

  IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT firstCmd, UINT lastCmd,
                                  UINT flags) override {
    // 默认动词与拖放场合不插入
    if (flags & (CMF_DEFAULTONLY | CMF_VERBSONLY)) return MakeResult(0);
    if (selection_.empty()) return MakeResult(0);

    // 业务规则全在应用侧；这里只负责把结果摆进菜单，异常一律吞掉
    std::vector<AiUI::ShellMenuItem> items;
    try {
      items = aiuiBuildShellMenu(selection_).items;
    } catch (...) {
      return MakeResult(0);
    }

    const UINT capacity = lastCmd - firstCmd + 1;
    if (items.size() > capacity) items.resize(capacity);

    for (size_t i = 0; i < items.size(); ++i) {
      ::InsertMenuW(menu, index++, MF_BYPOSITION | MF_STRING, firstCmd + static_cast<UINT>(i),
                    items[i].label.c_str());
      if (items[i].separatorAfter) {
        ::InsertMenuW(menu, index++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
      }
    }
    return MakeResult(static_cast<USHORT>(items.size()));
  }

  IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
    if (!info) return E_INVALIDARG;
    // 只认整数命令 id；传字符串名的（别的程序按动词名调用）一律拒绝
    if (HIWORD(info->lpVerb) != 0) return E_INVALIDARG;

    const UINT cmd = LOWORD(info->lpVerb);
    if (cmd >= kMaxItems) return E_INVALIDARG;

    std::vector<AiUI::ShellMenuItem> items;
    try {
      items = aiuiBuildShellMenu(selection_).items;
    } catch (...) {
      return E_FAIL;
    }
    if (cmd >= items.size()) return E_INVALIDARG;

    return RunCommand(items[cmd].arguments);
  }

  IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override {
    // 不需要帮助文字与规范动词名
    return E_NOTIMPL;
  }

 private:
  ~ContextMenu() { ::InterlockedDecrement(&g_objectCount); }

  static HRESULT MakeResult(USHORT count) {
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, count);
  }

  // 启动宿主 exe 并把选中的路径按约定传过去
  HRESULT RunCommand(const std::wstring& arguments) {
    if (selection_.empty()) return E_FAIL;

    const std::wstring exe = AppPath();
    if (exe.empty()) return E_FAIL;

    std::wstring command = Quote(exe);
    if (!arguments.empty()) {
      command += L' ';
      command += arguments;
    }
    for (const auto& item : selection_) {
      command += L' ';
      command += Quote(item.path);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (!::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &si, &pi)) {
      return HRESULT_FROM_WIN32(::GetLastError());
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return S_OK;
  }

  LONG refs_ = 1;
  std::vector<AiUI::ShellSelectionItem> selection_;
};

// ── 类工厂 ──

class ClassFactory final : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (ppv) *ppv = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    // 分配失败不能把异常抛回资源管理器
    auto* obj = new (std::nothrow) ContextMenu();
    if (!obj) return E_OUTOFMEMORY;
    const HRESULT hr = obj->QueryInterface(riid, ppv);
    obj->Release();
    return hr;
  }

  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) ::InterlockedIncrement(&serverLocks_);
    else ::InterlockedDecrement(&serverLocks_);
    return S_OK;
  }

  static LONG ServerLocks() { return serverLocks_; }

 private:
  ~ClassFactory() = default;
  LONG refs_ = 1;
  static inline LONG serverLocks_ = 0;
};

}  // namespace

// ── 导出的 DLL 入口 ──

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    ::DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  // 资源管理器会用各种 CLSID 试探，必须精确比对
  wchar_t text[64] = {};
  if (::StringFromGUID2(clsid, text, 64) <= 0) return CLASS_E_CLASSNOTAVAILABLE;
  if (::CompareStringOrdinal(text, -1, AIUI_SHELL_MENU_CLSID, -1, TRUE) != CSTR_EQUAL) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto* factory = new (std::nothrow) ClassFactory();
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

STDAPI DllCanUnloadNow() {
  return (g_objectCount == 0 && ClassFactory::ServerLocks() == 0) ? S_OK : S_FALSE;
}
