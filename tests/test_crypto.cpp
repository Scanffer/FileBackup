#include <catch.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "algorithm.h"

using backup::Bytes;
using backup::EncryptionId;
using backup::ICipher;

// ---------------------------------------------------------------------------
// core/crypto.cpp 内部算法入口。
//
// algorithm.h 只暴露 ICipher / derive_key / random_bytes / 编码工具等高层接口，
// SHA-256、HMAC-SHA256、原始 RC4、Poly1305、带 AAD 的 ChaCha20-Poly1305 没有
// 公开声明。为了不改共享头文件（属于 Lead 的作用域），这些算法入口定义在
// crypto.cpp 的 backup::crypto_detail 命名空间中，这里手工声明后直接调用，
// 用标准测试向量验证底层原语。生产代码不需要这些声明。
// ---------------------------------------------------------------------------
namespace backup {
namespace crypto_detail {
Bytes sha256(const Bytes& data);
Bytes hmac_sha256(const Bytes& key, const Bytes& data);
Bytes rc4(const Bytes& key, const Bytes& data);
Bytes poly1305(const Bytes& key, const Bytes& message);
Bytes chacha20_poly1305_encrypt(const Bytes& key, const Bytes& nonce,
                                const Bytes& aad, const Bytes& plaintext,
                                std::string& error);
}  // namespace crypto_detail
}  // namespace backup

namespace {

Bytes bytes_of(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

Bytes from_hex(const std::string& hex) {
    REQUIRE(hex.size() % 2 == 0);
    Bytes out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        REQUIRE(high >= 0);
        REQUIRE(low >= 0);
        out.push_back(static_cast<std::uint8_t>(high * 16 + low));
    }
    return out;
}

Bytes make_pattern(std::size_t size) {
    Bytes data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 73 + 41) % 251);
    }
    return data;
}

Bytes repeated(std::uint8_t value, std::size_t count) {
    return Bytes(count, value);
}

/// 逐字节异或两个等长数组，用于构造错误密钥 / 篡改输入。
Bytes with_flipped_byte(Bytes data, std::size_t index, std::uint8_t mask) {
    REQUIRE(index < data.size());
    data[index] ^= mask;
    return data;
}

// ---- 测试向量常量 ---------------------------------------------------------

const char* const kSha256Abc =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char* const kSha256Empty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char* const kSha2562Block =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
const char* const kSha256MillionA =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

// NIST SP 800-38A F.2.5（AES-256-CBC）
const char* const kAesKeyHex =
    "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4";
const char* const kAesIvHex = "000102030405060708090a0b0c0d0e0f";
const char* const kAesPlainHex =
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710";
const char* const kAesNistCipherHex =
    "f58c4c04d6e5f1ba779eabfb5f7bfbd6"
    "9cfc4e967edb808d679f777bc6702c7d"
    "39f23369a9d9bacfa530e26304231461"
    "b2eb05e2c39be9fcda6c19078c6a9d1b";
// 完整 5 块：前 4 块是 NIST 密文，第 5 块是 PKCS#7 填充块。
// （Node.js 24 / OpenSSL 交叉验证：aes-256-cbc(key, iv) 对同一明文的输出）
const char* const kAesPkcs7CipherHex =
    "f58c4c04d6e5f1ba779eabfb5f7bfbd6"
    "9cfc4e967edb808d679f777bc6702c7d"
    "39f23369a9d9bacfa530e26304231461"
    "b2eb05e2c39be9fcda6c19078c6a9d1b"
    "3f461796d6b0d6b2e0c2a72b4d80e644";

// ChaCha20-Poly1305（RFC 8439 §2.8.2 完整向量，含 AAD）
const char* const kChaChaRfcKeyHex =
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
const char* const kChaChaRfcNonceHex = "070000004041424344454647";
const char* const kChaChaRfcAadHex = "50515253c0c1c2c3c4c5c6c7";
const char* const kChaChaRfcCipherHex =
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b6116";
const char* const kChaChaRfcTagHex = "1ae10b594f09e26a7e902ecbd0600691";

// 空 AAD 固定向量（ICipher 的用法）：Node.js 24 / OpenSSL 交叉验证。
const char* const kChaChaEmptyAadPlain = "FileBackup ChaCha20-Poly1305 self test vector.";
const char* const kChaChaEmptyAadCipherTagHex =
    "d9128538439c23d16092afb85ee049c6a0f2b812596e02a7ecb9e0b275f166c9"
    "28bebf5ddffa7e5dddb9c66088f5fe469d764feb470fe26c8b8973291d8e";
