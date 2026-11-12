# aiui_add_app_entry(<target>)：把平台入口接进宿主可执行目标
#
# 各平台的图形入口不同（Windows 是 wWinMain，macOS 是 NSApplication，
# Linux 是 main）。这些细节属于 AiUI 的后端，应用只实现 AiUI::runApp()。
#
# 入口源文件必须直接编进可执行目标：放在静态库里时，没有任何符号引用
# 它，链接器会把整个目标文件丢掉，最终报「找不到 WinMain」。

# CMAKE_CURRENT_FUNCTION_LIST_DIR 指向本文件所在目录，不受调用方目录影响
function(aiui_add_app_entry target)
  if(WIN32)
    target_sources(${target} PRIVATE
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/os/windows/entry_windows.cpp")
  elseif(APPLE)
    message(FATAL_ERROR "AiUI 的 macOS 入口尚未实现")
  elseif(UNIX)
    message(FATAL_ERROR "AiUI 的 Linux 入口尚未实现")
  endif()
endfunction()
