cmake_minimum_required(VERSION 3.20)
project(7zstatic CXX C ASM_MASM)

# 7-Zip 26.05 静态库
#
# 直接复用官方 7z.dll 的构建配方（CPP/7zip/Bundles/Format7zF/Arc.mak），
# 所以支持的格式、算法、加密能力与官方 7z.dll 完全一致，但直接链接进 exe。
#
# 与原版 dll 构建的三处必要差异：
#   1. 不定义 Z7_EXTERNAL_CODECS —— 静态链接下没有外部 codec 宿主
#   2. 入口改用 Z7StaticExports.cpp，去掉 DllMain
#   3. 上层用 /WHOLEARCHIVE 链接，保证 *Register.cpp 的静态注册对象不被丢弃

set(Z7_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# 官方对象清单，由 tools/gen_sources.ps1 从 Arc.mak 生成
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

# 覆盖 CMake 的 MSVC 默认旗标(/GR /W3 等)，改用 7-Zip 官方组合，
# 避免 D9025 重复定义告警。
# 必须与主工程一致用 /MT：混合 /MD 和 /MT 会引入两套 CRT 与两个堆，
# 跨库传递 malloc 的指针时直接崩。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /nologo")
set(CMAKE_C_FLAGS   "/DWIN32 /D_WINDOWS /nologo")

add_library(7zstatic STATIC
  ${Z7_SRC_DIR}/CPP/7zip/Archive/Z7StaticExports.cpp
  ${Z7_SRC_DIR}/CPP/7zip/Archive/ArchiveExports.cpp
  # 官方 Format7zF 在 makefile 里额外追加的编解码器导出表，
  # CreateCoder / CreateHasher 的实现在这里
  ${Z7_SRC_DIR}/CPP/7zip/Compress/CodecExports.cpp
)

foreach(_f IN LISTS Z7_CPP_SOURCES)
  target_sources(7zstatic PRIVATE ${Z7_SRC_DIR}/${_f})
endforeach()
foreach(_f IN LISTS Z7_C_SOURCES)
  target_sources(7zstatic PRIVATE ${Z7_SRC_DIR}/${_f})
endforeach()

# ── 汇编模块 ──
# MASM 按【当前工作目录】解析相对 include，而 CMake 的 ASM_MASM 规则固定以构建目录
# 为 CWD，无法复刻官方在 Format7zF 目录下编译的做法（Sort.asm 里写的是
# ../../../../Asm/x86/7zAsm.asm，只在那个层级下成立）。
# 所以在配置期生成一份「共享宏 include 改成绝对路径」的副本再编译。
set(Z7_ASM_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/asmgen)
file(MAKE_DIRECTORY ${Z7_ASM_GEN_DIR})
file(TO_NATIVE_PATH "${Z7_SRC_DIR}/Asm/x86/7zAsm.asm" Z7_ASM_SHARED)

set(Z7_ASM_GEN "")
foreach(_f IN LISTS Z7_ASM_SOURCES)
  get_filename_component(_name ${_f} NAME_WE)
  file(READ ${Z7_SRC_DIR}/${_f} _txt)
  string(REPLACE "include ../../../../Asm/x86/7zAsm.asm"
                 "include ${Z7_ASM_SHARED}" _txt "${_txt}")
  string(REPLACE "include 7zAsm.asm"
                 "include ${Z7_ASM_SHARED}" _txt "${_txt}")
  file(WRITE ${Z7_ASM_GEN_DIR}/${_name}.asm "${_txt}")
  list(APPEND Z7_ASM_GEN ${Z7_ASM_GEN_DIR}/${_name}.asm)
endforeach()

set_source_files_properties(${Z7_ASM_GEN} PROPERTIES
  LANGUAGE ASM_MASM
  COMPILE_OPTIONS "/c"
)
target_sources(7zstatic PRIVATE ${Z7_ASM_GEN})

# LZMA 解码的汇编加速版需要这个宏才会被 LzmaDec.c 调用
set_source_files_properties(${Z7_SRC_DIR}/C/LzmaDec.c PROPERTIES
  COMPILE_DEFINITIONS "Z7_LZMA_DEC_OPT"
)

target_include_directories(7zstatic PUBLIC
  ${Z7_SRC_DIR}/CPP
  ${Z7_SRC_DIR}/CPP/7zip
  ${Z7_SRC_DIR}/C
)

target_compile_definitions(7zstatic PRIVATE
  UNICODE _UNICODE
  # 不能加 WIN32_LEAN_AND_MEAN：它会把 OLE 头排除掉，
  # 而 7-Zip 的 IUnknown / PROPVARIANT 依赖 <Windows.h> 里的 OLE 声明。
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

# 与官方 Build.mak 对齐：/Gr 默认 __fastcall、/GR- 关 RTTI、/Gy /GF 体积优化。
# 上游代码有大量旧式转换，不开 /W4 /permissive-。
# 必须按语言限定：这些旗标直接挂在 target 上会传给 ml64，
# 汇编器不认识 /O2 /EHsc 等选项，会刷一屏 A4018 警告。
target_compile_options(7zstatic PRIVATE
  $<$<COMPILE_LANGUAGE:C,CXX>:/O2 /Gy /GF /Gr /GR- /EHsc /utf-8 /W1>
  $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996 /wd4244 /wd4267 /wd4100 /wd4189>
  $<$<COMPILE_LANGUAGE:C,CXX>:/wd4701 /wd4702 /wd4706 /wd4310>
)

target_link_libraries(7zstatic PUBLIC oleaut32 ole32 user32 advapi32 shell32)
