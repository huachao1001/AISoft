# 7-Zip 的解码器有一部分是 C（C/Alloc.c），所以这里必须是 C CXX 双语言。
# 顶层 project() 只声明了 CXX，子目录补上，避免「.c 被静默忽略」。
enable_language(C)

# 7-Zip deflate 解码器（最小裁剪）。见同目录 README.md。
#
# 只做**解压**：`.pylib` 是 ZIP，用户机器上要读它；而写包发生在构建期
# （那时有 Python 的 zipfile）。所以这里不需要 7-Zip 的编码器。
#
# ── 为什么要重置目录级定义 ──
#
# 顶层 CMakeLists 全局加了 WIN32_LEAN_AND_MEAN。它会把 OLE 相关声明排除掉，
# 而 7-Zip 的 IUnknown / PROPVARIANT 依赖它们——症状是一屏「STDMETHOD 缺少
# 返回类型」。目录属性在这里显式重置，比在命令行上加 /U 干净：后者会和
# /D 冲突并触发 D9025。

get_directory_property(_aipy_inherited_defs COMPILE_DEFINITIONS)
if(_aipy_inherited_defs)
  list(REMOVE_ITEM _aipy_inherited_defs WIN32_LEAN_AND_MEAN)
  set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_aipy_inherited_defs}")
endif()

set(AIPY_7Z_DIR ${CMAKE_CURRENT_LIST_DIR})

add_library(aipy_deflate STATIC
  ${AIPY_7Z_DIR}/CPP/7zip/Compress/DeflateDecoder.cpp
  ${AIPY_7Z_DIR}/CPP/7zip/Compress/BitlDecoder.cpp
  ${AIPY_7Z_DIR}/CPP/7zip/Compress/LzOutWindow.cpp
  ${AIPY_7Z_DIR}/CPP/7zip/Common/InBuffer.cpp
  ${AIPY_7Z_DIR}/CPP/7zip/Common/OutBuffer.cpp
  ${AIPY_7Z_DIR}/C/Alloc.c
)

target_include_directories(aipy_deflate PUBLIC
  ${AIPY_7Z_DIR}/CPP
  ${AIPY_7Z_DIR}/CPP/7zip
  ${AIPY_7Z_DIR}/C)

if(MSVC)
  # 上游代码是旧式 C++，/W4 + /permissive- 会刷满警告（AiZip 侧也是降到 /W1）。
  #
  # 不能直接传 /W1：它会覆盖目录级的 /W4 并触发 D9025「重写选项」告警。
  # 改成按编号关掉上游代码确实会碰到的那些。
  target_compile_options(aipy_deflate PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/WX->)
  target_compile_options(aipy_deflate PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996 /wd4244 /wd4267 /wd4100 /wd4189>
    $<$<COMPILE_LANGUAGE:C,CXX>:/wd4701 /wd4702 /wd4706 /wd4310 /wd4464>)
  # 分发出的库必须关 /GL：/GL 产物链接期要 c2.dll，用户机器上只有 link.exe。
  target_compile_options(aipy_deflate PRIVATE $<$<CONFIG:Release>:/GL->)
endif()
