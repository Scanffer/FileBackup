# 第三方依赖。都是「源码直接 vendored 进仓库」，不用任何包管理器 ——
# 新组员 clone 下来就能编，不需要装东西（见 README 的构建说明）。

# ---------------------------------------------------------------------------
# cpp-httplib —— 单头文件 HTTP 库（v0.54.1，MIT 许可）。
#
# 做成 INTERFACE target，而不是把路径塞进全局 include_directories()：
# httplib.h 有 780 KB，只让 server/http_server.cpp 这一个编译单元看见它，
# 别拖慢 core/cli 的每次重编。
# ---------------------------------------------------------------------------
add_library(cpp_httplib INTERFACE)

target_include_directories(cpp_httplib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# WebView2 SDK —— 微软的 Chromium 内核宿主（见 webview2/README.md）。
#
# 和 httplib 一样是 INTERFACE：WebView2.h 有 2.95 MB / 约 3 万行，
# 只让 server/webview_host.cpp 一个编译单元看见。
# ---------------------------------------------------------------------------
add_library(webview2_sdk INTERFACE)

target_include_directories(webview2_sdk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2
)

# ---------------------------------------------------------------------------
# Zstandard —— 压缩算法（README 2.1 原计划的算法之一，作为附加能力保留；
# 计分主力是自研的 DEFLATE / LZ4 / LZW / RLE）。
#
# 完整源码 vendored，用官方 CMake 构建静态库 libzstd_static。
# 所有会生成程序/测试/共享库的开关都关掉，避免拖慢构建。
# ---------------------------------------------------------------------------
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/zstd/build/cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zstd_build
    EXCLUDE_FROM_ALL
)

# ---------------------------------------------------------------------------
# nlohmann/json —— 单头文件 JSON（manifest、REST API、筛选规则）。
# ---------------------------------------------------------------------------
add_library(fb_json INTERFACE)

target_include_directories(fb_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/json
)

# ---------------------------------------------------------------------------
# Catch2 v2 —— 单头文件单元测试框架（test_main.cpp 里 #define CATCH_CONFIG_MAIN）。
# ---------------------------------------------------------------------------
add_library(fb_catch2 INTERFACE)

target_include_directories(fb_catch2 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/catch2
)

# 注意：把 webview2/loader.rc 编成资源对象、再链进 exe 的活儿在
# cli/CMakeLists.txt 里 —— add_custom_command 的规则是**目录作用域**的，
# 写在这里的话兄弟目录 cli/ 看不见，会报
#   ninja: error: '...webview2_loader_rc.o', needed by 'FileBackup.exe',
#   missing and no known rule to make it
