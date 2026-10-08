#pragma once

#include <cstdint>
#include <filesystem>

/*
Header (20 字节，全部小端)
  magic       "VBAK"     4
  version     uint16     2   ← 现在 = 1
  flags       uint16     2   ← 预留压缩/加密
  entry_count uint32     4
  reserved    uint32     4
  data_off    uint32     4   ← 现在 = 20

Entry × entry_count
  path_len    uint16     2
  path        UTF-8      path_len 字节
  data_size   uint64     8
  checksum    uint32     4   ← 现在写 0，M4 启用
  data        data_size 字节
*/

namespace backup {

/// 打包选项。
struct PackOptions {
    std::filesystem::path src;      ///< 源目录
    std::filesystem::path archive;  ///< 输出 .vbak 文件路径
};

struct PackResult {
    std::uintmax_t entries_written = 0;
    std::uintmax_t bytes_written = 0;
};

/// 解包选项。
struct UnpackOptions {
    std::filesystem::path archive;  ///< 输入 .vbak 文件
    std::filesystem::path dst;      ///< 还原目标目录
    bool overwrite = true;          ///< 目标已存在时是否覆盖
};

struct UnpackResult {
    std::uintmax_t entries_read = 0;
    std::uintmax_t bytes_read = 0;
};

/// 把 src 整棵树打包成单个归档文件（.vbak）。
/// 失败抛 std::runtime_error（UTF-8 消息）。
PackResult pack_directory(const PackOptions& opts);

/// 把归档文件解包到 dst 目录。
/// 失败抛 std::runtime_error。会拒绝含 ".." 的条目路径（防 zip-slip）。
UnpackResult unpack_archive(const UnpackOptions& opts);

}  // namespace backup