# AiPic 的单元测试：只覆盖纯逻辑（像素运算、命名模版这类），不建窗口。
#
# 复用 AiUI 的测试框架（test_framework.h / test_framework.cpp），这样断言宏、
# 用例注册与输出格式和老工程一致，跑起来不用记两套。
cmake_minimum_required(VERSION 3.20)
project(AiPicTests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AiPic only builds with MSVC")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# 同级目录的 AiUI：测试框架和 RgbaImage 等类型都来自这里
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../AiUI ${CMAKE_CURRENT_BINARY_DIR}/AiUI)

add_executable(AiPic_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../../AiUI/tests/test_framework.cpp
  test_pixels.cpp
  ../src/editor/pixels.cpp
)

target_include_directories(AiPic_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../AiUI/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../../AiUI/include
  ../src
)

target_compile_options(AiPic_tests PRIVATE /utf-8 /W4 /permissive-)
