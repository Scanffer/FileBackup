#include "algorithm.h"

// ---------------------------------------------------------------------------
// crypto.cpp —— 全部密码学算法自研实现（不调用 OpenSSL / Windows CNG 的算法
// 接口；仅 random_bytes 使用 BCryptGenRandom 作为系统随机源）。
//
//   本文件包含：
//     SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256       (FIPS 180-4 / RFC 2104 /
//     RFC 8018) AES-256 + CBC + PKCS#7                          (FIPS 197 /
//     NIST SP 800-38A) ChaCha20-Poly1305                               (RFC
//     8439) RC4                                             (教学用流密码)
//   Base64 / hex / CRC32 / 加密算法注册表等工具函数。
//
// 设计要点（与 VBAK v3 归档格式的约定一致）：
//   * AES-256-CBC：key 32 字节、IV 16 字节，PKCS#7 填充（空明文也补一块）。
//   * ChaCha20-Poly1305：key 32 字节、nonce 12 字节；16 字节认证标签附在
//     密文尾部（ciphertext || tag），解密时先校验标签再解密。ICipher 没有
//     AAD 入口，生产路径使用空 AAD；内部 crypto_detail 入口支持 AAD，用于
//     RFC 8439 §2.8.2 标准向量测试。
//   * RC4：key 32 字节、IV 16 字节，实际流密钥 = SHA-256(key || iv)（把
//     32+16 字节压成 32 字节 RC4 密钥，避免直接使用短口令）。RC4 没有认证，
//     篡改/错误密钥无法检测，仅作教学与格式兼容用途。
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif
// 顺序不能反：bcrypt.h 依赖 windows.h 先带入 ULONG / NTSTATUS / NCRYPT_KEY_HANDLE
// 等基础类型，反过来会报 “'NTSTATUS' does not name a type”。
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace backup {
namespace {

// ===========================================================================
// 字节序与位运算小工具
// ===========================================================================

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
// 循环左移位
std::uint32_t rotl32(std::uint32_t x, unsigned n) { 
    return (x << n) | (x >> (32 - n));
}
// 循环右移位
std::uint32_t rotr32(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
//大端
std::uint32_t load_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

void store_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void store_be64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<std::uint8_t>(v);
        v >>= 8;
    }
}
//小端
std::uint32_t load_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void store_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

void store_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v);
        v >>= 8;
    }
}

/// 常数时间比较：用于认证标签校验，避免按字节提前返回泄露信息。
bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b,
                         std::size_t size) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < size; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

/// 尽量清除栈上的密钥材料（编译器优化时尽量少留痕；不保证绝对可靠）。
void secure_wipe(void* data, std::size_t size) {
    volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}

// ===========================================================================
// SHA-256（FIPS 180-4）
// ===========================================================================

constexpr std::size_t kSha256BlockSize = 64;
constexpr std::size_t kSha256DigestSize = 32;

class Sha256 {
   public:
    Sha256() { reset(); }

    void reset() {
        static constexpr std::uint32_t kInitial[8] = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::copy(kInitial, kInitial + 8, state_.begin());
        block_len_ = 0;
        total_len_ = 0;
    }

    void update(const std::uint8_t* data, std::size_t size) {
        total_len_ += size;
        while (size > 0) {
            const std::size_t take =
                std::min(size, kSha256BlockSize - block_len_);
            std::memcpy(block_.data() + block_len_, data, take);
            block_len_ += take;
            data += take;
            size -= take;
            if (block_len_ == kSha256BlockSize) {
                compress(block_.data());
                block_len_ = 0;
            }
        }
    }

    std::array<std::uint8_t, kSha256DigestSize> finish() {
        const std::uint64_t bit_len = total_len_ * 8;  // 调用前的消息长度
        block_[block_len_++] = 0x80;
        if (block_len_ > 56) {
            while (block_len_ < kSha256BlockSize) block_[block_len_++] = 0;
            compress(block_.data());
            block_len_ = 0;
        }
        while (block_len_ < 56) block_[block_len_++] = 0;
        store_be64(block_.data() + 56, bit_len);
        compress(block_.data());
        block_len_ = 0;

        std::array<std::uint8_t, kSha256DigestSize> digest{};
        for (std::size_t i = 0; i < 8; ++i) {
            store_be32(digest.data() + i * 4, state_[i]);
        }
        return digest;
    }

