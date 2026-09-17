# backup_core —— 备份引擎静态库
#
# 目录规划见 README §8。当前内容：
#   version.cpp   版本号
#   fs_utils.cpp  目录遍历
#
# 依赖：无，全部使用 C++17 标准库。
# 需要第三方库时再逐个引入（压缩 M2、加密 M3、打包 M6）。

add_library(backup_core STATIC
    version.cpp
    fs_utils.cpp
)

target_include_directories(backup_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# 版本号单一来源：project(FileBackup VERSION ...) 一处修改，全局同步。
target_compile_definitions(backup_core
    PRIVATE FB_VERSION="${PROJECT_VERSION}"
)
