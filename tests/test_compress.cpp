#include <catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "algorithm.h"

using backup::Bytes;
using backup::CompressionId;
using backup::ICompressor;

namespace {

Bytes bytes_of(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

std::string text_of(const Bytes& data) {
    return std::string(data.begin(), data.end());
}

Bytes from_hex(const std::string& hex) {
    Bytes out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(nibble(hex[i]) * 16 +
                                                nibble(hex[i + 1])));
    }
    return out;
}

Bytes make_pattern(std::size_t size) {
    Bytes data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37 + 11) % 251);
    }
    return data;
}

}  // namespace

TEST_CASE("deflate roundtrip: empty input", "[deflate]") {
    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);

    std::string error;
    const Bytes compressed = compressor->compress({}, 6, error);
    REQUIRE(error.empty());
    REQUIRE(compressed.size() >= 8);  // zlib 头 + 空块 + Adler-32

    const Bytes restored = compressor->decompress(compressed, 0, error);
    REQUIRE(error.empty());
    REQUIRE(restored.empty());
}

TEST_CASE("deflate roundtrip: levels 1..9", "[deflate]") {
    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);

    Bytes data;
    const std::string line = "文件备份 FileBackup 2026 - self implemented DEFLATE.\n";
    for (int i = 0; i < 2000; ++i) {
        const Bytes chunk = bytes_of(line);
        data.insert(data.end(), chunk.begin(), chunk.end());
    }
    data.insert(data.end(), 4096, static_cast<std::uint8_t>(0x00));
    data.insert(data.end(), 4096, static_cast<std::uint8_t>(0xFF));

    for (int level = 1; level <= 9; ++level) {
        std::string error;
        const Bytes compressed = compressor->compress(data, level, error);
        REQUIRE(error.empty());
        const Bytes restored =
            compressor->decompress(compressed, data.size(), error);
        REQUIRE(error.empty());
        REQUIRE(restored == data);
    }
}

TEST_CASE("deflate roundtrip: binary and incompressible data", "[deflate]") {
    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);

    for (const Bytes& data : {make_pattern(1),
                              make_pattern(1000),
                              make_pattern(70000),
                              make_pattern(300000)}) {
        std::string error;
        const Bytes compressed = compressor->compress(data, 6, error);
        REQUIRE(error.empty());
        const Bytes restored =
            compressor->decompress(compressed, data.size(), error);
        REQUIRE(error.empty());
        REQUIRE(restored == data);
    }
}

TEST_CASE("deflate actually compresses repetitive data", "[deflate]") {
    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);

    const Bytes data = bytes_of(std::string(300000, 'A'));
    std::string error;
    const Bytes compressed = compressor->compress(data, 6, error);
    REQUIRE(error.empty());
    REQUIRE(compressed.size() < data.size() / 4);
}

TEST_CASE("deflate inflates a python/zlib dynamic-Huffman stream", "[deflate]") {
    // 由 Python 3 的 zlib.compress(level=9) 生成：标准动态 Huffman 块。
    const std::string hex =
        "78da737175f3710c715548a9cc4bcccd4c56f0284d4bcb4dcc53284b4d2ec92fb252"
        "60646266616563e7e0e4e2e6e1e5e317101412161115139790949296919593575054"
        "52565155535748cbcc4955484a4cce2e2d502828cacf026a5730323032537019b560d4"
        "82c16f01004d43b379";
    const Bytes compressed = from_hex(hex);

    std::string expected;
    const std::string prefix = "DEFLATE dynamic Huffman vector: ";
    const std::string suffix = " file backup project 2026 ";
    for (int round = 0; round < 8; ++round) {
        expected += prefix;
        for (int byte = 1; byte <= 39; ++byte) {
            expected.push_back(static_cast<char>(byte));
        }
        expected += suffix;
    }

    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);
    std::string error;
    const Bytes restored = compressor->decompress(
        compressed, expected.size(), error);
    REQUIRE(error.empty());
    REQUIRE(text_of(restored) == expected);
}

TEST_CASE("deflate inflates a zlib stored-block stream", "[deflate]") {
    // Python zlib.compressobj(level=0)，即 BTYPE=0 的非压缩块。
    const std::string hex =
        "7801012400dbff73746f72656420626c6f636b20766563746f7220666f7220696e666c"
        "6174652074657374fec60dba";
    const Bytes compressed = from_hex(hex);
    const std::string expected = "stored block vector for inflate test";

    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);
    std::string error;
    const Bytes restored =
        compressor->decompress(compressed, expected.size(), error);
    REQUIRE(error.empty());
    REQUIRE(text_of(restored) == expected);
}

