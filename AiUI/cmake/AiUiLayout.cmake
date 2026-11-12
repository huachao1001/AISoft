# AIUI_ADD_LAYOUTS：把 XML 布局在构建期自动编译成 C++ 并加入目标
#
# 用法：
#   aiui_add_layouts(<target> [OUTPUT_DIR <dir>] <file.xml> [<file.xml> ...])
#
# 生成的 .h/.cpp 放在 ${CMAKE_CURRENT_BINARY_DIR}/aiui_generated 下，
# 类名取自文件名（main_page.xml → MainPage，命名空间 AiUI::generated）。
# 修改 XML 后重新构建即自动重新生成，无需手工执行任何工具。
#
# 同时生成 aiui_layouts.cpp，把每个布局登记到 LayoutInflater，
# 宿主即可用 LayoutInflater::inflate(L"main_page") 按名字取到布局类。

# layoutgen 只在需要时构建一次；作为宿主工具，不参与安装
if(NOT TARGET AiUI::layoutgen)
  add_executable(AiUI_layoutgen "${CMAKE_CURRENT_LIST_DIR}/../tools/layoutgen/main.cpp")
  set_target_properties(AiUI_layoutgen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
    FOLDER "AiUI/Tools"
  )
  # 工具本身不需要 GDI+ 等运行时依赖
  target_compile_options(AiUI_layoutgen PRIVATE /utf-8 /W4 /permissive-)
  add_executable(AiUI::layoutgen ALIAS AiUI_layoutgen)
endif()

function(aiui_add_layouts target)
  set(options)
  set(oneValueArgs OUTPUT_DIR)
  set(multiValueArgs)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "aiui_add_layouts(${target} ...) 至少需要一个 XML 文件")
  endif()

  if(ARG_OUTPUT_DIR)
    set(out_dir "${ARG_OUTPUT_DIR}")
  else()
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated")
  endif()
  file(MAKE_DIRECTORY "${out_dir}")

  set(generated_sources)
  set(registry_inputs)
  foreach(xml IN LISTS ARG_UNPARSED_ARGUMENTS)
    get_filename_component(xml_abs "${xml}" ABSOLUTE)
    if(NOT EXISTS "${xml_abs}")
      message(FATAL_ERROR "布局文件不存在：${xml}")
    endif()

    get_filename_component(xml_stem "${xml_abs}" NAME_WE)
    set(header "${out_dir}/${xml_stem}.h")
    set(source "${out_dir}/${xml_stem}.cpp")

    add_custom_command(
      OUTPUT "${header}" "${source}"
      COMMAND $<TARGET_FILE:AiUI::layoutgen> "${xml_abs}" "${header}" "${source}"
      DEPENDS "${xml_abs}" AiUI::layoutgen
      COMMENT "生成布局 ${xml_stem}"
      VERBATIM
    )
    list(APPEND generated_sources "${header}" "${source}")
    list(APPEND registry_inputs "${xml_abs}")
  endforeach()

  # 注册表：名字 → 工厂，供 LayoutInflater 按 XML 文件名取布局
  set(registry "${out_dir}/aiui_layouts.cpp")
  add_custom_command(
    OUTPUT "${registry}"
    COMMAND $<TARGET_FILE:AiUI::layoutgen> --registry "${registry}" ${registry_inputs}
    DEPENDS ${registry_inputs} AiUI::layoutgen
    COMMENT "生成布局注册表"
    VERBATIM
  )
  list(APPEND generated_sources "${registry}")

  target_sources(${target} PRIVATE ${generated_sources})
  target_include_directories(${target} PRIVATE "${out_dir}")

  # 生成的头文件在构建目录里，更新后让 IDE 能看到
  set_source_files_properties(${generated_sources} PROPERTIES GENERATED TRUE)
endfunction()

# AIUI_ADD_THEMES：把主题 XML 在构建期编译成命名调色板并加入目标
#
# 用法：
#   aiui_add_themes(<target> <themes.xml>)
#
# themes.xml 里写若干个 <Theme name="light" body="#f3f4f6" .../>，
# 生成代码在静态初始化阶段把它们登记进 ThemeRegistry，运行时即可用
# Window::setAppTheme(&ThemeRegistry::getOr(L"dark", kLightTheme)) 切换。
# 未在 XML 里写出的颜色沿用浅色主题，只覆盖需要改的字段即可。
function(aiui_add_themes target)
  if(NOT ARGN)
    message(FATAL_ERROR "aiui_add_themes(${target} ...) 至少需要一个 XML 文件")
  endif()

  get_filename_component(xml_abs "${ARGN}" ABSOLUTE)
  if(NOT EXISTS "${xml_abs}")
    message(FATAL_ERROR "主题文件不存在：${ARGN}")
  endif()

  set(out "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated/aiui_themes.cpp")
  add_custom_command(
    OUTPUT "${out}"
    COMMAND $<TARGET_FILE:AiUI::layoutgen> --themes "${xml_abs}" "${out}"
    DEPENDS "${xml_abs}" AiUI::layoutgen
    COMMENT "生成主题配色 ${ARGN}"
    VERBATIM
  )
  target_sources(${target} PRIVATE "${out}")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated")
  set_source_files_properties("${out}" PROPERTIES GENERATED TRUE)
endfunction()