const char* const kChaChaEmptyPlainTagHex =
    "a0784d7a4716f3feb4f64e7f4b39bf04";

// RC4：key32/iv16，内部种子 = SHA-256(key || iv)（Node.js 交叉验证）。
const char* const kRc4Key32Hex =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const char* const kRc4Iv16Hex = "101112131415161718191a1b1c1d1e1f";
const char* const kRc4IcipherPlain = "hello self made rc4";
const char* const kRc4IcipherCipherHex = "c55767ef223e912736d780c8da9a6aad5df93e";

}  // namespace

// ===========================================================================
// SHA-256
// ===========================================================================

TEST_CASE("sha256: FIPS 180-4 standard vectors", "[crypto][sha256]") {
    REQUIRE(backup::hex_encode(backup::crypto_detail::sha256(bytes_of("abc"))) ==
            kSha256Abc);
    REQUIRE(backup::hex_encode(backup::crypto_detail::sha256(Bytes{})) ==
            kSha256Empty);
    REQUIRE(backup::hex_encode(backup::crypto_detail::sha256(bytes_of(
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
            kSha2562Block);
}

TEST_CASE("sha256: one million 'a'", "[crypto][sha256]") {
    const std::string million(1000000, 'a');
    REQUIRE(backup::hex_encode(backup::crypto_detail::sha256(
                bytes_of(million))) == kSha256MillionA);
}

TEST_CASE("sha256: padding boundary lengths", "[crypto][sha256]") {
    // 期望值由 Node.js 24（OpenSSL）独立实现生成，覆盖 0x80 + 长度字段的
    // 所有临界位置：55/56、63/64、119/120（两块的同样边界）。
    struct Case {
        std::size_t length;
        const char* digest;
    };
    const Case cases[] = {
        {1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (const Case& test : cases) {
        const Bytes input = repeated('a', test.length);
        REQUIRE(backup::hex_encode(backup::crypto_detail::sha256(input)) ==
                test.digest);
    }
}

// ===========================================================================
// HMAC-SHA256
// ===========================================================================

TEST_CASE("hmac-sha256: RFC 4231 vectors", "[crypto][hmac]") {
    // case 1
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                repeated(0x0b, 20), bytes_of("Hi There"))) ==
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // case 2
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                bytes_of("Jefe"), bytes_of("what do ya want for nothing?"))) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // case 3
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                repeated(0xaa, 20), repeated(0xdd, 50))) ==
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    // case 4：key 为 25 字节
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                from_hex("0102030405060708090a0b0c0d0e0f10111213141516171819"),
                repeated(0xcd, 50))) ==
            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    // case 6：key 长度 131 > 块长 64，必须先哈希
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                repeated(0xaa, 131),
                bytes_of("Test Using Larger Than Block-Size Key - Hash Key "
                         "First"))) ==
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    // case 7：超长 key + 超长数据
    REQUIRE(backup::hex_encode(backup::crypto_detail::hmac_sha256(
                repeated(0xaa, 131),
                bytes_of(
                    "This is a test using a larger than block-size key and a "
                    "larger than block-size data. The key needs to be hashed "
                    "before being used by the HMAC algorithm."))) ==
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST_CASE("hmac-sha256: empty key and empty data", "[crypto][hmac]") {
    const Bytes mac = backup::crypto_detail::hmac_sha256(Bytes{}, Bytes{});
    REQUIRE(mac.size() == 32);
    REQUIRE(mac == from_hex(
                       "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"));
}

// ===========================================================================
// PBKDF2-HMAC-SHA256
// ===========================================================================

TEST_CASE("pbkdf2-hmac-sha256: known vectors", "[crypto][pbkdf2]") {
    std::string error;
    const Bytes salt = bytes_of("salt");

    const Bytes one_iteration = backup::derive_key("password", salt, 1, 32, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(one_iteration) ==
            "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    const Bytes four_k = backup::derive_key("password", salt, 4096, 32, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(four_k) ==
            "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    // dkLen = 40：跨两个 SHA-256 输出块（block index 1 与 2）。
    const Bytes forty = backup::derive_key("password", salt, 4096, 40, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(forty) ==
            "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"
            "f7ad98c1b458ce3f");

    // dkLen = 16：截断输出。
    const Bytes sixteen = backup::derive_key("password", salt, 1, 16, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(sixteen) == "120fb6cffcf8b32c43e7225256c4f837");

    // 自定义口令/盐/迭代次数（Node.js 24 / OpenSSL 交叉验证）。
    const Bytes custom_salt =
        from_hex("00112233445566778899aabbccddeeff");
    const Bytes custom = backup::derive_key("FileBackup-Password-2026",
                                            custom_salt, 2026, 48, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(custom) ==
            "468b122b576ea50fa9cab9605b243145cb8a75a91a7c9dc8f745dce6f4064343"
            "edc60088a1da5a785c163b27667feb8d");
}

TEST_CASE("pbkdf2-hmac-sha256: deterministic and salt-sensitive",
          "[crypto][pbkdf2]") {
    std::string error;
    const Bytes salt = from_hex("a1b2c3d4");
    const Bytes first = backup::derive_key("secret", salt, 100, 32, error);
    REQUIRE(error.empty());
    const Bytes second = backup::derive_key("secret", salt, 100, 32, error);
    REQUIRE(error.empty());
    REQUIRE(first == second);

    const Bytes other_salt = from_hex("a1b2c3d5");
    const Bytes third = backup::derive_key("secret", other_salt, 100, 32, error);
    REQUIRE(error.empty());
    REQUIRE(third != first);
}

TEST_CASE("pbkdf2-hmac-sha256: rejects invalid parameters", "[crypto][pbkdf2]") {
    std::string error;
    {
        const Bytes out = backup::derive_key("password", bytes_of("salt"), 0, 32,
                                             error);
        REQUIRE(!error.empty());
        REQUIRE(out.empty());
    }
    {
        const Bytes out = backup::derive_key("password", bytes_of("salt"), 1000,
                                             0, error);
        REQUIRE(!error.empty());
        REQUIRE(out.empty());
    }
}

// ===========================================================================
// AES-256-CBC
// ===========================================================================

TEST_CASE("aes-256-cbc: NIST SP 800-38A F.2.5 vector", "[crypto][aes]") {
    const auto cipher = backup::make_cipher(EncryptionId::Aes256Cbc);
    REQUIRE(cipher != nullptr);
    REQUIRE(cipher->key_size() == 32);
    REQUIRE(cipher->iv_size() == 16);

    const Bytes key = from_hex(kAesKeyHex);
    const Bytes iv = from_hex(kAesIvHex);
    const Bytes plaintext = from_hex(kAesPlainHex);
    REQUIRE(plaintext.size() == 64);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
    REQUIRE(error.empty());
    REQUIRE(ciphertext.size() == 80);  // 64 + PKCS#7 整块填充
    // 前 64 字节必须与 NIST 标准密文一致。
    REQUIRE(backup::hex_encode(Bytes(ciphertext.begin(),
                                     ciphertext.begin() + 64)) ==
            kAesNistCipherHex);
    // 完整 80 字节（含填充块）与 OpenSSL 交叉验证结果一致。
    REQUIRE(backup::hex_encode(ciphertext) == kAesPkcs7CipherHex);

    const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
    REQUIRE(error.empty());
    REQUIRE(restored == plaintext);
}

TEST_CASE("aes-256-cbc: empty plaintext and padding block", "[crypto][aes]") {
    const auto cipher = backup::make_cipher(EncryptionId::Aes256Cbc);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kAesKeyHex);
    const Bytes iv = from_hex(kAesIvHex);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(Bytes{}, key, iv, error);
    REQUIRE(error.empty());
    REQUIRE(ciphertext.size() == 16);
    // 空明文 → 16 字节的 0x10 填充块（OpenSSL 交叉验证）。
    REQUIRE(backup::hex_encode(ciphertext) ==
            "7e9248e5d829ca7593f0c549db2f5b8c");

    const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
    REQUIRE(error.empty());
    REQUIRE(restored.empty());
}

TEST_CASE("aes-256-cbc: roundtrip across sizes", "[crypto][aes]") {
    const auto cipher = backup::make_cipher(EncryptionId::Aes256Cbc);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kAesKeyHex);
    const Bytes iv = from_hex(kAesIvHex);

    const std::size_t sizes[] = {0,  1,  2,  15, 16,  17,  31,  32,
                                 33, 63, 64, 65, 255, 256, 1000, 4096};
    for (const std::size_t size : sizes) {
        const Bytes plaintext = make_pattern(size);
        std::string error;
        const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
        REQUIRE(error.empty());
        REQUIRE(ciphertext.size() == (size / 16 + 1) * 16);
        const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
        REQUIRE(error.empty());
        REQUIRE(restored == plaintext);
    }
}

