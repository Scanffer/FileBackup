# FileBackup.exe —— 唯一入口，CLI 与 Web GUI 共用（README §7.1 / §3.4）
#
# 无参数双击 → 拉起 Web GUI；带子命令 → 走 CLI。
# GUI 的服务端逻辑在 backup_server 里，这里只是把它链进来 ——
# 所以最终仍然只有一个 exe，不需要额外分发任何文件。

# ---------------------------------------------------------------------------
# 把 WebView2Loader.dll 编进资源段
#
# 为什么是资源而不是旁边放个文件：交付形态是**单个 exe**，多一个要分发的
# 文件就破坏了这一点。
#
# 为什么这段写在这里、而不是跟着 webview2/ 放 third_party/：
# add_custom_command 的规则是**目录作用域**的 —— 定义在 third_party/ 里的
# 规则，兄弟目录 cli/ 看不见（CMake 只把规则暴露给同目录及「之后才 add 的
# 子目录」，third_party 和 cli 是兄弟）。放那边会报：
#   ninja: error: 'third_party/webview2_loader_rc.o', needed by
#   'FileBackup.exe', missing and no known rule to make it
# 而资源对象又必须直接进可执行文件（理由见下），不能包成库绕过去，
# 所以干脆定义在真正消费它的这一层。
# ---------------------------------------------------------------------------
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
    COMMENT "把 WebView2Loader.dll 编进资源段"
    VERBATIM
)

add_executable(FileBackup
    main.cpp
    # 必须加在这一层，而不是塞进 backup_server 静态库：纯资源的 .o 不定义
    # 任何符号，静态库只把「能解决未定义符号」的成员拉进链接，它会被整个丢掉。
    ${WEBVIEW2_RES_OBJ}
)

target_link_libraries(FileBackup
    PRIVATE backup_core
    PRIVATE backup_server
)

# -municode：让 CRT 用 wmain 而不是 main 作为入口。CLI 必须用 wmain，
# 否则拿不到 UTF-16 参数，中文路径会让程序崩溃（见 cli/main.cpp 的说明）。
# 只给主程序加 —— 测试程序用的是普通 main。
if(MINGW)
    target_link_options(FileBackup PRIVATE -municode)
endif()

# 产物应为单 exe，无需随附任何 DLL（README §3.4）。
# 由根 CMakeLists.txt 里 MINGW 分支的 -static 系列开关保证。
set_target_properties(FileBackup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
