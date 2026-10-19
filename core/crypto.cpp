#include "algorithm.h"

#include <algorithm>
#include <array>

namespace backup {
namespace {

/// 标准 CRC32（IEEE 802.3 / zlib 多项式 0xEDB88320）查表。
std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

}  // namespace

// 由 crypto 模块实现（AES-256-CBC / ChaCha20-Poly1305 / PBKDF2 走 Windows
// CNG；RC4 自研）。本文件在骨架阶段只提供注册表与转换。

const char* to_string(EncryptionId id) {
    switch (id) {
        case EncryptionId::None: return "none";
        case EncryptionId::Aes256Cbc: return "aes256cbc";
        case EncryptionId::ChaCha20Poly1305: return "chacha20poly1305";
        case EncryptionId::Rc4: return "rc4";
    }
    return "none";
}

std::optional<EncryptionId> encryption_from_string(const std::string& s) {
    if (s == "none") return EncryptionId::None;
    if (s == "aes256cbc") return EncryptionId::Aes256Cbc;
    if (s == "chacha20poly1305") return EncryptionId::ChaCha20Poly1305;
    if (s == "rc4") return EncryptionId::Rc4;
    return std::nullopt;
}

const std::vector<AlgorithmInfo>& encryption_algorithms() {
    static const std::vector<AlgorithmInfo> list = {
        {"none", "不加密", false, false, ""},
        {"aes256cbc", "AES-256-CBC", false, false, "Windows CNG，PBKDF2-SHA256 派生密钥"},
        {"chacha20poly1305", "ChaCha20-Poly1305", false, false, "Windows CNG，AEAD"},
        {"rc4", "RC4", false, true, "自研流密码（教学用，不安全）"},
    };
    return list;
}

std::unique_ptr<ICipher> make_cipher(EncryptionId /*id*/) {
    return nullptr;  // 骨架阶段：由加密算法 agent 实现
}

Bytes derive_key(const std::string& /*password*/, const Bytes& /*salt*/,
                 std::uint32_t /*iterations*/, std::size_t /*key_size*/,
                 std::string& error) {
    error = "密钥派生尚未实现";
    return {};
}

Bytes random_bytes(std::size_t /*count*/) { return {}; }

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = make_crc32_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const Bytes& data) {
    return crc32(data.data(), data.size());
}

std::string hex_encode(const Bytes& /*data*/) { return {}; }
std::string base64_encode(const Bytes& /*data*/) { return {}; }
Bytes base64_decode(const std::string& /*text*/, std::string& error) {
    error = "Base64 尚未实现";
    return {};
}

}  // namespace backup
