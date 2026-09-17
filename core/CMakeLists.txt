# backup_core —— 备份引擎静态库
#
# 目录规划见 README §8。M0 只建立目标结构与依赖接线，
# 后续里程碑按 §10 逐步填入：
#   M1  fs_utils / manifest / repository / repo_mirror / pipeline
#   M2  compressor_* / crypto_*
#   M3  filter / metadata / special_file
#   M6  packer_* / repo_container
#   M7  scheduler / watcher_*

add_library(backup_core STATIC
    version.cpp
)

target_include_directories(backup_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# 版本号单一来源：project(FileBackup VERSION ...) 一处修改，全局同步。
target_compile_definitions(backup_core
    PRIVATE FB_VERSION="${PROJECT_VERSION}"
)

# M0 就把六个依赖全部接上：链接成功即证明 vcpkg 包名、triplet 与运行库口径
# 三者全部对齐。这正是 M0 应当排除的风险（README §十一 把 LNK2038 与
# triplet 选错列为「链接失败 / 演示事故」级风险）。
#
# 注意 STATIC 库本身不产生链接动作，真正的链接发生在 cli 与 tests 可执行文件，
# 因此 core/version.cpp 会真实调用各库的版本函数，让链接器必须去解析符号 ——
# 否则未使用的静态库不会触发 LNK2038，这个验证就是空的。
target_link_libraries(backup_core
    PUBLIC
        ${FB_ZLIB_TARGET}
        lz4::lz4
        zstd::libzstd
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
)
