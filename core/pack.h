#pragma once

#include <cstdint>
#include <filesystem>

#include "algorithm.h"

/*
VBAK v2 归档格式（全部小端）
================================================================
Header (24 字节)
  magic       "VBAK"     4
  version     uint16     2   ← 2
  flags       uint16     2   ← 预留（加密/索引等）
  comp_id     uint8      1   ← 归档默认压缩算法（0=none,1=deflate,...）
  reserved    uint8[3]   3
  entry_count uint32     4
  data_off    uint32     4   ← 24

Entry × entry_count
  path_len    uint16     2
  path        UTF-8      path_len 字节
  orig_size   uint64     8   ← 原始大小
  stor_size   uint64     8   ← 存储大小（压缩后）
  crc32       uint32     4   ← 原始内容 CRC32
  comp_id     uint8      1   ← 本条目实际使用的压缩算法（0=未压缩）
  data        stor_size 字节

说明：
  - 压缩是逐条目独立的，comp_id 为 0 表示该条目直接存原数据；
    对不可压缩的文件（压缩后反而变大）会自动退化为原始存储。
  - v1 归档（没有 compression/crc 字段）仍可解包。
*/

namespace backup {

/// 打包选项。
struct PackOptions {
    std::filesystem::path src;      ///< 源目录
    std::filesystem::path archive;  ///< 输出 .vbak 文件路径
    CompressionId compression = CompressionId::None;
    int compression_level = 6;
};

struct PackResult {
    std::uintmax_t entries_written = 0;
    std::uintmax_t bytes_written = 0;  ///< 原始字节数
};

/// 解包选项。
struct UnpackOptions {
    std::filesystem::path archive;  ///< 输入 .vbak 文件
    std::filesystem::path dst;      ///< 还原目标目录
    bool overwrite = true;          ///< 目标已存在时是否覆盖
};

struct UnpackResult {
    std::uintmax_t entries_read = 0;
    std::uintmax_t bytes_read = 0;  ///< 原始字节数
};

/// 把 src 整棵树打包成单个归档文件（.vbak）。
/// 失败抛 std::runtime_error（UTF-8 消息）。
PackResult pack_directory(const PackOptions& opts);

/// 把归档文件解包到 dst 目录。
/// 失败抛 std::runtime_error。会拒绝含 ".." 的条目路径（防 zip-slip）。
UnpackResult unpack_archive(const UnpackOptions& opts);

}  // namespace backup
