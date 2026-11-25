// pcc：把脚本编成字节码、反汇编、打包成 exe。
//
// ── 用法 ──
//
//     pcc app.py                    # 生成 app.bc
//     pcc app.py out.bc            # 指定输出
//     pcc --disasm app.bc          # 反汇编看指令流
//     pcc --exe app.py app.exe     # 打包成 exe
//     pcc --exe app.py app.exe --launcher aipy_app.exe
//     pcc --unbundle app.exe app.bc
//
// ── 为什么整个编译器是 C++ ──
//
// 用户机器上不该有 Python。发布件的全部内容是这几个 exe 加 `.lib`，
// 凡是「打包一个脚本」需要的步骤都必须能在没有任何解释器的机器上跑完。
#pragma once

namespace pcc {

// 与 aipyic.py 的 main 同签名：返回退出码。
int runCommandLine(int argc, char** argv);

}  // namespace pcc
