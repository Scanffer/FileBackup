cmake_minimum_required(VERSION 3.20)

# 文件备份软件 —— 软件开发综合实验
# 详见 README.md（设计）与 SETUP.md（环境配置）
project(FileBackup VERSION 0.1.0 LANGUAGES CXX)

# ---- C++ 标准：SETUP.md §7 锁定 C++17 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 给 VS Code 的 C/C++ 插件生成 compile_commands.json ----
# 没有它，编辑器不知道 vcpkg 的头文件（在 $VCPKG_ROOT/installed/ 下）在哪，
# 会把 #include <lz4.h> 标成红线。注意：这只是编辑器的提示，
# 跟能不能编译成功无关 —— 编译器是看 CMake 传的参数，不看这个文件。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- 单 exe 交付的静态运行库口径（README §3.4） ----
# CMakePresets.json 已经设了同名 cache 变量；这里只是兜底，
# 方便不用 presets 直接 cmake -B build 的人也不会踩到 LNK2038。
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- 源码含中文注释，MSVC 默认按 GBK 解析会报 C2001（SETUP.md §6） ----
# 这个开关是必须的，不是可选项。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# 依赖：全部由 vcpkg manifest 模式提供（vcpkg.json）
# 六个库一个都不能少 —— 压缩/加密是必选功能，JSON 与测试框架贯穿必选全程。
# ---------------------------------------------------------------------------
find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

# zlib 的目标名视查找方式而定：
#   MODULE 模式（vcpkg 的 vcpkg-cmake-wrapper.cmake）→ ZLIB::ZLIB
#   CONFIG 模式（ZLIBConfig.cmake + ZLIB-static.cmake）→ ZLIB::ZLIBSTATIC
if(TARGET ZLIB::ZLIB)
    set(FB_ZLIB_TARGET ZLIB::ZLIB)
elseif(TARGET ZLIB::ZLIBSTATIC)
    set(FB_ZLIB_TARGET ZLIB::ZLIBSTATIC)
else()
    message(FATAL_ERROR "找到 zlib 但没有任何已知的导入目标（ZLIB::ZLIB / ZLIB::ZLIBSTATIC）")
endif()

message(STATUS "FileBackup: vcpkg triplet = ${VCPKG_TARGET_TRIPLET}")
message(STATUS "FileBackup: zlib 目标 = ${FB_ZLIB_TARGET}")

# ---------------------------------------------------------------------------
# 子目录
#   分层原则（README §4）：core 不依赖 HTTP、不依赖任何前端概念，
#   可被 cli 与 server 共同链接。因此 GUI 延期不影响核心功能演示。
# ---------------------------------------------------------------------------
add_subdirectory(core)    # backup_core 静态库：备份引擎
add_subdirectory(cli)     # FileBackup.exe：命令行入口

# server/ 与 desktop/ 在 M0 尚无可编译内容，按 README §8 占位不构建：
#   server/  —— M4 接入 HTTP API + SSE
#   desktop/ —— M9 可选桌面外壳

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
