# FileBackup.exe —— 图形界面入口（Windows GUI 子系统）
#
# 和 cli/ 里的 FileBackup-cli.exe 分开链接：
#   gui 目标用 -mwindows（无控制台）+ -municode（wWinMain）+ 资源段的
#   WebView2Loader.dll，双击就是一个干净的应用窗口。
#
# WebView2Loader.dll 的资源段编译规则放在这里（消费它的目标所在目录），
# 原因见下面 add_custom_command 的注释。

find_program(WINDRES_EXECUTABLE windres)
if(NOT WINDRES_EXECUTABLE)
    message(FATAL_ERROR
        "找不到 windres。它随 MinGW-w64 一起安装，正常应在 PATH 里。\n"
        "本项目用 MinGW 构建（见根 CMakeLists.txt），换成别的工具链需要\n"
        "另行提供资源编译器。")
endif()

set(WEBVIEW2_DIR ${CMAKE_SOURCE_DIR}/third_party/webview2)
set(WEBVIEW2_RES_OBJ ${CMAKE_CURRENT_BINARY_DIR}/webview2_loader_rc.o)

# WORKING_DIRECTORY 钉在 DLL 旁边：RCDATA 里那个文件名是相对 windres 的
# **工作目录**解析的，而自定义命令默认在构建目录里跑。这样 loader.rc 里
# 就只用写裸文件名，不用拼相对路径。
add_custom_command(
    OUTPUT ${WEBVIEW2_RES_OBJ}
    COMMAND ${WINDRES_EXECUTABLE} loader.rc -o ${WEBVIEW2_RES_OBJ}
    WORKING_DIRECTORY ${WEBVIEW2_DIR}
    DEPENDS ${WEBVIEW2_DIR}/loader.rc ${WEBVIEW2_DIR}/WebView2Loader.dll
    COMMENT "把 WebView2Loader.dll 编进 GUI exe 的资源段"
    VERBATIM
)

add_executable(FileBackupGui
    main.cpp
    # 纯资源 .o 不定义任何符号，不能塞进静态库（会被整个丢掉），
    # 必须直接作为可执行文件的输入。
    ${WEBVIEW2_RES_OBJ}
)

target_link_libraries(FileBackupGui
    PRIVATE backup_core
    PRIVATE backup_server
)

# -mwindows：Windows GUI 子系统，不创建控制台。
# -municode：使用 wWinMain 入口（UTF-16）。
if(MINGW)
    target_link_options(FileBackupGui PRIVATE -mwindows -municode)
endif()

# 交付形态：GUI 程序就叫 FileBackup.exe，放在构建根目录，方便双击。
set_target_properties(FileBackupGui PROPERTIES
    OUTPUT_NAME "FileBackup"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