   private:
    void compress(const std::uint8_t* block) {
        static constexpr std::uint32_t kRound[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2],
                      d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6],
                      h = state_[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 =
                rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kRound[i] + w[i];
            const std::uint32_t s0 =
                rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, kSha256BlockSize> block_{};
    std::size_t block_len_ = 0;
    std::uint64_t total_len_ = 0;
};

std::array<std::uint8_t, kSha256DigestSize> sha256_digest(
    const std::uint8_t* data, std::size_t size) {
    Sha256 hasher;
    hasher.update(data, size);
    return hasher.finish();
}

Bytes sha256_bytes(const Bytes& data) {
    const auto digest = sha256_digest(data.data(), data.size());
    return Bytes(digest.begin(), digest.end());
}

// ===========================================================================
// HMAC-SHA256（RFC 2104）
// ===========================================================================

std::array<std::uint8_t, kSha256DigestSize> hmac_sha256_digest(
    const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
    std::size_t data_size) {
    std::array<std::uint8_t, kSha256BlockSize> key_block{};
    if (key_size > kSha256BlockSize) {
        const auto hashed = sha256_digest(key, key_size);
        std::memcpy(key_block.data(), hashed.data(), hashed.size());
    } else if (key_size > 0) {
        std::memcpy(key_block.data(), key, key_size);
    }

    std::array<std::uint8_t, kSha256BlockSize> inner_pad{};
    std::array<std::uint8_t, kSha256BlockSize> outer_pad{};
    for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
        inner_pad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36);
        outer_pad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5c);
    }

    Sha256 inner;
    inner.update(inner_pad.data(), inner_pad.size());
    inner.update(data, data_size);
    const auto inner_digest = inner.finish();

    Sha256 outer;
    outer.update(outer_pad.data(), outer_pad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

Bytes hmac_sha256_bytes(const Bytes& key, const Bytes& data) {
    const auto mac =
        hmac_sha256_digest(key.data(), key.size(), data.data(), data.size());
    return Bytes(mac.begin(), mac.end());
}

// ===========================================================================
// PBKDF2-HMAC-SHA256（RFC 8018 §5.2）
// ===========================================================================

Bytes pbkdf2_hmac_sha256_bytes(const std::uint8_t* password,
                               std::size_t password_size, const Bytes& salt,
                               std::uint32_t iterations, std::size_t key_size) {
    const std::size_t block_count =
        (key_size + kSha256DigestSize - 1) / kSha256DigestSize;
    Bytes derived;
    derived.reserve(key_size);
    for (std::size_t block = 1; block <= block_count; ++block) {
        // U1 = HMAC(password, salt || INT_32_BE(block_index))
        Bytes message = salt;
        std::uint8_t index_bytes[4];
        store_be32(index_bytes, static_cast<std::uint32_t>(block));
        message.insert(message.end(), index_bytes, index_bytes + 4);

        auto u = hmac_sha256_digest(password, password_size, message.data(),
                                    message.size());
        Bytes t(u.begin(), u.end());
        Bytes current(u.begin(), u.end());
        for (std::uint32_t round = 2; round <= iterations; ++round) {
            auto next = hmac_sha256_digest(password, password_size,
                                           current.data(), current.size());
            for (std::size_t i = 0; i < t.size(); ++i) t[i] ^= next[i];
            current.assign(next.begin(), next.end());
        }
        const std::size_t remaining = key_size - derived.size();
        const std::size_t take = std::min(remaining, t.size());
        derived.insert(derived.end(), t.begin(),
                       t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return derived;
}

// ===========================================================================
// AES-256（FIPS 197）：S 盒现场推导 + 密钥扩展 + 分组加解密
// ===========================================================================

constexpr std::size_t kAesBlockSize = 16;
constexpr std::size_t kAes256Rounds = 14;

/// GF(2^8) 乘法，模多项式 x^8 + x^4 + x^3 + x + 1（0x11B）。
std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if (b & 1u) product ^= a;
        const bool high = (a & 0x80u) != 0;
        a = static_cast<std::uint8_t>(a << 1);
        if (high) a ^= 0x1bu;
        b = static_cast<std::uint8_t>(b >> 1);
    }
    return product;
}

/// GF(2^8) 乘法逆元：a^254（a != 0），0 的逆元按 AES 约定为 0。
std::uint8_t gf_inv(std::uint8_t a) {
    if (a == 0) return 0;
    std::uint8_t result = 1;
    for (int i = 0; i < 254; ++i) result = gf_mul(result, a);
    return result;
}

std::uint8_t rotl8(std::uint8_t value, unsigned bits) {
    return static_cast<std::uint8_t>((value << bits) | (value >> (8 - bits)));
}

std::uint8_t xtime(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 1) ^
                                     ((value & 0x80u) ? 0x1bu : 0x00u));
}

struct AesTables {
    std::array<std::uint8_t, 256> sbox{};
    std::array<std::uint8_t, 256> inv_sbox{};
    std::array<std::uint8_t, 256> mul2{};
    std::array<std::uint8_t, 256> mul3{};
    std::array<std::uint8_t, 256> mul9{};
    std::array<std::uint8_t, 256> mul11{};
    std::array<std::uint8_t, 256> mul13{};
    std::array<std::uint8_t, 256> mul14{};
};

const AesTables& aes_tables() {
    static const AesTables tables = [] {
        AesTables t{};
        for (int value = 0; value < 256; ++value) {
            const auto v = static_cast<std::uint8_t>(value);
            const std::uint8_t inv = gf_inv(v);
            // 仿射变换：s = inv ^ rotl(inv,1) ^ rotl(inv,2) ^ rotl(inv,3)
            //                  ^ rotl(inv,4) ^ 0x63
            t.sbox[v] = static_cast<std::uint8_t>(
                inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                rotl8(inv, 4) ^ 0x63u);
            t.mul2[v] = xtime(v);
            t.mul3[v] = static_cast<std::uint8_t>(t.mul2[v] ^ v);
            t.mul9[v] = gf_mul(v, 9);
            t.mul11[v] = gf_mul(v, 11);
            t.mul13[v] = gf_mul(v, 13);
            t.mul14[v] = gf_mul(v, 14);
        }
        for (int value = 0; value < 256; ++value) {
            t.inv_sbox[t.sbox[value]] = static_cast<std::uint8_t>(value);
        }
        return t;
    }();
    return tables;
}

/// 扩展后的 AES-256 轮密钥：15 组 × 16 字节。
struct Aes256Key {
    std::array<std::uint8_t, 15 * 16> round_key{};
};

/// AES-256 密钥扩展（Nk=8，Nr=14，共 60 个 32-bit 字）。
void aes256_expand_key(const std::uint8_t key[32], Aes256Key& expanded) {
    const AesTables& tables = aes_tables();
    std::uint8_t words[60][4];
    for (std::size_t i = 0; i < 8; ++i) {
        words[i][0] = key[i * 4 + 0];
        words[i][1] = key[i * 4 + 1];
        words[i][2] = key[i * 4 + 2];
        words[i][3] = key[i * 4 + 3];
    }

    std::uint8_t rcon = 0x01;
    for (std::size_t i = 8; i < 60; ++i) {
        std::uint8_t temp[4] = {words[i - 1][0], words[i - 1][1],
                                words[i - 1][2], words[i - 1][3]};
        if (i % 8 == 0) {
            // RotWord + SubWord + Rcon
            const std::uint8_t first = temp[0];
            temp[0] = static_cast<std::uint8_t>(tables.sbox[temp[1]] ^ rcon);
            temp[1] = tables.sbox[temp[2]];
            temp[2] = tables.sbox[temp[3]];
            temp[3] = tables.sbox[first];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            // SubWord
            for (std::uint8_t& byte : temp) byte = tables.sbox[byte];
        }
        for (std::size_t j = 0; j < 4; ++j) {
            words[i][j] = static_cast<std::uint8_t>(words[i - 8][j] ^ temp[j]);
        }
    }

    for (std::size_t i = 0; i < 60; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            expanded.round_key[i * 4 + j] = words[i][j];
        }
    }
}