TEST_CASE("aes-256-cbc: rejects wrong key, corrupted padding and bad sizes",
          "[crypto][aes]") {
    const auto cipher = backup::make_cipher(EncryptionId::Aes256Cbc);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kAesKeyHex);
    const Bytes iv = from_hex(kAesIvHex);
    const Bytes plaintext = from_hex(kAesPlainHex);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
    REQUIRE(error.empty());

    // 错误密钥：解密结果几乎不可能通过 PKCS#7 校验，必须报错。
    const Bytes wrong_key = with_flipped_byte(key, 0, 0x01);
    const Bytes bad = cipher->decrypt(ciphertext, wrong_key, iv, error);
    REQUIRE(!error.empty());
    REQUIRE(bad.empty());

    // 篡改填充：最后一个密文块的前一块（C_{n-1}）最后一字节异或 0x01，
    // 会把明文末尾 0x10 填充改成 0x11，填充必然非法。
    const Bytes tampered_padding = with_flipped_byte(ciphertext, 63, 0x01);
    REQUIRE(cipher->decrypt(tampered_padding, key, iv, error).empty());
    REQUIRE(!error.empty());

    // 非 16 字节倍数 / 空密文 / 截断。
    {
        Bytes truncated(ciphertext.begin(), ciphertext.end() - 1);
        REQUIRE(cipher->decrypt(truncated, key, iv, error).empty());
        REQUIRE(!error.empty());
    }
    {
        REQUIRE(cipher->decrypt(Bytes{}, key, iv, error).empty());
        REQUIRE(!error.empty());
    }
    {
        REQUIRE(cipher->decrypt(from_hex("010203"), key, iv, error).empty());
        REQUIRE(!error.empty());
    }

    // 密钥 / IV 长度非法。
    {
        REQUIRE(cipher->encrypt(plaintext, Bytes(31, 0x11), iv, error).empty());
        REQUIRE(!error.empty());
        REQUIRE(cipher->decrypt(ciphertext, Bytes(31, 0x11), iv, error).empty());
        REQUIRE(!error.empty());
    }
    {
        REQUIRE(cipher->encrypt(plaintext, key, Bytes(15, 0x11), error).empty());
        REQUIRE(!error.empty());
        REQUIRE(cipher->decrypt(ciphertext, key, Bytes(15, 0x11), error).empty());
        REQUIRE(!error.empty());
    }
}

