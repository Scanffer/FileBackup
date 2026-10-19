# backup_server —— Web GUI 的 HTTP 层（静态库）
#
# 这里**没有 main()**。程序入口始终是 cli/main.cpp 的 wmain，
# 这样 CLI 和 GUI 才能共用同一个 FileBackup.exe（README §3.4「单 exe 交付」）。
# server 只是被链接进去的一段代码，不是独立程序。
#
# 与 README §8 目录规划的两处有意偏离：
#   1. 那里写的是 server/main.cpp —— 但 main.cpp 这名字暗示独立 exe，
#      与「一个 exe」冲突，故改名为 http_server.cpp。
#   2. 那里规划了 web/ + tools/embed_web.py 在构建期把前端嵌进二进制。
#      这里改用 web_page.h 里的 raw string literal：同样能嵌进 exe，
#      但不需要把 Python 重新引入构建链（见根 CMakeLists 的说明）。

add_library(backup_server STATIC
    http_server.cpp
    http_server.h     # 仅让 IDE 显示出来，不参与编译
    tasks.cpp         # 异步任务管理器（单 worker 串行执行备份/还原）
    tasks.h           # 同上
    web_page.h        # 同上
    webview_host.cpp
    webview_host.h    # 同上
)

# PUBLIC 而不是 PRIVATE：http_server.h 是给 cli/ 用的公开接口，
# 链接 backup_server 的一方必须能找到它，否则 cli/main.cpp 会
# "fatal error: http_server.h: No such file or directory"。
target_include_directories(backup_server
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(backup_server
    PRIVATE backup_core
    PRIVATE cpp_httplib
    PRIVATE webview2_sdk
)

# 系统库显式写全。
#
# MinGW 的默认 specs 会带一部分（user32 / shell32 / advapi32），所以
# 之前只用 ShellExecuteW 的时候不写也编得过。但 ole32 不在默认里 ——
# 少了它，链接阶段会报 undefined reference to `__imp_CoInitializeEx'。
# 与其依赖「哪些在默认里」这个隐式约定，不如用到的都写出来。
#
# 另外，httplib.h 里那句 #pragma comment(lib, "ws2_32.lib") 是 MSVC 专有
# 语法，MinGW 会当未知 pragma 忽略（编译不报错，链接时才炸）：
#   undefined reference to `__imp_WSAStartup' / `__imp_select' / `__imp_bind'
#
# 这些是 PRIVATE，但会随 backup_server 传到最终 exe 的链接行，
# 所以 cli/ 那边不需要再写一遍。
if(WIN32)
    target_link_libraries(backup_server PRIVATE
        ws2_32    # winsock —— httplib 用
        ole32     # COM —— WebView2 用
        user32    # 窗口
        gdi32     # GetDeviceCaps / GetDC（按 DPI 算窗口尺寸）
        shell32   # ShellExecuteW —— 浏览器兜底
        dwmapi    # DwmSetWindowAttribute —— 深色标题栏
        uuid      # CLSID_FileOpenDialog / IID_IFileOpenDialog —— 原生选择框
    )
endif()

# 内嵌页面里有中文。GCC 默认输入输出就是 UTF-8，这里显式钉死，
# 免得文件哪天被别的编辑器存成 GBK，中文就烂在 exe 里了。
target_compile_options(backup_server PRIVATE
    -finput-charset=UTF-8 -fexec-charset=UTF-8
)
