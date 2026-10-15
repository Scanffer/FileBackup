# cpp-httplib —— 单头文件 HTTP 库（v0.54.1，MIT 许可）。
#
# 做成 INTERFACE target，而不是把路径塞进全局 include_directories()：
# httplib.h 有 780 KB，只让 server/http_server.cpp 这一个编译单元看见它，
# 别拖慢 core/cli 的每次重编。
add_library(cpp_httplib INTERFACE)

target_include_directories(cpp_httplib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