void aes_add_round_key(std::uint8_t state[16], const std::uint8_t* round_key) {
    for (std::size_t i = 0; i < kAesBlockSize; ++i) {
        state[i] ^= round_key[i];
    }
}

void aes_sub_bytes(std::uint8_t state[16]) {
    const AesTables& tables = aes_tables();
    for (std::size_t i = 0; i < kAesBlockSize; ++i) {
        state[i] = tables.sbox[state[i]];
    }
}

void aes_inv_sub_bytes(std::uint8_t state[16]) {
    const AesTables& tables = aes_tables();
    for (std::size_t i = 0; i < kAesBlockSize; ++i) {
        state[i] = tables.inv_sbox[state[i]];
    }
}

/// 状态按列存放：state[4 * column + row]。
void aes_shift_rows(std::uint8_t state[16]) {
    std::uint8_t shifted[16];
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            shifted[4 * column + row] = state[4 * ((column + row) & 3u) + row];
        }
    }
    std::memcpy(state, shifted, kAesBlockSize);
}

void aes_inv_shift_rows(std::uint8_t state[16]) {
    std::uint8_t shifted[16];
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            shifted[4 * column + row] =
                state[4 * ((column + 4 - row) & 3u) + row];
        }
    }
    std::memcpy(state, shifted, kAesBlockSize);
}

void aes_mix_columns(std::uint8_t state[16]) {
    const AesTables& tables = aes_tables();
    for (std::size_t column = 0; column < 4; ++column) {
        const std::uint8_t a0 = state[4 * column + 0];
        const std::uint8_t a1 = state[4 * column + 1];
        const std::uint8_t a2 = state[4 * column + 2];
        const std::uint8_t a3 = state[4 * column + 3];
        const std::uint8_t all = static_cast<std::uint8_t>(a0 ^ a1 ^ a2 ^ a3);
        state[4 * column + 0] =
            static_cast<std::uint8_t>(a0 ^ all ^ tables.mul2[a0 ^ a1]);
        state[4 * column + 1] =
            static_cast<std::uint8_t>(a1 ^ all ^ tables.mul2[a1 ^ a2]);
        state[4 * column + 2] =
            static_cast<std::uint8_t>(a2 ^ all ^ tables.mul2[a2 ^ a3]);
        state[4 * column + 3] =
            static_cast<std::uint8_t>(a3 ^ all ^ tables.mul2[a3 ^ a0]);
    }
}

void aes_inv_mix_columns(std::uint8_t state[16]) {
    const AesTables& tables = aes_tables();
    for (std::size_t column = 0; column < 4; ++column) {
        const std::uint8_t a0 = state[4 * column + 0];
        const std::uint8_t a1 = state[4 * column + 1];
        const std::uint8_t a2 = state[4 * column + 2];
        const std::uint8_t a3 = state[4 * column + 3];
        state[4 * column + 0] =
            static_cast<std::uint8_t>(tables.mul14[a0] ^ tables.mul11[a1] ^
                                      tables.mul13[a2] ^ tables.mul9[a3]);
        state[4 * column + 1] =
            static_cast<std::uint8_t>(tables.mul9[a0] ^ tables.mul14[a1] ^
                                      tables.mul11[a2] ^ tables.mul13[a3]);
        state[4 * column + 2] =
            static_cast<std::uint8_t>(tables.mul13[a0] ^ tables.mul9[a1] ^
                                      tables.mul14[a2] ^ tables.mul11[a3]);
        state[4 * column + 3] =
            static_cast<std::uint8_t>(tables.mul11[a0] ^ tables.mul13[a1] ^
                                      tables.mul9[a2] ^ tables.mul14[a3]);
    }
}

void aes256_encrypt_block(const Aes256Key& key, const std::uint8_t input[16],
                          std::uint8_t output[16]) {
    std::uint8_t state[16];
    std::memcpy(state, input, kAesBlockSize);
    aes_add_round_key(state, key.round_key.data());

    for (std::size_t round = 1; round < kAes256Rounds; ++round) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, key.round_key.data() + round * 16);
    }

    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, key.round_key.data() + kAes256Rounds * 16);
    std::memcpy(output, state, kAesBlockSize);
}

void aes256_decrypt_block(const Aes256Key& key, const std::uint8_t input[16],
                          std::uint8_t output[16]) {
    std::uint8_t state[16];
    std::memcpy(state, input, kAesBlockSize);
    aes_add_round_key(state, key.round_key.data() + kAes256Rounds * 16);

    for (std::size_t round = kAes256Rounds - 1; round >= 1; --round) {
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        aes_add_round_key(state, key.round_key.data() + round * 16);
        aes_inv_mix_columns(state);
    }

    aes_inv_shift_rows(state);
    aes_inv_sub_bytes(state);
    aes_add_round_key(state, key.round_key.data());
    std::memcpy(output, state, kAesBlockSize);
}

/// AES-256-CBC + PKCS#7 填充。调用方保证 key=32、iv=16 字节。
Bytes aes256_cbc_encrypt_pkcs7(const Bytes& key, const Bytes& iv,
                               const Bytes& plaintext) {
    Aes256Key expanded;
    aes256_expand_key(key.data(), expanded);

    const std::size_t padding =
        kAesBlockSize - (plaintext.size() % kAesBlockSize);
    const std::size_t padded_size = plaintext.size() + padding;
    Bytes ciphertext(padded_size);

    std::uint8_t previous[kAesBlockSize];
    std::memcpy(previous, iv.data(), kAesBlockSize);

    for (std::size_t offset = 0; offset < padded_size;
         offset += kAesBlockSize) {
        std::uint8_t block[kAesBlockSize];
        for (std::size_t i = 0; i < kAesBlockSize; ++i) {
            const std::size_t position = offset + i;
            const std::uint8_t value = position < plaintext.size()
                                           ? plaintext[position]
                                           : static_cast<std::uint8_t>(padding);
            block[i] = static_cast<std::uint8_t>(value ^ previous[i]);
        }
        aes256_encrypt_block(expanded, block, ciphertext.data() + offset);
        std::memcpy(previous, ciphertext.data() + offset, kAesBlockSize);
        secure_wipe(block, sizeof(block));
    }
    secure_wipe(&expanded, sizeof(expanded));
    return ciphertext;
}

