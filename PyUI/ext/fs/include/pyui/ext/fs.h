// fs 扩展：文件读写与路径拼接
//
// 为什么是扩展而不是核心：核心只提供最基础的 Python 语义，凡是要碰系统
// 资源的能力（文件、网络、线程）都属于扩展。这样「用了才链接」才有意义——
// 一个不读文件的程序不该带进来 fopen 和路径处理。
#pragma once

#include "pyui/py_value.h"

namespace pyui {
namespace ext {
namespace fs {

// Value 在 pyui::py 里（脚本层的动态值），不在 pyui 下。
// 必须写全名：fs 里没有 Value，不加限定会被查找成 fs::Value，
// 症状是「缺少类型说明符」——完全看不出根因在命名空间。
using pyui::py::Value;

// 读整个文件。失败返回 None（脚本里用 is_none 判断）。
// 会自动去掉 UTF-8 BOM：记事本保存的文本会带它，留在正文里第一个字是乱码。
Value readFile(const Value& path);

// 写整个文件。成功返回 None，失败返回错误描述字符串。
// 短写（磁盘满）也报错，否则用户会以为保存成功。
Value writeFile(const Value& path, const Value& text);

// 按当前平台的路径分隔符拼两段。
Value joinPath(const Value& left, const Value& right);

// 用户主目录。拿不到时返回 None。
Value homeDirectory();

}  // namespace fs
}  // namespace ext
}  // namespace pyui
