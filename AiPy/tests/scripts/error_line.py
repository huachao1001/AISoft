# 第 5 行必须出错：整数没有 upper 方法。
#
# 这份脚本唯一的用途是验证「错误带源行号」。行号变了就要同步改
# tests/test_vm.cpp 的 testErrorLine。
value = 1
value = value + 1
text = "ok"
broken = value.upper()