/// AES-256-CBC 解密 + PKCS#7 去填充。失败返回空并写 error。
Bytes aes256_cbc_decrypt_pkcs7(const Bytes& key, const Bytes& iv,
                               const Bytes& ciphertext, std::string& error) {
    if (ciphertext.empty() || ciphertext.size() % kAesBlockSize != 0) {
        error = "AES-256-CBC 密文长度非法（必须是 16 的正整数倍）";
        return {};
    }

    Aes256Key expanded;
    aes256_expand_key(key.data(), expanded);

    Bytes plaintext(ciphertext.size());
    std::uint8_t previous[kAesBlockSize];
    std::memcpy(previous, iv.data(), kAesBlockSize);

    for (std::size_t offset = 0; offset < ciphertext.size();
         offset += kAesBlockSize) {
        std::uint8_t block[kAesBlockSize];
        aes256_decrypt_block(expanded, ciphertext.data() + offset, block);
        for (std::size_t i = 0; i < kAesBlockSize; ++i) {
            plaintext[offset + i] =
                static_cast<std::uint8_t>(block[i] ^ previous[i]);
        }
        std::memcpy(previous, ciphertext.data() + offset, kAesBlockSize);
        secure_wipe(block, sizeof(block));
    }
    secure_wipe(&expanded, sizeof(expanded));

    // PKCS#7 校验：填充值 1..16，且末尾 pad 个字节都等于 pad。
    const std::uint8_t pad =
        plaintext[plaintext.size() - 1];  // plaintext 非空（ciphertext 非空）
    bool valid = pad >= 1 && pad <= kAesBlockSize &&
                 static_cast<std::size_t>(pad) <= plaintext.size();
    if (valid) {
        for (std::size_t i = 0; i < pad; ++i) {
            if (plaintext[plaintext.size() - 1 - i] != pad) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        secure_wipe(plaintext.data(), plaintext.size());
        error = "AES-256-CBC 解密失败：密钥错误或密文被篡改（PKCS#7 填充非法）";
        return {};
    }
    plaintext.resize(plaintext.size() - pad);
    return plaintext;
}

// ===========================================================================
// ChaCha20（RFC 8439 §2.3）+ Poly1305（§2.5）+ AEAD（§2.8）
// ===========================================================================

void chacha20_quarter_round(std::uint32_t& a, std::uint32_t& b,
                            std::uint32_t& c, std::uint32_t& d) {
    a += b;
    d ^= a;
    d = rotl32(d, 16);
    c += d;
    b ^= c;
    b = rotl32(b, 12);
    a += b;
    d ^= a;
    d = rotl32(d, 8);
    c += d;
    b ^= c;
    b = rotl32(b, 7);
}

/// 生成一个 64 字节 ChaCha20 密钥流块。
void chacha20_block(const std::uint8_t key[32], std::uint32_t counter,
                    const std::uint8_t nonce[12], std::uint8_t output[64]) {
    static constexpr std::uint32_t kSigma[4] = {0x61707865u, 0x3320646eu,
                                                0x79622d32u, 0x6b206574u};
    std::uint32_t state[16];
    state[0] = kSigma[0];
    state[1] = kSigma[1];
    state[2] = kSigma[2];
    state[3] = kSigma[3];
    for (std::size_t i = 0; i < 8; ++i) {
        state[4 + i] = load_le32(key + i * 4);
    }
    state[12] = counter;
    for (std::size_t i = 0; i < 3; ++i) {
        state[13 + i] = load_le32(nonce + i * 4);
    }

    std::uint32_t working[16];
    std::memcpy(working, state, sizeof(state));
    for (int round = 0; round < 10; ++round) {
        // 列混合
        chacha20_quarter_round(working[0], working[4], working[8], working[12]);
        chacha20_quarter_round(working[1], working[5], working[9], working[13]);
        chacha20_quarter_round(working[2], working[6], working[10],
                               working[14]);
        chacha20_quarter_round(working[3], working[7], working[11],
                               working[15]);
        // 对角混合
        chacha20_quarter_round(working[0], working[5], working[10],
                               working[15]);
        chacha20_quarter_round(working[1], working[6], working[11],
                               working[12]);
        chacha20_quarter_round(working[2], working[7], working[8], working[13]);
        chacha20_quarter_round(working[3], working[4], working[9], working[14]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
        store_le32(output + i * 4, working[i] + state[i]);
    }
}

/// ChaCha20 加/解密（异或密钥流）。counter 从 counter_start 开始递增。
void chacha20_xor(const std::uint8_t key[32], std::uint32_t counter_start,
                  const std::uint8_t nonce[12], const std::uint8_t* input,
                  std::size_t size, std::uint8_t* output) {
    std::uint8_t keystream[64];
    std::uint32_t counter = counter_start;
    for (std::size_t offset = 0; offset < size; offset += sizeof(keystream)) {
        chacha20_block(key, counter++, nonce, keystream);
        const std::size_t take =
            std::min<std::size_t>(sizeof(keystream), size - offset);
        for (std::size_t i = 0; i < take; ++i) {
            output[offset + i] =
                static_cast<std::uint8_t>(input[offset + i] ^ keystream[i]);
        }
    }
    secure_wipe(keystream, sizeof(keystream));
}

/// Poly1305 一次性认证器（RFC 8439 §2.5，26-bit limb 实现）。
class Poly1305 {
   public:
    explicit Poly1305(const std::uint8_t key[32]) {
        // r 按 RFC 要求 clamp（0x0ffffffc0ffffffc0ffffffc0fffffff）。
        r_[0] = load_le32(key + 0) & 0x3ffffffu;
        r_[1] = (load_le32(key + 3) >> 2) & 0x3ffff03u;
        r_[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ffu;
        r_[3] = (load_le32(key + 9) >> 6) & 0x3f03fffu;
        r_[4] = (load_le32(key + 12) >> 8) & 0x00fffffu;
        std::memcpy(s_, key + 16, sizeof(s_));
        h_[0] = h_[1] = h_[2] = h_[3] = h_[4] = 0;
        buffer_len_ = 0;
    }

    void update(const std::uint8_t* data, std::size_t size) {
        if (size == 0) return;
        if (buffer_len_ != 0) {
            const std::size_t take = std::min(size, 16 - buffer_len_);
            std::memcpy(buffer_.data() + buffer_len_, data, take);
            buffer_len_ += take;
            data += take;
            size -= take;
            if (buffer_len_ == 16) {
                process_block(buffer_.data(), 1u << 24);
                buffer_len_ = 0;
            }
        }
        while (size >= 16) {
            process_block(data, 1u << 24);
            data += 16;
            size -= 16;
        }
        if (size > 0) {
            std::memcpy(buffer_.data(), data, size);
            buffer_len_ = size;
        }
    }

    /// RFC 8439 §2.8：对 aad / ciphertext 做 16 字节零对齐。
    void pad16() {
        if (buffer_len_ == 0) return;
        static const std::uint8_t zeros[16] = {};
        update(zeros, 16 - buffer_len_);
    }

    void finish(std::uint8_t tag[16]) {
        // 最后不足 16 字节的块按 RFC 8439 §2.5 处理：补零后追加 1 字节，
        // 作为 2^(8*剩余字节数) 的“隐式高位”，不再加 2^128（hibit=0）。
        if (buffer_len_ > 0) {
            std::array<std::uint8_t, 16> last{};
            std::memcpy(last.data(), buffer_.data(), buffer_len_);
            last[buffer_len_] = 1;
            process_block(last.data(), 0);
            buffer_len_ = 0;
        }

        std::uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3],
                      h4 = h_[4];
        std::uint32_t carry = 0;
        carry = h1 >> 26;
        h1 &= 0x3ffffffu;
        h2 += carry;
        carry = h2 >> 26;
        h2 &= 0x3ffffffu;
        h3 += carry;
        carry = h3 >> 26;
        h3 &= 0x3ffffffu;
        h4 += carry;
        carry = h4 >> 26;
        h4 &= 0x3ffffffu;
        h0 += carry * 5;
        carry = h0 >> 26;
        h0 &= 0x3ffffffu;
        h1 += carry;

        // g = h + 5 - 2^130；若 h >= p 则取 g，否则取 h。
        std::uint32_t g0 = h0 + 5;
        carry = g0 >> 26;
        g0 &= 0x3ffffffu;
        std::uint32_t g1 = h1 + carry;
        carry = g1 >> 26;
        g1 &= 0x3ffffffu;
        std::uint32_t g2 = h2 + carry;
        carry = g2 >> 26;
        g2 &= 0x3ffffffu;
        std::uint32_t g3 = h3 + carry;
        carry = g3 >> 26;
        g3 &= 0x3ffffffu;
        std::uint32_t g4 = h4 + carry - (1u << 26);

        const std::uint32_t mask = (g4 >> 31) - 1;  // 有借位=0，否则全 1
        g0 &= mask;
        g1 &= mask;
        g2 &= mask;
        g3 &= mask;
        g4 &= mask;
        const std::uint32_t keep = ~mask;
        h0 = (h0 & keep) | g0;
        h1 = (h1 & keep) | g1;
        h2 = (h2 & keep) | g2;
        h3 = (h3 & keep) | g3;
        h4 = (h4 & keep) | g4;

        // 130-bit 结果拆成 4 个 32-bit 字，加上 s（key 的后 16 字节）。
        const std::uint32_t word0 = h0 | (h1 << 26);
        const std::uint32_t word1 = (h1 >> 6) | (h2 << 20);
        const std::uint32_t word2 = (h2 >> 12) | (h3 << 14);
        const std::uint32_t word3 = (h3 >> 18) | (h4 << 8);

        std::uint64_t sum =
            static_cast<std::uint64_t>(word0) + load_le32(s_ + 0);
        store_le32(tag + 0, static_cast<std::uint32_t>(sum));
        sum =
            static_cast<std::uint64_t>(word1) + load_le32(s_ + 4) + (sum >> 32);
        store_le32(tag + 4, static_cast<std::uint32_t>(sum));
        sum =
            static_cast<std::uint64_t>(word2) + load_le32(s_ + 8) + (sum >> 32);
        store_le32(tag + 8, static_cast<std::uint32_t>(sum));
        sum = static_cast<std::uint64_t>(word3) + load_le32(s_ + 12) +
              (sum >> 32);
        store_le32(tag + 12, static_cast<std::uint32_t>(sum));
    }

   private:
    void process_block(const std::uint8_t* block, std::uint32_t hibit) {
        const std::uint32_t r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3],
                            r4 = r_[4];
        const std::uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

        std::uint32_t h0 = h_[0] + (load_le32(block + 0) & 0x3ffffffu);
        std::uint32_t h1 = h_[1] + ((load_le32(block + 3) >> 2) & 0x3ffffffu);
        std::uint32_t h2 = h_[2] + ((load_le32(block + 6) >> 4) & 0x3ffffffu);
        std::uint32_t h3 = h_[3] + ((load_le32(block + 9) >> 6) & 0x3ffffffu);
        std::uint32_t h4 = h_[4] + ((load_le32(block + 12) >> 8) | hibit);

        std::uint64_t d0 = static_cast<std::uint64_t>(h0) * r0 +
                           static_cast<std::uint64_t>(h1) * s4 +
                           static_cast<std::uint64_t>(h2) * s3 +
                           static_cast<std::uint64_t>(h3) * s2 +
                           static_cast<std::uint64_t>(h4) * s1;
        std::uint64_t d1 = static_cast<std::uint64_t>(h0) * r1 +
                           static_cast<std::uint64_t>(h1) * r0 +
                           static_cast<std::uint64_t>(h2) * s4 +
                           static_cast<std::uint64_t>(h3) * s3 +
                           static_cast<std::uint64_t>(h4) * s2;
        std::uint64_t d2 = static_cast<std::uint64_t>(h0) * r2 +
                           static_cast<std::uint64_t>(h1) * r1 +
                           static_cast<std::uint64_t>(h2) * r0 +
                           static_cast<std::uint64_t>(h3) * s4 +
                           static_cast<std::uint64_t>(h4) * s3;
        std::uint64_t d3 = static_cast<std::uint64_t>(h0) * r3 +
                           static_cast<std::uint64_t>(h1) * r2 +
                           static_cast<std::uint64_t>(h2) * r1 +
                           static_cast<std::uint64_t>(h3) * r0 +
                           static_cast<std::uint64_t>(h4) * s4;
        std::uint64_t d4 = static_cast<std::uint64_t>(h0) * r4 +
                           static_cast<std::uint64_t>(h1) * r3 +
                           static_cast<std::uint64_t>(h2) * r2 +
                           static_cast<std::uint64_t>(h3) * r1 +
                           static_cast<std::uint64_t>(h4) * r0;

        std::uint64_t carry = d0 >> 26;
        h0 = static_cast<std::uint32_t>(d0 & 0x3ffffffu);
        d1 += carry;
        carry = d1 >> 26;
        h1 = static_cast<std::uint32_t>(d1 & 0x3ffffffu);
        d2 += carry;
        carry = d2 >> 26;
        h2 = static_cast<std::uint32_t>(d2 & 0x3ffffffu);
        d3 += carry;
        carry = d3 >> 26;
        h3 = static_cast<std::uint32_t>(d3 & 0x3ffffffu);
        d4 += carry;
        carry = d4 >> 26;
        h4 = static_cast<std::uint32_t>(d4 & 0x3ffffffu);
        h0 += static_cast<std::uint32_t>(carry) * 5;
        carry = h0 >> 26;
        h0 &= 0x3ffffffu;
        h1 += static_cast<std::uint32_t>(carry);

        h_[0] = h0;
        h_[1] = h1;
        h_[2] = h2;
        h_[3] = h3;
        h_[4] = h4;
    }

    std::uint32_t r_[5]{};
    std::uint32_t h_[5]{};
    std::uint8_t s_[16]{};
    std::array<std::uint8_t, 16> buffer_{};
    std::size_t buffer_len_ = 0;
};

/// RFC 8439 §2.8 的 AEAD 构造：密文 = ChaCha20(counter=1) 异或，
/// 标签 = Poly1305(one-time key, aad || pad || ct || pad || len(aad) ||
/// len(ct))。
Bytes chacha20_poly1305_encrypt_bytes(const std::uint8_t key[32],
                                      const std::uint8_t nonce[12],
                                      const std::uint8_t* aad,
                                      std::size_t aad_size,
                                      const std::uint8_t* plaintext,
                                      std::size_t size) {
    std::uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);
    Poly1305 mac(block0);  // 一次性密钥 = block0 前 32 字节
    secure_wipe(block0, sizeof(block0));

    Bytes output(size + 16);
    chacha20_xor(key, 1, nonce, plaintext, size, output.data());

    if (aad_size > 0) {
        mac.update(aad, aad_size);
        mac.pad16();
    }
    mac.update(output.data(), size);
    mac.pad16();
    std::uint8_t lengths[16];
    store_le64(lengths, static_cast<std::uint64_t>(aad_size));
    store_le64(lengths + 8, static_cast<std::uint64_t>(size));
    mac.update(lengths, sizeof(lengths));
    mac.finish(output.data() + size);
    return output;
}