// ===========================================================================
// ChaCha20-Poly1305
// ===========================================================================

TEST_CASE("poly1305: RFC 8439 section 2.5.2 vector", "[crypto][chacha]") {
    const Bytes key = from_hex(
        "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    const Bytes message = bytes_of("Cryptographic Forum Research Group");
    const Bytes tag = backup::crypto_detail::poly1305(key, message);
    REQUIRE(backup::hex_encode(tag) == "a8061dc1305136c6c22b8baf0c0127a9");
}

TEST_CASE("chacha20-poly1305: RFC 8439 section 2.8.2 AEAD vector with AAD",
          "[crypto][chacha]") {
    const Bytes key = from_hex(kChaChaRfcKeyHex);
    const Bytes nonce = from_hex(kChaChaRfcNonceHex);
    const Bytes aad = from_hex(kChaChaRfcAadHex);
    const Bytes plaintext = bytes_of(
        "Ladies and Gentlemen of the class of '99: If I could offer you only "
        "one tip for the future, sunscreen would be it.");
    REQUIRE(plaintext.size() == 114);

    std::string error;
    const Bytes ciphertext = backup::crypto_detail::chacha20_poly1305_encrypt(
        key, nonce, aad, plaintext, error);
    REQUIRE(error.empty());
    REQUIRE(ciphertext.size() == 114 + 16);
    REQUIRE(backup::hex_encode(ciphertext) ==
            std::string(kChaChaRfcCipherHex) + kChaChaRfcTagHex);
}

TEST_CASE("chacha20-poly1305: ICipher fixed vector with empty AAD",
          "[crypto][chacha]") {
    const auto cipher = backup::make_cipher(EncryptionId::ChaCha20Poly1305);
    REQUIRE(cipher != nullptr);
    REQUIRE(cipher->key_size() == 32);
    REQUIRE(cipher->iv_size() == 12);

    const Bytes key = from_hex(kChaChaRfcKeyHex);
    const Bytes nonce = from_hex(kChaChaRfcNonceHex);
    const Bytes plaintext = bytes_of(kChaChaEmptyAadPlain);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(plaintext, key, nonce, error);
    REQUIRE(error.empty());
    // 期望值来自 Node.js 24 / OpenSSL（chaCha20-poly1305, 空 AAD）。
    REQUIRE(backup::hex_encode(ciphertext) == kChaChaEmptyAadCipherTagHex);

    const Bytes restored = cipher->decrypt(ciphertext, key, nonce, error);
    REQUIRE(error.empty());
    REQUIRE(restored == plaintext);

    // 空明文：密文只剩 16 字节标签。
    const Bytes empty_tag = cipher->encrypt(Bytes{}, key, nonce, error);
    REQUIRE(error.empty());
    REQUIRE(backup::hex_encode(empty_tag) == kChaChaEmptyPlainTagHex);
    const Bytes empty_restored = cipher->decrypt(empty_tag, key, nonce, error);
    REQUIRE(error.empty());
    REQUIRE(empty_restored.empty());
}

TEST_CASE("chacha20-poly1305: roundtrip across sizes", "[crypto][chacha]") {
    const auto cipher = backup::make_cipher(EncryptionId::ChaCha20Poly1305);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kChaChaRfcKeyHex);
    const Bytes nonce = from_hex(kChaChaRfcNonceHex);

    const std::size_t sizes[] = {0,  1,  2,  15, 16,  17,  63,  64,
                                 65, 114, 255, 256, 1000, 4096};
    for (const std::size_t size : sizes) {
        const Bytes plaintext = make_pattern(size);
        std::string error;
        const Bytes ciphertext = cipher->encrypt(plaintext, key, nonce, error);
        REQUIRE(error.empty());
        REQUIRE(ciphertext.size() == size + 16);
        const Bytes restored = cipher->decrypt(ciphertext, key, nonce, error);
        REQUIRE(error.empty());
        REQUIRE(restored == plaintext);
    }
}

