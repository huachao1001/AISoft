// 操作码名字表。报错信息与反汇编共用同一份，两边写两套必然慢慢分叉。
//
// ── 为什么这张表不能省 ──
//
// 没有它，报错只能写「操作码 27 越界」，而写脚本的人看不懂 27 是什么。
// 名字表让内部错误变成「kSlice 越界」——虽然仍然是编译器 bug，但至少
// 能定位到是哪一段生成的字节码出了问题。
#include "aipy/vm.h"

namespace aipy {
namespace vm {

const char* opName(Op op) {
  switch (op) {
    case Op::kConst: return "const";
    case Op::kNone: return "none";
    case Op::kTrue: return "true";
    case Op::kFalse: return "false";
    case Op::kLoadLocal: return "load_local";
    case Op::kStoreLocal: return "store_local";
    case Op::kLoadGlobal: return "load_global";
    case Op::kStoreGlobal: return "store_global";
    case Op::kBin: return "bin";
    case Op::kCmp: return "cmp";
    case Op::kUnary: return "unary";
    case Op::kJump: return "jump";
    case Op::kJumpIfFalse: return "jump_if_false";
    case Op::kJumpIfFalseKeep: return "jump_if_false_keep";
    case Op::kJumpIfTrueKeep: return "jump_if_true_keep";
    case Op::kPop: return "pop";
    case Op::kDup: return "dup";
    case Op::kCall: return "call";
    case Op::kCallBuiltin: return "call_builtin";
    case Op::kCallMethod: return "call_method";
    case Op::kCallExt: return "call_ext";
    case Op::kReturn: return "return";
    case Op::kBuildList: return "build_list";
    case Op::kBuildDict: return "build_dict";
    case Op::kGetItem: return "get_item";
    case Op::kSetItem: return "set_item";
    case Op::kSlice: return "slice";
    case Op::kSetSlice: return "set_slice";
    case Op::kIterInit: return "iter_init";
    case Op::kIterNext: return "iter_next";
    case Op::kIterBreak: return "iter_break";
    case Op::kInplace: return "inplace";
    case Op::kWithEnter: return "with_enter";
    case Op::kWithExit: return "with_exit";
    case Op::kSetupExc: return "setup_exc";
    case Op::kPopExc: return "pop_exc";
    case Op::kRaise: return "raise";
    case Op::kRaiseType: return "raise_type";
    case Op::kReraise: return "reraise";
    case Op::kExcMatch: return "exc_match";
    case Op::kCurrentExc: return "current_exc";
    case Op::kClearExc: return "clear_exc";
    case Op::kDelLocal: return "del_local";
    case Op::kDelGlobal: return "del_global";
    case Op::kBuildTuple: return "build_tuple";
    case Op::kBuildSet: return "build_set";
    case Op::kCallDyn: return "call_dyn";
    case Op::kDictMerge: return "dict_merge";
    case Op::kMakeFunction: return "make_function";
    case Op::kCallValue: return "call_value";
    case Op::kCallValueDyn: return "call_value_dyn";
    case Op::kMakeClass: return "make_class";
    case Op::kGetAttr: return "get_attr";
    case Op::kSetAttr: return "set_attr";
    case Op::kInstantiate: return "instantiate";
    case Op::kMakeSuper: return "make_super";  }
  return "?";
}

}  // namespace vm
}  // namespace aipy