/// 解密并校验标签：先校验 tag，通过后才解密（避免把未认证明文交出去）。
bool chacha20_poly1305_decrypt_bytes(const std::uint8_t key[32],
                                     const std::uint8_t nonce[12],
                                     const std::uint8_t* aad,
                                     std::size_t aad_size,
                                     const std::uint8_t* ciphertext_and_tag,
                                     std::size_t size, Bytes& plaintext) {
    if (size < 16) return false;
    const std::size_t ciphertext_size = size - 16;

    std::uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);
    Poly1305 mac(block0);
    secure_wipe(block0, sizeof(block0));

    if (aad_size > 0) {
        mac.update(aad, aad_size);
        mac.pad16();
    }
    mac.update(ciphertext_and_tag, ciphertext_size);
    mac.pad16();
    std::uint8_t lengths[16];
    store_le64(lengths, static_cast<std::uint64_t>(aad_size));
    store_le64(lengths + 8, static_cast<std::uint64_t>(ciphertext_size));
    mac.update(lengths, sizeof(lengths));

    std::uint8_t expected_tag[16];
    mac.finish(expected_tag);
    const std::uint8_t* actual_tag = ciphertext_and_tag + ciphertext_size;
    if (!constant_time_equal(expected_tag, actual_tag, 16)) {
        secure_wipe(expected_tag, sizeof(expected_tag));
        return false;
    }
    secure_wipe(expected_tag, sizeof(expected_tag));

    plaintext.resize(ciphertext_size);
    chacha20_xor(key, 1, nonce, ciphertext_and_tag, ciphertext_size,
                 plaintext.data());
    return true;
}