TEST_CASE("chacha20-poly1305: rejects tampered tag, tampered ciphertext, "
          "wrong key and bad sizes",
          "[crypto][chacha]") {
    const auto cipher = backup::make_cipher(EncryptionId::ChaCha20Poly1305);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kChaChaRfcKeyHex);
    const Bytes nonce = from_hex(kChaChaRfcNonceHex);
    const Bytes plaintext = bytes_of(kChaChaEmptyAadPlain);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(plaintext, key, nonce, error);
    REQUIRE(error.empty());

    // 篡改 tag（最后一个字节）：必须在解密前被拒绝。
    const Bytes tampered_tag =
        with_flipped_byte(ciphertext, ciphertext.size() - 1, 0x01);
    REQUIRE(cipher->decrypt(tampered_tag, key, nonce, error).empty());
    REQUIRE(!error.empty());

    // 篡改密文（中间字节）：tag 校验失败。
    const Bytes tampered_ct = with_flipped_byte(ciphertext, 0, 0x80);
    REQUIRE(cipher->decrypt(tampered_ct, key, nonce, error).empty());
    REQUIRE(!error.empty());

    // 错误密钥。
    const Bytes wrong_key = with_flipped_byte(key, 31, 0x40);
    REQUIRE(cipher->decrypt(ciphertext, wrong_key, nonce, error).empty());
    REQUIRE(!error.empty());

    // 错误 nonce。
    const Bytes wrong_nonce = with_flipped_byte(nonce, 0, 0x01);
    REQUIRE(cipher->decrypt(ciphertext, key, wrong_nonce, error).empty());
    REQUIRE(!error.empty());

    // 截断：不足 16 字节 / 去掉 tag 的一个字节。
    REQUIRE(cipher->decrypt(Bytes{}, key, nonce, error).empty());
    REQUIRE(!error.empty());
    REQUIRE(cipher->decrypt(repeated(0x00, 15), key, nonce, error).empty());
    REQUIRE(!error.empty());
    {
        const Bytes truncated(ciphertext.begin(), ciphertext.end() - 1);
        REQUIRE(cipher->decrypt(truncated, key, nonce, error).empty());
        REQUIRE(!error.empty());
    }

    // 密钥 / nonce 长度非法。
    REQUIRE(cipher->encrypt(plaintext, Bytes(31, 0x11), nonce, error).empty());
    REQUIRE(!error.empty());
    REQUIRE(cipher->encrypt(plaintext, key, Bytes(11, 0x11), error).empty());
    REQUIRE(!error.empty());
    REQUIRE(cipher->decrypt(ciphertext, Bytes(31, 0x11), nonce, error).empty());
    REQUIRE(!error.empty());
    REQUIRE(cipher->decrypt(ciphertext, key, Bytes(11, 0x11), error).empty());
    REQUIRE(!error.empty());
}

