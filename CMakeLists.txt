cmake_minimum_required(VERSION 3.20)
project(AIZip CXX C ASM_MASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AIZip only builds with MSVC")
endif()

# 静态链接 CRT，免安装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# 7-Zip 内核以静态库形式编进 exe，不再需要外部 dll
add_subdirectory(third_party/7z/build-cmake)

# 7-Zip 各格式靠 *Register.cpp 里的全局对象在静态初始化阶段注册自己，这些对象
# 没有任何外部引用，普通静态库链接会被整体丢弃，结果是格式列表为空。
# /WHOLEARCHIVE 整库拉入，再由 /OPT:REF 清理真正死掉的函数。
set(Z7_LINK_OPTIONS "/WHOLEARCHIVE:7zstatic")

add_executable(AIZip WIN32
  src/main.cpp
  src/app.cpp
  src/archive.cpp
  src/sevenzip_loader.cpp
  src/ui.cpp
  src/ui_main.cpp
  src/ui_list.cpp
  src/ui_tree.cpp
  src/ui_toolbar.cpp
  src/dlg_compress.cpp
  src/dlg_extract.cpp
  src/dlg_input.cpp
  src/dlg_progress.cpp
  src/util.cpp
  src/res/aizip.rc
)

target_include_directories(AIZip PRIVATE src)

target_compile_definitions(AIZip PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  # SetWindowSubclass / DefSubclassProc 等 comctl32 v6 API 要求
  _WIN32_WINNT=0x0601
)

# 体积优先：函数/数据分段 + 链接期裁剪
target_compile_options(AIZip PRIVATE /O2 /Gy /Gw /GF /utf-8 /W4 /permissive- /Zc:__cplusplus /GL)

# 清单里声明 comctl32 v6（控件视觉风格）与 Per-Monitor V2 DPI 感知。
# 必须用 /MANIFESTINPUT 合并，直接在 .rc 里写 RT_MANIFEST 会和链接器自动生成的清单冲突。
#
# /WHOLEARCHIVE:7zstatic 是关键：7-Zip 各格式靠 *Register.cpp 里的全局对象在
# 静态初始化阶段注册自己，这些对象没有任何外部引用，普通静态库链接会被整体丢弃，
# 结果是格式列表为空。整库拉入后由 /OPT:REF 清理真正死掉的函数。
target_link_options(AIZip PRIVATE
  /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO
  "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/res/aizip.manifest"
  ${Z7_LINK_OPTIONS}
)

target_link_libraries(AIZip PRIVATE
  7zstatic
  comctl32 shell32 shlwapi ole32 oleaut32 uuid uxtheme msimg32 dwmapi
)

# 内核功能回归工具（非发布产物）：命令行驱动压缩/解压，便于端到端校验
add_executable(test_arc
  tools/test_arc.cpp
  src/archive.cpp
  src/sevenzip_loader.cpp
  src/util.cpp
)
target_include_directories(test_arc PRIVATE src)
target_compile_definitions(test_arc PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
)
target_compile_options(test_arc PRIVATE /O2 /utf-8 /W4 /permissive-)
target_link_options(test_arc PRIVATE /OPT:REF /OPT:ICF ${Z7_LINK_OPTIONS})
target_link_libraries(test_arc PRIVATE 7zstatic comctl32 shell32 shlwapi ole32 oleaut32 uuid
                                      uxtheme msimg32 dwmapi)
