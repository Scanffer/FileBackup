cmake_minimum_required(VERSION 3.20)

# 文件备份软件 —— 软件开发综合实验
# 详见 README.md（设计）
project(FileBackup VERSION 0.1.0 LANGUAGES C CXX)

# ---- C++ 标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# zstd 等第三方 C 库用 C11 就够。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- 编译器口径：MinGW-w64 GCC ----
# MinGW 和 MSVC 可能同时存在于 PATH 上，Ninja 生成器会静默挑一个能用的。
# 挑错了不会立刻报错，而是拖到链接阶段才给出莫名其妙的错误。
# 这里提前失败，并把「怎么办」写进错误信息。
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "本项目用 MinGW-w64 GCC 构建，但当前选中了 "
        "${CMAKE_CXX_COMPILER_ID}（${CMAKE_CXX_COMPILER}）。\n"
        "请确认 MinGW 的 bin 目录在 PATH 里，删掉 build/ 后重新配置。")
endif()

# ---- 给 VS Code 的 C/C++ 插件生成 compile_commands.json ----
# 没有它，编辑器找不到头文件路径，会把 #include 标成红线。
# 这只是给编辑器的提示，与能否编译成功无关。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- 单 exe 交付（README §3.4）----
# -static 系列：不加的话 exe 会依赖 libstdc++-6.dll、libgcc_s_seh-1.dll、
#   libwinpthread-1.dll —— 换一台没装 MinGW 的机器就跑不起来。
#
# -municode 不放在这里：它会让所有可执行文件都要求 wWinMain，而测试程序
# 用的是普通的 main。只给 FileBackup.exe 单独加（见 cli/CMakeLists.txt）。
#
# 验收方法：objdump -p build/mingw-release/FileBackup.exe | grep "DLL Name"
# 输出里不应出现上面那三个 dll。
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# 子目录
#   分层原则（README §4）：core 不依赖 HTTP、不依赖任何前端概念，
#   可被 cli 与 server 共同链接。
#
#   顺序注意：third_party 要先于 core —— core 链接 zstd/json 的 target。
# ---------------------------------------------------------------------------
add_subdirectory(third_party)  # httplib / WebView2 / zstd / json / Catch2
add_subdirectory(core)         # backup_core 静态库：备份引擎
add_subdirectory(server)       # backup_server 静态库：Web GUI 的 HTTP 层（无 main）
add_subdirectory(cli)          # FileBackup-cli.exe：命令行入口
if(WIN32)
    add_subdirectory(gui)      # FileBackup.exe：图形界面入口（无控制台）
endif()

enable_testing()
add_subdirectory(tests)        # backup_tests：Catch2 单元测试