// ===========================================================================
// RC4
// ===========================================================================

TEST_CASE("rc4: classic known vectors", "[crypto][rc4]") {
    REQUIRE(backup::hex_encode(backup::crypto_detail::rc4(
                bytes_of("Key"), bytes_of("Plaintext"))) ==
            "bbf316e8d940af0ad3");
    REQUIRE(backup::hex_encode(backup::crypto_detail::rc4(
                bytes_of("Wiki"), bytes_of("pedia"))) == "1021bf0420");
    REQUIRE(backup::hex_encode(backup::crypto_detail::rc4(
                bytes_of("Secret"), bytes_of("Attack at dawn"))) ==
            "45a01f645fc35b383552544b9bf5");
}

TEST_CASE("rc4: ICipher uses SHA-256(key || iv) as stream seed",
          "[crypto][rc4]") {
    const auto cipher = backup::make_cipher(EncryptionId::Rc4);
    REQUIRE(cipher != nullptr);
    REQUIRE(cipher->key_size() == 32);
    REQUIRE(cipher->iv_size() == 16);

    const Bytes key = from_hex(kRc4Key32Hex);
    const Bytes iv = from_hex(kRc4Iv16Hex);
    const Bytes plaintext = bytes_of(kRc4IcipherPlain);

    std::string error;
    const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
    REQUIRE(error.empty());
    // 期望值：RC4(SHA-256(key||iv), plaintext)，由 Node.js 独立实现核对。
    REQUIRE(backup::hex_encode(ciphertext) == kRc4IcipherCipherHex);

    const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
    REQUIRE(error.empty());
    REQUIRE(restored == plaintext);

    // 错误 IV 得到不同密钥流（RC4 无认证，不报错但结果不同）。
    const Bytes other_iv = with_flipped_byte(iv, 0, 0x01);
    const Bytes other = cipher->decrypt(ciphertext, key, other_iv, error);
    REQUIRE(error.empty());
    REQUIRE(other != plaintext);
}

TEST_CASE("rc4: roundtrip and empty input", "[crypto][rc4]") {
    const auto cipher = backup::make_cipher(EncryptionId::Rc4);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kRc4Key32Hex);
    const Bytes iv = from_hex(kRc4Iv16Hex);

    std::string error;
    REQUIRE(cipher->encrypt(Bytes{}, key, iv, error).empty());
    REQUIRE(error.empty());
    REQUIRE(cipher->decrypt(Bytes{}, key, iv, error).empty());
    REQUIRE(error.empty());

    const std::size_t sizes[] = {1, 2, 15, 16, 17, 255, 256, 1000, 4096};
    for (const std::size_t size : sizes) {
        const Bytes plaintext = make_pattern(size);
        const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
        REQUIRE(error.empty());
        REQUIRE(ciphertext.size() == size);
        REQUIRE(ciphertext != plaintext);  // 非空输入不应原样输出
        const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
        REQUIRE(error.empty());
        REQUIRE(restored == plaintext);
    }
}

TEST_CASE("rc4: rejects wrong key/iv sizes", "[crypto][rc4]") {
    const auto cipher = backup::make_cipher(EncryptionId::Rc4);
    REQUIRE(cipher != nullptr);
    const Bytes key = from_hex(kRc4Key32Hex);
    const Bytes iv = from_hex(kRc4Iv16Hex);
    std::string error;
    REQUIRE(cipher->encrypt(bytes_of("data"), Bytes(16, 0x00), iv, error)
                .empty());
    REQUIRE(!error.empty());
    REQUIRE(cipher->encrypt(bytes_of("data"), key, Bytes(8, 0x00), error)
                .empty());
    REQUIRE(!error.empty());
}

// ===========================================================================
// Base64 / hex
// ===========================================================================