// ===========================================================================
// RC4（教学用流密码）
// ===========================================================================

Bytes rc4_bytes(const Bytes& key, const Bytes& data) {
    if (key.empty()) return {};
    std::array<std::uint8_t, 256> s{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<std::uint8_t>(i);
    }
    std::uint8_t j = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        j = static_cast<std::uint8_t>(j + s[i] + key[i % key.size()]);
        std::swap(s[i], s[j]);
    }

    Bytes output(data.size());
    std::uint8_t i = 0;
    j = 0;
    for (std::size_t n = 0; n < data.size(); ++n) {
        i = static_cast<std::uint8_t>(i + 1);
        j = static_cast<std::uint8_t>(j + s[i]);
        std::swap(s[i], s[j]);
        const std::uint8_t key_byte = s[static_cast<std::uint8_t>(s[i] + s[j])];
        output[n] = static_cast<std::uint8_t>(data[n] ^ key_byte);
    }
    return output;
}

/// RC4 的 32 字节流密钥 = SHA-256(key32 || iv16)。
Bytes rc4_seed_key(const Bytes& key, const Bytes& iv) {
    Bytes material = key;
    material.insert(material.end(), iv.begin(), iv.end());
    return sha256_bytes(material);
}

// ===========================================================================
// Base64（RFC 4648）
// ===========================================================================

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

}  // namespace

