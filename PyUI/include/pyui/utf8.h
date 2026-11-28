// UTF-8 工具：转发到 AiPy。
//
// **实现不在 PyUI 里**。UTF-8 的字符边界扫描是语言层与图形层共同需要
// 的能力，但它不属于任何一边；把它放在 AiPy（零界面依赖）里，两条
// 依赖链才不会互相拖拽。
//
// 这个头只做一件事：把 aipy 的函数引入 pyui 命名空间，让界面层既有的
// 调用点（widgets / view / text_field 共 50+ 处）保持原样。PyUI 不是
// 「拥有」这些函数，只是「使用」它们——所以这里是转发，不是定义。
#pragma once

#include "aipy/utf8.h"

namespace pyui {

using aipy::utf8AlignDown;
using aipy::utf8Encode;
using aipy::utf8Length;
using aipy::utf8NextBoundary;
using aipy::utf8OffsetOfChar;
using aipy::utf8PrefixByChars;
using aipy::utf8PrefixLength;
using aipy::utf8PrevBoundary;

}  // namespace pyui
