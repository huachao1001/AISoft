# 生成安装程序的资源脚本：把构建出的 AiZip.exe 作为 RCDATA 打进安装包
#
# 直接在主 .rc 里写路径不行：那个路径在配置期还不存在，且随构建目录变化。
# 这里在构建期生成一份只含资源语句的 .rc，由 setup.rc 之外的规则单独编译。
#
# 参数：-DEXE=<AiZip.exe 路径> -DOUT=<生成的 .rc 路径> -DRC=<带图标与版本的主 .rc>

if(NOT EXE OR NOT OUT OR NOT RC)
  message(FATAL_ERROR "aiui_setup_rc.cmake 需要 -DEXE= -DOUT= -DRC=")
endif()

if(NOT EXISTS "${EXE}")
  message(FATAL_ERROR "找不到要打包的程序：${EXE}")
endif()

# .rc 里路径必须用正斜杠，否则 \A、\b 会被当成转义序列
file(TO_CMAKE_PATH "${EXE}" exe_path)

# 主 .rc 用 #include 引入本文件，这样图标、版本信息与内嵌程序在同一个编译单元里
file(WRITE "${OUT}"
  "// 由 aiui_setup_rc.cmake 生成，请勿手工编辑\n"
  "101 RCDATA \"${exe_path}\"\n"
)
