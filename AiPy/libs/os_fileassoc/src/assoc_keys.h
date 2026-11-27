// `fileassoc` 内部共用：注册表键操作。
//
// ── 为什么单独一个头 ──
//
// register / unregister / icon / query 四个导出函数都要读写
// `HKCU\Software\Classes` 下的键，但每个导出函数各在一个 .cpp 里。工具放
// 这里，那些 .cpp 各自引用符号、不复制定义——按需链接仍按函数挑 obj。
#pragma once

#include <string>

namespace aipy {
namespace os_fileassoc {

// 写一个字符串值（REG_SZ）。`name` 空串表示写键的默认值。
// 成功返回空串，失败返回可读原因。
std::string writeString(const std::string& keyPath, const std::string& name,
                        const std::string& value);

// 读一个字符串值。不存在或类型不对时返回空串。
std::string readString(const std::string& keyPath, const std::string& name);

// 递归删除一个键。键不存在也算成功（幂等）。
std::string deleteKey(const std::string& keyPath);

// 键是否存在。
bool keyExists(const std::string& keyPath);

// 系统错误码 → 可读原因。与 `libs/io` 的 reasonOf 同构，但这里只服务于
// 注册表，不引入文件系统那套路径处理。
std::string systemErrorText(long code);

}  // namespace os_fileassoc
}  // namespace aipy