TEST_CASE("base64: RFC 4648 test vectors", "[crypto][base64]") {
    const std::pair<const char*, const char*> cases[] = {
        {"", ""},         {"f", "Zg=="},     {"fo", "Zm8="},
        {"foo", "Zm9v"},  {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    std::string error;
    for (const auto& test : cases) {
        const Bytes input = bytes_of(test.first);
        REQUIRE(backup::base64_encode(input) == test.second);
        const Bytes decoded = backup::base64_decode(test.second, error);
        REQUIRE(error.empty());
        REQUIRE(decoded == input);
    }
}

TEST_CASE("base64: roundtrip arbitrary bytes", "[crypto][base64]") {
    std::string error;
    for (std::size_t size = 0; size <= 130; ++size) {
        const Bytes input = make_pattern(size);
        const std::string encoded = backup::base64_encode(input);
        REQUIRE(encoded.size() == (size + 2) / 3 * 4);
        const Bytes decoded = backup::base64_decode(encoded, error);
        REQUIRE(error.empty());
        REQUIRE(decoded == input);
    }

    // 覆盖全部 256 种字节值。
    Bytes all(256);
    for (std::size_t i = 0; i < 256; ++i) {
        all[i] = static_cast<std::uint8_t>(i);
    }
    const Bytes decoded = backup::base64_decode(backup::base64_encode(all),
                                                error);
    REQUIRE(error.empty());
    REQUIRE(decoded == all);
}

TEST_CASE("base64: rejects malformed input", "[crypto][base64]") {
    std::string error;
    const char* const bad_inputs[] = {
        "A",        // 长度不是 4 的倍数
        "AAAAA",    // 长度不是 4 的倍数
        "!!!!",     // 非法字符
        "AA=A",     // 填充符在中间
        "=AAA",     // 填充符在开头
        "AAA=AAAA", // 填充符不在最后一组
        "AB==",     // 尾部填充位非零（非规范编码）
        "AAB=",     // 尾部填充位非零
        "Zm9v ",    // 空白字符
        "Zm9v\n",   // 换行
    };
    for (const char* input : bad_inputs) {
        const Bytes decoded = backup::base64_decode(input, error);
        REQUIRE(!error.empty());
        REQUIRE(decoded.empty());
    }

    // 空串是合法的。
    REQUIRE(backup::base64_decode("", error).empty());
    REQUIRE(error.empty());
}

TEST_CASE("hex: encode and roundtrip", "[crypto][hex]") {
    REQUIRE(backup::hex_encode(Bytes{}).empty());
    REQUIRE(backup::hex_encode(from_hex("00ff10ab")) == "00ff10ab");

    for (std::size_t size = 0; size <= 64; ++size) {
        const Bytes data = make_pattern(size);
        const std::string encoded = backup::hex_encode(data);
        REQUIRE(encoded.size() == size * 2);
        REQUIRE(from_hex(encoded) == data);
    }

    const Bytes all = [] {
        Bytes bytes(256);
        for (std::size_t i = 0; i < 256; ++i) {
            bytes[i] = static_cast<std::uint8_t>(i);
        }
        return bytes;
    }();
    REQUIRE(from_hex(backup::hex_encode(all)) == all);
    REQUIRE(backup::hex_encode(all).find_first_not_of("0123456789abcdef") ==
            std::string::npos);
}

// ===========================================================================
// CRC32 / random_bytes / 注册表
// ===========================================================================

TEST_CASE("crc32: known vectors", "[crypto][crc32]") {
    REQUIRE(backup::crc32(Bytes{}) == 0u);
    REQUIRE(backup::crc32(bytes_of("123456789")) == 0xCBF43926u);
    REQUIRE(backup::crc32(bytes_of("hello")) == 0x3610A686u);
    REQUIRE(backup::crc32(repeated(0x00, 1)) == 0xD202EF8Du);

    const Bytes data = make_pattern(1234);
    REQUIRE(backup::crc32(data) ==
            backup::crc32(data.data(), data.size()));
}

TEST_CASE("random_bytes: sizes and randomness", "[crypto][random]") {
    REQUIRE(backup::random_bytes(0).empty());
    REQUIRE(backup::random_bytes(1).size() == 1);
    REQUIRE(backup::random_bytes(32).size() == 32);
    REQUIRE(backup::random_bytes(1000).size() == 1000);

    const Bytes first = backup::random_bytes(32);
    const Bytes second = backup::random_bytes(32);
    REQUIRE(first.size() == 32);
    REQUIRE(second.size() == 32);
    REQUIRE(first != second);  // 概率 2^-256 级别的意外

    const Bytes sample = backup::random_bytes(64);
    REQUIRE(sample.size() == 64);
    REQUIRE(std::any_of(sample.begin(), sample.end(),
                        [](std::uint8_t byte) { return byte != 0; }));
}

TEST_CASE("encryption registry: ids and metadata", "[crypto][registry]") {
    REQUIRE(std::string(backup::to_string(EncryptionId::None)) == "none");
    REQUIRE(std::string(backup::to_string(EncryptionId::Aes256Cbc)) ==
            "aes256cbc");
    REQUIRE(std::string(backup::to_string(EncryptionId::ChaCha20Poly1305)) ==
            "chacha20poly1305");
    REQUIRE(std::string(backup::to_string(EncryptionId::Rc4)) == "rc4");

    REQUIRE(backup::encryption_from_string("none") == EncryptionId::None);
    REQUIRE(backup::encryption_from_string("aes256cbc") ==
            EncryptionId::Aes256Cbc);
    REQUIRE(backup::encryption_from_string("chacha20poly1305") ==
            EncryptionId::ChaCha20Poly1305);
    REQUIRE(backup::encryption_from_string("rc4") == EncryptionId::Rc4);
    REQUIRE(!backup::encryption_from_string("aes128cbc").has_value());

    const auto& algorithms = backup::encryption_algorithms();
    REQUIRE(algorithms.size() == 4);
    // 4 种算法都声明为自研（本文件实现）。
    for (const auto& info : algorithms) {
        REQUIRE(info.self_made == (info.id != "none"));
    }
}

TEST_CASE("make_cipher: all implemented ciphers and their sizes",
          "[crypto][registry]") {
    REQUIRE(backup::make_cipher(EncryptionId::None) == nullptr);

    struct Expectation {
        EncryptionId id;
        std::size_t key_size;
        std::size_t iv_size;
    };
    const Expectation expectations[] = {
        {EncryptionId::Aes256Cbc, 32, 16},
        {EncryptionId::ChaCha20Poly1305, 32, 12},
        {EncryptionId::Rc4, 32, 16},
    };
    for (const Expectation& expectation : expectations) {
        const auto cipher = backup::make_cipher(expectation.id);
        REQUIRE(cipher != nullptr);
        REQUIRE(cipher->id() == expectation.id);
        REQUIRE(cipher->key_size() == expectation.key_size);
        REQUIRE(cipher->iv_size() == expectation.iv_size);
    }
}

// ===========================================================================
// 端到端（模拟 VBAK v3 的用法）：PBKDF2 派生 + 随机 IV + 每条目加解密
// ===========================================================================

TEST_CASE("cipher + derive_key: VBAK-style roundtrip", "[crypto][integration]") {
    const char* const algorithms[] = {"aes256cbc", "chacha20poly1305", "rc4"};
    for (const char* algorithm : algorithms) {
        const auto id = backup::encryption_from_string(algorithm);
        REQUIRE(id.has_value());
        const auto cipher = backup::make_cipher(*id);
        REQUIRE(cipher != nullptr);

        const Bytes salt = backup::random_bytes(16);
        REQUIRE(salt.size() == 16);
        std::string error;
        const Bytes key = backup::derive_key("test123", salt, 1000,
                                             cipher->key_size(), error);
        REQUIRE(error.empty());
        REQUIRE(key.size() == cipher->key_size());

        for (const std::size_t size : {0u, 1u, 16u, 1000u, 50000u}) {
            const Bytes plaintext = make_pattern(size);
            const Bytes iv = backup::random_bytes(cipher->iv_size());
            const Bytes ciphertext = cipher->encrypt(plaintext, key, iv, error);
            REQUIRE(error.empty());
            const Bytes restored = cipher->decrypt(ciphertext, key, iv, error);
            REQUIRE(error.empty());
            REQUIRE(restored == plaintext);
        }

        // 错误密码：ChaCha20-Poly1305 的 AEAD 标签校验必然失败。
        // AES 的错误密钥/错误填充另有固定向量测试（PKCS#7 校验本身有极低
        // 概率被随机数据“蒙中”，固定向量可保证测试确定性）；RC4 无认证，
        // 错误密码只会解出垃圾数据、不会报错。
        if (*id == EncryptionId::ChaCha20Poly1305) {
            const Bytes other_salt = backup::random_bytes(16);
            const Bytes other_key = backup::derive_key(
                "wrong-password", other_salt, 1000, cipher->key_size(), error);
            REQUIRE(error.empty());
            const Bytes plaintext = bytes_of("FileBackup encrypted payload");
            const Bytes iv = backup::random_bytes(cipher->iv_size());
            const Bytes ciphertext =
                cipher->encrypt(plaintext, key, iv, error);
            REQUIRE(error.empty());
            const Bytes bad = cipher->decrypt(ciphertext, other_key, iv, error);
            REQUIRE(!error.empty());
            REQUIRE(bad.empty());
        }
    }
}