// ===========================================================================
// 内部算法入口（crypto_detail）
//
// algorithm.h 只暴露 ICipher / derive_key 等高层接口；SHA-256、HMAC-SHA256、
// 原始 RC4、带 AAD 的 ChaCha20-Poly1305 没有公开头。为了不改共享头文件
// （Lead 的作用域），这里把基础原语放在 crypto_detail 命名空间，由
// tests/test_crypto.cpp 自行声明后调用标准测试向量。生产代码同样可复用。
// ===========================================================================

namespace crypto_detail {

Bytes sha256(const Bytes& data) { return sha256_bytes(data); }

Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    return hmac_sha256_bytes(key, data);
}

Bytes pbkdf2_hmac_sha256(const std::string& password, const Bytes& salt,
                         std::uint32_t iterations, std::size_t key_size,
                         std::string& error) {
    error.clear();
    if (iterations == 0) {
        error = "PBKDF2 迭代次数必须大于 0";
        return {};
    }
    if (key_size == 0) {
        error = "PBKDF2 输出密钥长度必须大于 0";
        return {};
    }
    if (key_size > (1u << 20)) {
        error = "PBKDF2 输出密钥长度过大";
        return {};
    }
    const auto* password_bytes =
        reinterpret_cast<const std::uint8_t*>(password.data());
    return pbkdf2_hmac_sha256_bytes(password_bytes, password.size(), salt,
                                    iterations, key_size);
}

Bytes rc4(const Bytes& key, const Bytes& data) { return rc4_bytes(key, data); }

Bytes poly1305(const Bytes& key, const Bytes& message) {
    if (key.size() != 32) return {};
    Poly1305 mac(key.data());
    mac.update(message.data(), message.size());
    Bytes tag(16);
    mac.finish(tag.data());
    return tag;
}

Bytes chacha20_poly1305_encrypt(const Bytes& key, const Bytes& nonce,
                                const Bytes& aad, const Bytes& plaintext,
                                std::string& error) {
    error.clear();
    if (key.size() != 32) {
        error = "ChaCha20-Poly1305 需要 32 字节密钥";
        return {};
    }
    if (nonce.size() != 12) {
        error = "ChaCha20-Poly1305 需要 12 字节 nonce";
        return {};
    }
    return chacha20_poly1305_encrypt_bytes(key.data(), nonce.data(), aad.data(),
                                           aad.size(), plaintext.data(),
                                           plaintext.size());
}

}  // namespace crypto_detail

// ===========================================================================
// 加密算法注册表
// ===========================================================================

const char* to_string(EncryptionId id) {
    switch (id) {
        case EncryptionId::None:
            return "none";
        case EncryptionId::Aes256Cbc:
            return "aes256cbc";
        case EncryptionId::ChaCha20Poly1305:
            return "chacha20poly1305";
        case EncryptionId::Rc4:
            return "rc4";
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
        {"aes256cbc", "AES-256-CBC", false, true,
         "自研实现，PKCS#7 填充，PBKDF2-SHA256 派生密钥"},
        {"chacha20poly1305", "ChaCha20-Poly1305", false, true,
         "自研实现（RFC 8439），AEAD"},
        {"rc4", "RC4", false, true, "自研流密码（教学用，不安全）"},
    };
    return list;
}

// ===========================================================================
// ICipher 实现
// ===========================================================================

namespace {

constexpr std::size_t kAes256KeySize = 32;
constexpr std::size_t kChaCha20KeySize = 32;
constexpr std::size_t kChaCha20NonceSize = 12;
constexpr std::size_t kRc4KeySize = 32;
constexpr std::size_t kRc4IvSize = 16;
constexpr std::size_t kPoly1305TagSize = 16;

class Aes256CbcCipher final : public ICipher {
   public:
    EncryptionId id() const override { return EncryptionId::Aes256Cbc; }
    std::size_t key_size() const override { return kAes256KeySize; }
    std::size_t iv_size() const override { return kAesBlockSize; }

    Bytes encrypt(const Bytes& plaintext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (key.size() != kAes256KeySize) {
            error = "AES-256-CBC 需要 32 字节密钥";
            return {};
        }
        if (iv.size() != kAesBlockSize) {
            error = "AES-256-CBC 需要 16 字节 IV";
            return {};
        }
        return aes256_cbc_encrypt_pkcs7(key, iv, plaintext);
    }

    Bytes decrypt(const Bytes& ciphertext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (key.size() != kAes256KeySize) {
            error = "AES-256-CBC 需要 32 字节密钥";
            return {};
        }
        if (iv.size() != kAesBlockSize) {
            error = "AES-256-CBC 需要 16 字节 IV";
            return {};
        }
        return aes256_cbc_decrypt_pkcs7(key, iv, ciphertext, error);
    }
};

class ChaCha20Poly1305Cipher final : public ICipher {
   public:
    EncryptionId id() const override { return EncryptionId::ChaCha20Poly1305; }
    std::size_t key_size() const override { return kChaCha20KeySize; }
    std::size_t iv_size() const override { return kChaCha20NonceSize; }

    Bytes encrypt(const Bytes& plaintext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (key.size() != kChaCha20KeySize) {
            error = "ChaCha20-Poly1305 需要 32 字节密钥";
            return {};
        }
        if (iv.size() != kChaCha20NonceSize) {
            error = "ChaCha20-Poly1305 需要 12 字节 nonce";
            return {};
        }
        // ICipher 没有 AAD 入口，生产路径使用空 AAD。
        return chacha20_poly1305_encrypt_bytes(key.data(), iv.data(), nullptr,
                                               0, plaintext.data(),
                                               plaintext.size());
    }

