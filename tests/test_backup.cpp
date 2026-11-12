#include <catch.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "backup.h"
#include "filter.h"
#include "metadata.h"
#include "test_tmp.h"

namespace {

namespace fs = std::filesystem;

fs::path fresh_temp_dir() {
    return backup_test::fresh_dir("filebackup_mirror_test");
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("mirror backup applies filter and writes manifest", "[mirror][filter]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    write_text(src / "docs" / "a.txt", "keep");
    write_text(src / "docs" / "b.log", "drop by name");
    write_text(src / "pics" / "c.jpg", "drop by include-only");

    std::string error;
    auto filter = backup::Filter::from_json(
        std::string(R"([
            {"action":"include","path":"docs/**"},
            {"action":"exclude","name":"*.log"}
        ])"),
        error);
    REQUIRE(filter.has_value());

    backup::MirrorOptions options;
    options.src = src;
    options.dst = dst;
    options.filter = *filter;
    options.keep_metadata = true;

    const backup::MirrorResult result = backup::mirror_directory(options);
    REQUIRE(result.files_copied == 1);
    REQUIRE(result.entries_filtered == 2);
    REQUIRE(fs::exists(dst / "docs" / "a.txt"));
    REQUIRE_FALSE(fs::exists(dst / "docs" / "b.log"));
    REQUIRE_FALSE(fs::exists(dst / "pics" / "c.jpg"));
    REQUIRE(fs::exists(dst / ".backup-manifest.json"));

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("mirror restore reapplies metadata from manifest", "[mirror][metadata]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    const fs::path out = base / "out";
    write_text(src / "meta.txt", "payload");

    std::string error;
    auto meta = backup::read_metadata(src / "meta.txt", error);
    REQUIRE(meta.has_value());
    meta->mtime_ns = 1234567890000000000LL;
    std::string apply_error;
    REQUIRE(backup::apply_metadata(src / "meta.txt", *meta, apply_error));

    backup::MirrorOptions backup_options;
    backup_options.src = src;
    backup_options.dst = dst;
    backup_options.keep_metadata = true;
    REQUIRE(backup::mirror_directory(backup_options).files_copied == 1);

    backup::MirrorRestoreOptions restore_options;
    restore_options.src = dst;
    restore_options.dst = out;
    restore_options.restore_metadata = true;
    REQUIRE(backup::restore_mirror(restore_options).files_copied == 1);

    std::string read_error;
    auto restored = backup::read_metadata(out / "meta.txt", read_error);
    REQUIRE(restored.has_value());
    REQUIRE(restored->mtime_ns == 1234567890000000000LL);

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("mirror backup --no-metadata leaves no manifest", "[mirror][metadata]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    write_text(src / "plain.txt", "plain");

    backup::MirrorOptions options;
    options.src = src;
    options.dst = dst;
    options.keep_metadata = false;

    REQUIRE(backup::mirror_directory(options).files_copied == 1);
    REQUIRE(fs::exists(dst / "plain.txt"));
    REQUIRE_FALSE(fs::exists(dst / ".backup-manifest.json"));

    std::error_code ec;
    fs::remove_all(base, ec);
}

// ------------------------------------------------------------------
// 回归：源目录 == 目标目录。早期实现「先删后拷」，会把源文件本身删掉，
// 随后 copy_file 报 no_such_file_or_directory，源文件永久丢失。
// ------------------------------------------------------------------
TEST_CASE("mirror backup refuses identical src and dst", "[mirror][safety]") {
    const fs::path base = fresh_temp_dir();
    const fs::path dir = base / "same";
    write_text(dir / "a" / "first.txt", "first");
    write_text(dir / "b" / "second.txt", "second");

    backup::MirrorOptions options;
    options.src = dir;
    options.dst = dir;

    REQUIRE_THROWS_AS(backup::mirror_directory(options), std::runtime_error);

    // 最关键的一条：两个源文件都还在，内容一字未改。
    REQUIRE(fs::exists(dir / "a" / "first.txt"));
    REQUIRE(fs::exists(dir / "b" / "second.txt"));
    REQUIRE(read_text(dir / "a" / "first.txt") == "first");
    REQUIRE(read_text(dir / "b" / "second.txt") == "second");

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST_CASE("mirror restore refuses identical src and dst", "[mirror][safety]") {
    const fs::path base = fresh_temp_dir();
    const fs::path dir = base / "same";
    write_text(dir / "only.txt", "only");

    backup::MirrorRestoreOptions options;
    options.src = dir;
    options.dst = dir;

    REQUIRE_THROWS_AS(backup::restore_mirror(options), std::runtime_error);
    REQUIRE(fs::exists(dir / "only.txt"));
    REQUIRE(read_text(dir / "only.txt") == "only");

    std::error_code ec;
    fs::remove_all(base, ec);
}

// 目标落在源内部（或反之）同样会让上一次的复制结果被当成源再复制一遍，
// 目录逐层膨胀；入口直接拒绝。
TEST_CASE("mirror backup refuses nested src and dst", "[mirror][safety]") {
    const fs::path base = fresh_temp_dir();
    const fs::path src = base / "src";
    const fs::path inner = src / "inner";
    write_text(src / "file.txt", "content");

    backup::MirrorOptions nested_in_src;
    nested_in_src.src = src;
    nested_in_src.dst = inner;
    REQUIRE_THROWS_AS(backup::mirror_directory(nested_in_src),
                      std::runtime_error);
    // 校验在任何写盘动作之前，所以不能留下被建出来的目标目录。
    REQUIRE_FALSE(fs::exists(inner));

    backup::MirrorOptions src_inside_dst;
    src_inside_dst.src = inner;
    src_inside_dst.dst = src;
    // 源目录不存在，先建一个空目录再来一次（此时 src 在 dst 内部）。
    fs::create_directories(inner);
    REQUIRE_THROWS_AS(backup::mirror_directory(src_inside_dst),
                      std::runtime_error);

    std::error_code ec;
    fs::remove_all(base, ec);
}
