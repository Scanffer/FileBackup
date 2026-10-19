#include <catch.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "algorithm.h"
#include "pack.h"

namespace {

namespace fs = std::filesystem;

fs::path fresh_temp_dir() {
    const fs::path dir = fs::temp_directory_path() / "filebackup_pack_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string make_repetitive(std::size_t count) {
    std::string text;
    text.reserve(count);
    const std::string pattern = "FileBackup 文件备份 - DEFLATE test pattern. ";
    while (text.size() < count) text += pattern;
    text.resize(count);
    return text;
}

}  // namespace

TEST_CASE("crc32 matches the standard test vector", "[pack][crc32]") {
    const std::string text = "123456789";
    REQUIRE(backup::crc32(
                reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size()) == 0xCBF43926u);
}

TEST_CASE("pack/unpack with deflate roundtrip", "[pack]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    const fs::path archive = base / "docs.vbak";

    const std::string big = make_repetitive(200000);
    const std::string chinese = "中文路径与内容测试：文件备份软件。\n";
    const std::string binary(4096, '\0');
    write_text(src / "a.txt", big);
    write_text(src / "sub" / "b.txt", "hello nested world");
    write_text(src / "sub" / "中文.txt", chinese);
    write_text(src / "binary.bin", binary);

    backup::PackOptions options;
    options.src = src;
    options.archive = archive;
    options.compression = backup::CompressionId::Deflate;
    options.compression_level = 6;

    const backup::PackResult packed = backup::pack_directory(options);
    REQUIRE(packed.entries_written == 4);
    REQUIRE(packed.bytes_written == big.size() + 18 + chinese.size() + 4096);
    REQUIRE(fs::file_size(archive) < packed.bytes_written / 2);

    backup::UnpackOptions unpack;
    unpack.archive = archive;
    unpack.dst = dst;
    const backup::UnpackResult restored = backup::unpack_archive(unpack);

    REQUIRE(restored.entries_read == 4);
    REQUIRE(read_text(dst / "a.txt") == big);
    REQUIRE(read_text(dst / "sub" / "b.txt") == "hello nested world");
    REQUIRE(read_text(dst / "sub" / "中文.txt") == chinese);
    REQUIRE(read_text(dst / "binary.bin") == binary);

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("pack/unpack without compression roundtrip", "[pack]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    const fs::path archive = base / "raw.vbak";

    write_text(src / "one.txt", "one");
    write_text(src / "two.txt", "two two two");

    backup::PackOptions options;
    options.src = src;
    options.archive = archive;
    options.compression = backup::CompressionId::None;

    const backup::PackResult packed = backup::pack_directory(options);
    REQUIRE(packed.entries_written == 2);
    // 不压缩时归档应该只比原始数据多一点点头部开销。
    REQUIRE(fs::file_size(archive) < packed.bytes_written + 512);

    backup::UnpackOptions unpack;
    unpack.archive = archive;
    unpack.dst = dst;
    backup::unpack_archive(unpack);
    REQUIRE(read_text(dst / "one.txt") == "one");
    REQUIRE(read_text(dst / "two.txt") == "two two two");

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("pack falls back to raw storage for incompressible bytes", "[pack]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    const fs::path archive = base / "rand.vbak";

    std::string blob;
    blob.resize(100000);
    std::uint32_t state = 0x12345678u;
    for (char& byte : blob) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<char>((state >> 16) & 0xFF);
    }
    write_text(src / "random.bin", blob);

    backup::PackOptions options;
    options.src = src;
    options.archive = archive;
    options.compression = backup::CompressionId::Deflate;

    const backup::PackResult packed = backup::pack_directory(options);
    REQUIRE(packed.entries_written == 1);
    // 不可压缩数据应退化到原始存储：归档大小 ≈ 原始大小 + 条目头。
    REQUIRE(fs::file_size(archive) <= packed.bytes_written + 1024);

    backup::UnpackOptions unpack;
    unpack.archive = archive;
    unpack.dst = dst;
    backup::unpack_archive(unpack);
    REQUIRE(read_text(dst / "random.bin") == blob);

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("pack/unpack encrypted archives (AES/ChaCha/RC4)", "[pack][crypto]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const std::string repetitive = make_repetitive(12000);
    write_text(src / "a.txt", repetitive);
    write_text(src / "sub" / "b.txt", "encrypted nested payload");
    write_text(src / "中文.txt", "中文内容加密测试。\n");

    const std::string password = "P@ss-中文-123";
    for (const backup::EncryptionId enc :
         {backup::EncryptionId::Aes256Cbc,
          backup::EncryptionId::ChaCha20Poly1305,
          backup::EncryptionId::Rc4}) {
        CAPTURE(backup::to_string(enc));
        const fs::path archive =
            base / (std::string("enc-") + backup::to_string(enc) + ".vbak");

        backup::PackOptions options;
        options.src = src;
        options.archive = archive;
        options.compression = backup::CompressionId::Deflate;
        options.encryption = enc;
        options.password = password;

        const backup::PackResult packed = backup::pack_directory(options);
        REQUIRE(packed.entries_written == 3);

        backup::UnpackOptions unpack;
        unpack.archive = archive;
        unpack.dst = base / (std::string("out-") + backup::to_string(enc));
        unpack.password = password;

        const backup::UnpackResult restored = backup::unpack_archive(unpack);
        REQUIRE(restored.entries_read == 3);
        REQUIRE(read_text(unpack.dst / "a.txt") == repetitive);
        REQUIRE(read_text(unpack.dst / "sub" / "b.txt") ==
                "encrypted nested payload");
        REQUIRE(read_text(unpack.dst / "中文.txt") ==
                "中文内容加密测试。\n");

        SECTION("missing password is rejected") {
            backup::UnpackOptions bad = unpack;
            bad.password.clear();
            REQUIRE_THROWS(backup::unpack_archive(bad));
        }

        SECTION("wrong password is rejected") {
            backup::UnpackOptions bad = unpack;
            bad.password = "wrong-password";
            REQUIRE_THROWS(backup::unpack_archive(bad));
        }

        SECTION("tampered archive is rejected") {
            std::string bytes = read_text(archive);
            REQUIRE(!bytes.empty());
            bytes.back() = static_cast<char>(bytes.back() ^ 0x7F);
            const fs::path tampered = base / "tampered.vbak";
            write_text(tampered, bytes);

            backup::UnpackOptions bad = unpack;
            bad.archive = tampered;
            REQUIRE_THROWS(backup::unpack_archive(bad));
        }
    }

    std::error_code ec;
    fs::remove_all(base, ec);
}