    Bytes decrypt(const Bytes& ciphertext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (key.size() != kChaCha20KeySize) {
            error = "ChaCha20-Poly1305 需要 32 字节密钥";
            return {};
        }
        if (iv.size() != kChaCha20NonceSize) {
            error = "ChaCha20-Poly1305 需要 12 字节 nonce";
            return {};
        }
        if (ciphertext.size() < kPoly1305TagSize) {
            error = "ChaCha20-Poly1305 密文过短（缺少 16 字节认证标签）";
            return {};
        }
        Bytes plaintext;
        if (!chacha20_poly1305_decrypt_bytes(key.data(), iv.data(), nullptr, 0,
                                             ciphertext.data(),
                                             ciphertext.size(), plaintext)) {
            error = "ChaCha20-Poly1305 认证失败：密钥错误或数据被篡改";
            return {};
        }
        return plaintext;
    }
};

class Rc4Cipher final : public ICipher {
   public:
    EncryptionId id() const override { return EncryptionId::Rc4; }
    std::size_t key_size() const override { return kRc4KeySize; }
    std::size_t iv_size() const override { return kRc4IvSize; }

    /// RC4 对称：加密 = 解密 = 密钥流异或。无认证，无法检测错误密钥/篡改；
    /// 仅用于教学和格式兼容（界面上标注“不安全”）。
    Bytes encrypt(const Bytes& plaintext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (!check_sizes(key, iv, error)) return {};
        return rc4_bytes(rc4_seed_key(key, iv), plaintext);
    }

    Bytes decrypt(const Bytes& ciphertext, const Bytes& key, const Bytes& iv,
                  std::string& error) override {
        error.clear();
        if (!check_sizes(key, iv, error)) return {};
        return rc4_bytes(rc4_seed_key(key, iv), ciphertext);
    }

   private:
    static bool check_sizes(const Bytes& key, const Bytes& iv,
                            std::string& error) {
        if (key.size() != kRc4KeySize) {
            error = "RC4 需要 32 字节密钥";
            return false;
        }
        if (iv.size() != kRc4IvSize) {
            error = "RC4 需要 16 字节 IV";
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<ICipher> make_cipher(EncryptionId id) {
    switch (id) {
        case EncryptionId::None:
            return nullptr;
        case EncryptionId::Aes256Cbc:
            return std::make_unique<Aes256CbcCipher>();
        case EncryptionId::ChaCha20Poly1305:
            return std::make_unique<ChaCha20Poly1305Cipher>();
        case EncryptionId::Rc4:
            return std::make_unique<Rc4Cipher>();
    }
    return nullptr;
}

// ===========================================================================
// 密钥派生 / 随机数 / 校验 / 编码
// ===========================================================================

Bytes derive_key(const std::string& password, const Bytes& salt,
                 std::uint32_t iterations, std::size_t key_size,
                 std::string& error) {
    return crypto_detail::pbkdf2_hmac_sha256(password, salt, iterations,
                                             key_size, error);
}

Bytes random_bytes(std::size_t count) {
    if (count == 0) return {};
    Bytes output(count);
    std::size_t filled = 0;
    while (filled < count) {
        // BCryptGenRandom 一次最多 ULONG 字节，分块处理以防极端大请求。
        const std::size_t chunk =
            std::min<std::size_t>(count - filled, 1u << 20);
        const NTSTATUS status = BCryptGenRandom(
            nullptr, output.data() + filled, static_cast<ULONG>(chunk),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            secure_wipe(output.data(), output.size());
            return {};
        }
        filled += chunk;
    }
    return output;
}

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

std::string hex_encode(const Bytes& data) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t byte : data) {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return out;
}

std::string base64_encode(const Bytes& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t group =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
            static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(group >> 18) & 0x3Fu]);
        out.push_back(kBase64Alphabet[(group >> 12) & 0x3Fu]);
        out.push_back(kBase64Alphabet[(group >> 6) & 0x3Fu]);
        out.push_back(kBase64Alphabet[group & 0x3Fu]);
    }
    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(group >> 18) & 0x3Fu]);
        out.push_back(kBase64Alphabet[(group >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t group =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(group >> 18) & 0x3Fu]);
        out.push_back(kBase64Alphabet[(group >> 12) & 0x3Fu]);
        out.push_back(kBase64Alphabet[(group >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

Bytes base64_decode(const std::string& text, std::string& error) {
    error.clear();
    if (text.empty()) return {};
    if (text.size() % 4 != 0) {
        error = "Base64 长度必须是 4 的倍数";
        return {};
    }

    Bytes out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool is_last_group = (i + 4 == text.size());
        int values[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int k = 0; k < 4; ++k) {
            const char character = text[i + static_cast<std::size_t>(k)];
            if (character == '=') {
                if (!is_last_group || k < 2) {
                    error = "Base64 填充符位置非法";
                    return {};
                }
                ++padding;
                continue;
            }
            if (padding != 0) {
                error = "Base64 填充符后不允许再出现数据";
                return {};
            }
            values[k] = base64_value(character);
            if (values[k] < 0) {
                error = "Base64 含非法字符";
                return {};
            }
        }

        const std::uint32_t group =
            (static_cast<std::uint32_t>(values[0]) << 18) |
            (static_cast<std::uint32_t>(values[1]) << 12) |
            (static_cast<std::uint32_t>(values[2]) << 6) |
            static_cast<std::uint32_t>(values[3]);
        out.push_back(static_cast<std::uint8_t>(group >> 16));
        if (padding < 2) out.push_back(static_cast<std::uint8_t>(group >> 8));
        if (padding < 1) out.push_back(static_cast<std::uint8_t>(group));
        // 规范编码要求被填充掉的低位为 0；拒绝非规范输入。
        if (padding == 1 && (values[2] & 0x03) != 0) {
            error = "Base64 尾部填充位非零";
            return {};
        }
        if (padding == 2 && (values[1] & 0x0F) != 0) {
            error = "Base64 尾部填充位非零";
            return {};
        }
    }
    return out;
}

}  // namespace backup
