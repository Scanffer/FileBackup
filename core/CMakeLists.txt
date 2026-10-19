# backup_core —— 备份引擎静态库
#
# 分层原则（README §4）：core 不依赖 HTTP、不依赖任何前端概念，
# 可被 cli 与 server 共同链接。

add_library(backup_core STATIC
    version.cpp
    fs_utils.cpp
    backup.cpp
    pack.cpp
    compress.cpp
    crypto.cpp
    filter.cpp
    metadata.cpp
    archive.cpp
)

target_include_directories(backup_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# 版本号单一来源：project(FileBackup VERSION ...) 一处修改，全局同步。
target_compile_definitions(backup_core
    PRIVATE FB_VERSION="${PROJECT_VERSION}"
)

# 第三方依赖：
#   fb_json 是 PUBLIC —— filter.h / metadata.h 的公开头里用了 nlohmann::json，
#   链接 backup_core 的 cli/server/tests 也必须能找到这个头。
target_link_libraries(backup_core
    PUBLIC fb_json
    PRIVATE libzstd_static
)

if(WIN32)
    # bcrypt：random_bytes 用 BCryptGenRandom（只做安全随机源）。
    # advapi32：元数据的属主/ACL 读写（GetNamedSecurityInfo / SetFileSecurity 等）。
    target_link_libraries(backup_core PRIVATE bcrypt advapi32)
endif()
