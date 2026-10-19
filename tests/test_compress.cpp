#include <catch.hpp>

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
