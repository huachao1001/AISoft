# 深递归：验证显式帧栈不会爆掉 C++ 调用栈。
#
# 为什么这条必须单独测：VM 用 std::vector<Frame> 存帧而不是递归调用 run()，
# 所以理论深度只受 kMaxCallDepth 限制，与 C++ 栈无关。但「理论上」三个字
# 在这里没有价值——一旦哪天有人把执行循环改回递归，这条脚本会当场崩掉，
# 而不是等到用户写了个深层递归才崩。

def depth(n):
    if n <= 0:
        return 0
    return 1 + depth(n - 1)


# 200 层：在 kMaxCallDepth(256) 之内，必须成功。
shallow = depth(200)
