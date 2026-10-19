#include <catch.hpp>

#include <filesystem>
#include <fstream>
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
