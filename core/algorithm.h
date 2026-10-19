#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace backup {

// ---------------------------------------------------------------------------
// 算法 ID
//
// 这些数值会写进 VBAK 的头，属于归档格式的一部分，**不要改已有值**。
// 新增算法只能往后追加。
// ---------------------------------------------------------------------------

enum class PackId : std::uint8_t {
    Vbak = 0,   ///< 自研单容器格式
    Ustar = 1,  ///< 兼容 tar（P1，暂未实现）
};

enum class CompressionId : std::uint8_t {
    None = 0,
    Deflate = 1,
    Lz4 = 2,
    Zstd = 3,
    Rle = 4,
    Lzw = 5,  ///< 自研，替代 README 原计划中的 Zstd 计分位
};

enum class EncryptionId : std::uint8_t {
    None = 0,
    Aes256Cbc = 1,
    ChaCha20Poly1305 = 2,
    Rc4 = 3,
};

const char* to_string(PackId id);
const char* to_string(CompressionId id);
const char* to_string(EncryptionId id);

std::optional<PackId> pack_from_string(const std::string& s);
std::optional<CompressionId> compression_from_string(const std::string& s);
std::optional<EncryptionId> encryption_from_string(const std::string& s);

/// 给 GUI / CLI 展示的算法元信息。
struct AlgorithmInfo {
    std::string id;        ///< 稳定 ID（小写），与 from_string 一致
    std::string label;     ///< 显示名
    bool has_level = false;
    bool self_made = false;  ///< true = 本项目自己实现，用于界面标注
    std::string note;
};

const std::vector<AlgorithmInfo>& compression_algorithms();
const std::vector<AlgorithmInfo>& encryption_algorithms();
const std::vector<AlgorithmInfo>& pack_algorithms();

/// 把压缩级别夹到算法支持的范围；None 算法忽略。
int clamp_compression_level(CompressionId id, int level);

// ---------------------------------------------------------------------------
// 压缩
// ---------------------------------------------------------------------------

class ICompressor {
public:
    virtual ~ICompressor() = default;
    virtual CompressionId id() const = 0;

    /// 压缩整块数据。失败返回空 vector 并写 error。
    virtual Bytes compress(const Bytes& input, int level, std::string& error) = 0;

    /// 解压整块数据。expected_size 是原始大小（VBAK 索引里有），
    /// 用于预分配与完整性校验；返回大小必须等于它。
    virtual Bytes decompress(const Bytes& input, std::size_t expected_size,
                             std::string& error) = 0;
};

std::unique_ptr<ICompressor> make_compressor(CompressionId id);

// ---------------------------------------------------------------------------
// 加密
// ---------------------------------------------------------------------------

class ICipher {
public:
    virtual ~ICipher() = default;
    virtual EncryptionId id() const = 0;
    virtual std::size_t key_size() const = 0;
    /// 写进 VBAK 索引的 IV / nonce 长度（AES=16, ChaCha=12, RC4=16）。
    virtual std::size_t iv_size() const = 0;

    /// 加密；AEAD 的认证标签包含在返回的密文尾部。
    virtual Bytes encrypt(const Bytes& plaintext, const Bytes& key,
                          const Bytes& iv, std::string& error) = 0;

    /// 解密；输入是 encrypt 的输出，iv 来自索引。
    virtual Bytes decrypt(const Bytes& ciphertext, const Bytes& key,
                          const Bytes& iv, std::string& error) = 0;
};

std::unique_ptr<ICipher> make_cipher(EncryptionId id);

/// PBKDF2-HMAC-SHA256 派生密钥（Windows CNG 实现）。
Bytes derive_key(const std::string& password, const Bytes& salt,
                 std::uint32_t iterations, std::size_t key_size,
                 std::string& error);

/// 密码学安全随机字节（BCryptGenRandom）。
Bytes random_bytes(std::size_t count);

/// CRC32（IEEE 802.3，与 zlib 兼容），用于条目/索引/全档校验。
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
std::uint32_t crc32(const Bytes& data);

/// 十六进制 / Base64 编码工具（元数据 JSON、调试输出用）。
std::string hex_encode(const Bytes& data);
std::string base64_encode(const Bytes& data);
Bytes base64_decode(const std::string& text, std::string& error);

}  // namespace backup
