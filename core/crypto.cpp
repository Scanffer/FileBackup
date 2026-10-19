#include "algorithm.h"

#include <algorithm>

namespace backup {

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

std::uint32_t crc32(const std::uint8_t* /*data*/, std::size_t /*size*/) {
    return 0;
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