TEST_CASE("deflate rejects corrupted or truncated streams", "[deflate]") {
    auto compressor = backup::make_compressor(CompressionId::Deflate);
    REQUIRE(compressor != nullptr);

    const Bytes data = bytes_of(std::string(20000, 'x'));
    std::string error;
    Bytes compressed = compressor->compress(data, 6, error);
    REQUIRE(error.empty());

    SECTION("bad zlib header check") {
        Bytes broken = compressed;
        broken[1] ^= 0x01;
        const Bytes restored =
            compressor->decompress(broken, data.size(), error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("truncated stream") {
        Bytes broken(compressed.begin(),
                     compressed.begin() + static_cast<std::ptrdiff_t>(
                         compressed.size() / 2));
        const Bytes restored =
            compressor->decompress(broken, data.size(), error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("wrong expected size") {
        const Bytes restored =
            compressor->decompress(compressed, data.size() + 1, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("compression registry and ids", "[algorithm]") {
    REQUIRE(backup::compression_from_string("deflate") ==
            CompressionId::Deflate);
    REQUIRE(!backup::compression_from_string("no-such-algo").has_value());
    REQUIRE(std::string(backup::to_string(CompressionId::Deflate)) ==
            "deflate");
    REQUIRE(backup::clamp_compression_level(CompressionId::Deflate, 99) ==
            9);
}

// ===========================================================================
// LZ4 / RLE / LZW（自研新增算法）
// ===========================================================================

namespace {

const std::vector<CompressionId>& new_algorithms() {
    static const std::vector<CompressionId> ids = {
        CompressionId::Lz4, CompressionId::Rle, CompressionId::Lzw};
    return ids;
}

/// 确定性伪随机字节（xorshift32），保证测试可复现。
Bytes random_bytes(std::size_t size, std::uint32_t seed) {
    Bytes data(size);
    std::uint32_t state = seed == 0 ? 1u : seed;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

Bytes repeat_pattern(const Bytes& pattern, std::size_t total) {
    Bytes data;
    data.reserve(total);
    while (data.size() < total) {
        const std::size_t chunk =
            std::min<std::size_t>(pattern.size(), total - data.size());
        data.insert(data.end(), pattern.begin(),
                    pattern.begin() + static_cast<std::ptrdiff_t>(chunk));
    }
    return data;
}

/// 压缩 + 解压一次并断言完整往返；level 取多个值验证与新算法无关。
void require_roundtrip(CompressionId id, const Bytes& data) {
    auto compressor = backup::make_compressor(id);
    REQUIRE(compressor != nullptr);
    INFO("algorithm = " << backup::to_string(id)
                        << ", input size = " << data.size());
    for (const int level : {1, 6, 9}) {
        std::string error;
        const Bytes compressed = compressor->compress(data, level, error);
        REQUIRE(error.empty());
        const Bytes restored =
            compressor->decompress(compressed, data.size(), error);
        REQUIRE(error.empty());
        REQUIRE(restored.size() == data.size());
        REQUIRE(restored == data);
    }
}

Bytes compress_with(CompressionId id, const Bytes& data) {
    auto compressor = backup::make_compressor(id);
    REQUIRE(compressor != nullptr);
    std::string error;
    const Bytes compressed = compressor->compress(data, 6, error);
    REQUIRE(error.empty());
    return compressed;
}

}  // namespace

TEST_CASE("new compressors: factory, ids and metadata", "[compress][algorithm]") {
    for (const CompressionId id : new_algorithms()) {
        auto compressor = backup::make_compressor(id);
        REQUIRE(compressor != nullptr);
        REQUIRE(compressor->id() == id);

        const std::string name = backup::to_string(id);
        const auto parsed = backup::compression_from_string(name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == id);

        bool registered = false;
        for (const auto& info : backup::compression_algorithms()) {
            if (info.id == name) {
                registered = true;
                REQUIRE(info.self_made);
                REQUIRE(!info.label.empty());
            }
        }
        REQUIRE(registered);
    }
    REQUIRE(backup::make_compressor(CompressionId::None) == nullptr);
}

TEST_CASE("new compressors: empty and tiny inputs roundtrip", "[compress]") {
    const std::vector<Bytes> inputs = {
        {},
        {0x00},
        {0x00, 0x00},
        {0xFF},
        {0xFF, 0xFF},
        {0x41, 0x42},
        {0x42, 0x41, 0x42},
        bytes_of("abc"),
        {0x00, 0xFF, 0x00},
        {0x7F, 0x80, 0x81, 0xFE, 0xFF},
    };
    for (const CompressionId id : new_algorithms()) {
        for (const Bytes& data : inputs) {
            require_roundtrip(id, data);
        }
    }
}

TEST_CASE("lz4: exact standard block output for tiny inputs", "[lz4][compress]") {
    using backup::make_compressor;

    // 空块 = 单个 0x00 token（空最后字面量序列）。
    REQUIRE(compress_with(CompressionId::Lz4, {}) == from_hex("00"));
    // 1 字节：token 高 4 位 = 1（字面量），随后是字节本身。
    REQUIRE(compress_with(CompressionId::Lz4, {0x41}) == from_hex("1041"));
    // 3 字节字面量：token = 0x30，低 4 位（匹配长度）为 0。
    REQUIRE(compress_with(CompressionId::Lz4, bytes_of("abc")) ==
            from_hex("30616263"));

    std::string error;
    auto compressor = backup::make_compressor(CompressionId::Lz4);
    REQUIRE(compressor != nullptr);
    // 空流不是合法 block 格式（至少要有一个 token）。
    const Bytes restored = compressor->decompress({}, 0, error);
    REQUIRE(!error.empty());
    REQUIRE(restored.empty());
}

TEST_CASE("lz4: decodes hand-built standard blocks", "[lz4][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Lz4);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("empty block") {
        const Bytes restored = compressor->decompress(from_hex("00"), 0, error);
        REQUIRE(error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("literal-only block") {
        // token 0x30 = 3 个字面量，无匹配。
        const Bytes restored =
            compressor->decompress(from_hex("30616263"), 3, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "abc");
    }

    SECTION("match sequence + last literals") {
        // token 0x35：3 字面量 "abc"，匹配长度 5+4=9，offset=3；
        // 之后 token 0x60 + 6 字面量 "abcabc"。
        // 输出 = "abc" + 复制 9 字节 = "abcabcabcabc"，再加 "abcabc"。
        const Bytes restored = compressor->decompress(
            from_hex("35616263030060" "616263616263"), 18, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "abcabcabcabcabcabc");
    }

    SECTION("extended literal length") {
        // token 0xF0：15 + 扩展字节 5 = 20 个字面量，即 "0123456789abcdefghij"。
        Bytes block = from_hex("f005");
        const Bytes literals = bytes_of("0123456789abcdefghij");
        block.insert(block.end(), literals.begin(), literals.end());
        const Bytes restored = compressor->decompress(block, 20, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "0123456789abcdefghij");
    }
}

TEST_CASE("rle: decodes hand-built packets", "[rle][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Rle);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("literal packet") {
        // 0x03 = 4 个原始字节。
        const Bytes restored =
            compressor->decompress(from_hex("0361626364"), 4, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "abcd");
    }

    SECTION("repeat packets") {
        // 0x80 = 重复 2 次，0x81 = 3 次，0x82 = 4 次。
        REQUIRE(text_of(compressor->decompress(from_hex("8041"), 2, error)) ==
                "AA");
        REQUIRE(error.empty());
        REQUIRE(text_of(compressor->decompress(from_hex("8141"), 3, error)) ==
                "AAA");
        REQUIRE(error.empty());
        REQUIRE(text_of(compressor->decompress(from_hex("8241"), 4, error)) ==
                "AAAA");
        REQUIRE(error.empty());
    }

    SECTION("mixed packets") {
        // 1 个字面量 'x'，再重复 'y' 3 次。
        const Bytes restored =
            compressor->decompress(from_hex("00788179"), 4, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "xyyy");
    }

    SECTION("empty stream") {
        const Bytes restored = compressor->decompress({}, 0, error);
        REQUIRE(error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("lzw: decodes hand-built code streams", "[lzw][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Lzw);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("empty stream = CLEAR + END") {
        // 9 位 CLEAR(256) + 9 位 END(257)，LSB 优先打包。
        REQUIRE(compress_with(CompressionId::Lzw, {}) == from_hex("000302"));
        const Bytes restored =
            compressor->decompress(from_hex("000302"), 0, error);
        REQUIRE(error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("ABABABA") {
        // 码序列：CLEAR, 'A', 'B', 258, 260, END，均为 9 位。
        // 解压端在收到 260（== 下一个待分配码）时走 KwKwK 分支：258 + 'A'。
        const Bytes restored = compressor->decompress(
            from_hex("00830811483020"), 7, error);
        REQUIRE(error.empty());
        REQUIRE(text_of(restored) == "ABABABA");
    }
}

TEST_CASE("new compressors: repetitive data compresses", "[compress]") {
    const Bytes all_a(300000, 0x41);

    // 所有三种算法对单一字节的长游程都应显著压缩。
    for (const CompressionId id : new_algorithms()) {
        INFO("algorithm = " << backup::to_string(id));
        const Bytes compressed = compress_with(id, all_a);
        REQUIRE(compressed.size() < all_a.size() / 16);
        require_roundtrip(id, all_a);
    }

    // 周期性文本：LZ4 / LZW 都能找到跨周期匹配，必然明显压缩。
    const Bytes sentence = bytes_of(
        "FileBackup 2026 - self implemented LZ4/RLE/LZW compression. "
        "0123456789 abcdefghijklmnopqrstuvwxyz. ");
    const Bytes repeated = repeat_pattern(sentence, 300000);
    for (const CompressionId id :
         {CompressionId::Lz4, CompressionId::Lzw}) {
        INFO("algorithm = " << backup::to_string(id));
        const Bytes compressed = compress_with(id, repeated);
        REQUIRE(compressed.size() < repeated.size() / 8);
        require_roundtrip(id, repeated);
    }

    // RLE 对“多段游程”数据应显著压缩。
    Bytes runs_data;
    for (int i = 0; i < 200; ++i) {
        runs_data.insert(runs_data.end(), 256, static_cast<std::uint8_t>(0x00));
        runs_data.insert(runs_data.end(), 700, static_cast<std::uint8_t>(0xFF));
        runs_data.insert(runs_data.end(), 3, static_cast<std::uint8_t>(i));
    }
    INFO("rle runs input size = " << runs_data.size());
    const Bytes runs_compressed = compress_with(CompressionId::Rle, runs_data);
    REQUIRE(runs_compressed.size() < runs_data.size() / 16);
    require_roundtrip(CompressionId::Rle, runs_data);
}

TEST_CASE("new compressors: random / binary / >=100KB roundtrip", "[compress]") {
    const std::vector<Bytes> inputs = {
        random_bytes(1, 0x12345678u),
        random_bytes(1000, 0x12345678u),
        make_pattern(70000),          // 70KB 半随机二进制
        random_bytes(128 * 1024, 0xC0FFEEu),  // 128KB 真随机（不可压缩）
        make_pattern(150000),         // 150KB 周期性二进制
    };
    for (const CompressionId id : new_algorithms()) {
        for (const Bytes& data : inputs) {
            require_roundtrip(id, data);
        }
    }
}

TEST_CASE("new compressors: wrong expected_size is rejected", "[compress]") {
    const Bytes data = repeat_pattern(
        bytes_of("expected-size-check-0123456789 "), 50000);
    for (const CompressionId id : new_algorithms()) {
        INFO("algorithm = " << backup::to_string(id));
        auto compressor = backup::make_compressor(id);
        REQUIRE(compressor != nullptr);
        std::string error;
        const Bytes compressed = compressor->compress(data, 6, error);
        REQUIRE(error.empty());

        const std::vector<std::size_t> wrong_sizes = {
            data.size() + 1, data.size() - 1, 0};
        for (const std::size_t wrong_size : wrong_sizes) {
            INFO("expected_size = " << wrong_size
                                    << ", real size = " << data.size());
            const Bytes restored =
                compressor->decompress(compressed, wrong_size, error);
            REQUIRE(!error.empty());
            REQUIRE(restored.empty());
        }
    }
}

TEST_CASE("new compressors: corrupted or truncated streams are rejected",
          "[compress]") {
    const Bytes data = repeat_pattern(
        bytes_of("corruption-check-0123456789 "), 40000);
    for (const CompressionId id : new_algorithms()) {
        INFO("algorithm = " << backup::to_string(id));
        auto compressor = backup::make_compressor(id);
        REQUIRE(compressor != nullptr);
        std::string error;
        const Bytes compressed = compressor->compress(data, 6, error);
        REQUIRE(error.empty());
        REQUIRE(compressed.size() > 8);

        // 截断到一半：三种格式都必须报错（不会返回错误大小的数据）。
        const Bytes truncated(
            compressed.begin(),
            compressed.begin() + static_cast<std::ptrdiff_t>(
                                     compressed.size() / 2));
        const Bytes restored =
            compressor->decompress(truncated, data.size(), error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("lz4: malformed blocks are rejected", "[lz4][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Lz4);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("offset larger than produced data") {
        // token 0x25：2 字面量 "ab"，匹配长度 5+4=9；offset=5 但只产出了 2 字节。
        const Bytes broken = from_hex("2561620500");
        const Bytes restored = compressor->decompress(broken, 11, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("literal length exceeds remaining stream") {
        // token 0xF0 -> 字面量长度扩展，但扩展字节都被流截断。
        const Bytes broken = from_hex("f0");
        const Bytes restored = compressor->decompress(broken, 20, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("missing match offset") {
        // token 0x20：2 字节字面量后还剩 1 字节，无法读出 2 字节 offset。
        const Bytes broken = from_hex("20616263");
        const Bytes restored = compressor->decompress(broken, 10, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("truncated match extension") {
        // token 0x2F：2 字面量，匹配长度扩展被截断。
        const Bytes broken = from_hex("2f6162" "0300");
        const Bytes restored = compressor->decompress(broken, 20, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("rle: malformed streams are rejected", "[rle][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Rle);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("truncated literal packet") {
        // 0x05 = 6 个字节，但只跟了 1 个。
        const Bytes broken = from_hex("0561");
        const Bytes restored = compressor->decompress(broken, 6, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("repeat packet missing value") {
        const Bytes broken = from_hex("82");
        const Bytes restored = compressor->decompress(broken, 4, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("output larger than expected_size") {
        // 字面量 packet 声明 6 字节，但 expected_size 只有 3。
        const Bytes broken = from_hex("056162636465");
        const Bytes restored = compressor->decompress(broken, 3, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("clean stream but wrong size") {
        const Bytes stream = from_hex("8041");  // "AA"
        const Bytes restored = compressor->decompress(stream, 3, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("lzw: malformed streams are rejected", "[lzw][compress]") {
    auto compressor = backup::make_compressor(CompressionId::Lzw);
    REQUIRE(compressor != nullptr);
    std::string error;

    SECTION("empty stream") {
        const Bytes restored = compressor->decompress({}, 0, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("missing initial CLEAR") {
        // 第一个 9 位码 = 1，不是 CLEAR(256)。
        const Bytes broken = from_hex("0100");
        const Bytes restored = compressor->decompress(broken, 0, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("invalid dictionary code") {
        // CLEAR 之后直接出现码 300（>= 下一个待分配码 258），非法。
        const Bytes broken = from_hex("005902");
        const Bytes restored = compressor->decompress(broken, 1, error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }

    SECTION("truncated stream without END") {
        auto lzw = backup::make_compressor(CompressionId::Lzw);
        REQUIRE(lzw != nullptr);
        std::string compress_error;
        const Bytes source = repeat_pattern(bytes_of("lzw-truncation-"), 4000);
        const Bytes compressed = lzw->compress(source, 6, compress_error);
        REQUIRE(compress_error.empty());
        REQUIRE(compressed.size() > 4);
        const Bytes truncated(
            compressed.begin(),
            compressed.begin() + static_cast<std::ptrdiff_t>(
                                     compressed.size() / 2));
        const Bytes restored =
            lzw->decompress(truncated, source.size(), error);
        REQUIRE(!error.empty());
        REQUIRE(restored.empty());
    }
}

TEST_CASE("new compressors: garbage streams never crash", "[compress]") {
    // 确定性随机垃圾：解压端必须要么报错、要么返回与 expected_size 一致的
    // 数据；不能越界/崩溃（配合 -D_GLIBCXX_ASSERTIONS 运行时更严格）。
    std::uint32_t state = 0x9E3779B9u;
    auto next_rand = [&]() -> std::uint32_t {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    for (const CompressionId id : new_algorithms()) {
        INFO("algorithm = " << backup::to_string(id));
        auto compressor = backup::make_compressor(id);
        REQUIRE(compressor != nullptr);
        for (int round = 0; round < 400; ++round) {
            const std::size_t size = next_rand() % 128;
            Bytes garbage(size);
            for (std::size_t i = 0; i < size; ++i) {
                garbage[i] = static_cast<std::uint8_t>(next_rand() >> 24);
            }
            const std::size_t expected = next_rand() % 512;
            std::string error;
            const Bytes restored =
                compressor->decompress(garbage, expected, error);
            if (error.empty()) {
                REQUIRE(restored.size() == expected);
            } else {
                REQUIRE(restored.empty());
            }
        }
    }
}
