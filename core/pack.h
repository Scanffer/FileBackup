#pragma once

#include <cstdint>
#include <filesystem>

#include "algorithm.h"
#include "filter.h"
#include "metadata.h"

/*
VBAK 归档格式（全部小端）
================================================================
v1（历史版本，只读兼容）
  Header 20 字节：magic/version=1/flags/entry_count/reserved/data_off
  Entry：path_len u16 + path + data_size u64 + checksum u32 + data

v2（未加密，当前默认）
  Header 20 字节
    magic "VBAK" 4
    version u16 = 2
    flags u16
    comp_id u8
    reserved u8[3]
    entry_count u32
    data_off u32 = 20
  Entry
    path_len u16 + path
    orig_size u64
    stor_size u64
    crc32 u32（原始内容）
    comp_id u8（0=原样存储，否则条目实际使用的压缩算法）
    data stor_size 字节

v3（加密，存在加密参数时写出）
  Header 64 字节
    magic "VBAK" 4
    version u16 = 3
    flags u16（bit0=1 表示加密）
    comp_id u8      ← 归档默认压缩算法
    enc_id u8       ← 加密算法
    reserved u16
    entry_count u32
    data_off u32 = 64
    kdf_iterations u32
    salt u8[16]     ← PBKDF2 盐
    reserved2 u8[24]
  Entry
    path_len u16 + path
    orig_size u64
    stor_size u64（含 AEAD tag / CBC padding）
    crc32 u32（原始内容）
    comp_id u8
    iv u8[16]       ← 实际使用前 iv_size 字节（AES 16 / ChaCha 12 / RC4 16）
    data stor_size 字节

v4（当前版本：可选元数据）
  Header 64 字节，布局与 v3 相同，只把 version 改为 4：
    高亮：flags bit0 = 加密，bit1 = 条目带元数据（MetaBlock）
  Entry
    path_len u16 + path
    orig_size u64
    stor_size u64（含 AEAD tag / CBC padding）
    crc32 u32（原始内容）
    comp_id u8
    [若 flags.bit0=1] iv u8[16]
    meta_len u32
    meta meta_len 字节（metadata_to_bytes 的版本化二进制；0 表示无）
    data stor_size 字节

约定：
  - 每条目独立压缩、独立 IV 加密；次序是「先压缩，再加密」；
  - 密码不写入归档；PBKDF2 派生一次密钥，供所有条目使用；
  - 不可压缩的文件仍会以 comp_id=0 存储，避免归档变大；
  - keep_metadata=true 时写 v4 并把元数据放进每条目；
  - v1/v2/v3 归档仍可解包；新归档写 v4。
*/

namespace backup {

/// 打包选项。
struct PackOptions {
    std::filesystem::path src;      ///< 源目录
    std::filesystem::path archive;  ///< 输出 .vbak 文件路径
    CompressionId compression = CompressionId::None;
    int compression_level = 6;

    /// 加密算法；非 None 时写出加密归档，password 必须非空。
    EncryptionId encryption = EncryptionId::None;
    std::string password;

    /// 六维筛选；空 Filter 表示全选。
    Filter filter;

    /// 是否把文件元数据（权限/属主/时间戳/属性/ACL）写入归档。
    bool keep_metadata = true;
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
    std::string password;           ///< 加密归档需要
    bool restore_metadata = true;   ///< 还原 v4 归档里的元数据
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
