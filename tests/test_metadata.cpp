#include <catch.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "metadata.h"

namespace {

namespace fs = std::filesystem;

using backup::Bytes;
using backup::Metadata;

constexpr std::int64_t kSecondNs = 1000000000LL;
constexpr std::int64_t kBaseNs = 1588748889LL * kSecondNs;  // 2020-05-06 07:08:09 UTC

// Windows 文件属性位（不引入 windows.h，保持测试文件轻量）。
constexpr std::uint32_t kAttrReadonly = 0x1;
constexpr std::uint32_t kAttrHidden = 0x2;
constexpr std::uint32_t kAttrDirectory = 0x10;
constexpr std::uint32_t kAttrArchive = 0x20;

fs::path fresh_dir(const std::string& tag) {
    const fs::path dir =
        fs::temp_directory_path() / fs::u8path("filebackup_meta_test_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void cleanup(const fs::path& dir) {
    // 测试可能设置只读/隐藏属性；remove_all 对普通文件目录足够。
    std::error_code ec;
    fs::remove_all(dir, ec);
}

fs::path make_file(const fs::path& dir, const std::string& name,
                   const std::string& content = "FileBackup metadata test") {
    const fs::path path = dir / fs::u8path(name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool near_ns(std::int64_t actual, std::int64_t expected,
             std::int64_t tolerance) {
    const std::int64_t diff = actual >= expected ? actual - expected
                                                 : expected - actual;
    return diff <= tolerance;
}

/// 校验字节串是一个合法的自相对安全描述符（SECURITY_DESCRIPTOR_RELATIVE）。
bool looks_like_self_relative_sd(const Bytes& acl) {
    if (acl.size() < 20) return false;
    if (acl[0] != 1) return false;  // Revision
    const std::uint16_t control =
        static_cast<std::uint16_t>(acl[2] | (static_cast<std::uint16_t>(acl[3]) << 8));
    return (control & 0x8000u) != 0;  // SE_SELF_RELATIVE
}

void check_same_metadata(const Metadata& actual, const Metadata& expected) {
    CHECK(actual.mode == expected.mode);
    CHECK(actual.uid == expected.uid);
    CHECK(actual.gid == expected.gid);
    CHECK(actual.owner == expected.owner);
    CHECK(actual.group == expected.group);
    CHECK(actual.mtime_ns == expected.mtime_ns);
    CHECK(actual.atime_ns == expected.atime_ns);
    CHECK(actual.ctime_ns == expected.ctime_ns);
    CHECK(actual.win_attrs == expected.win_attrs);
    CHECK(actual.acl == expected.acl);
}

Metadata make_full_metadata() {
    Metadata meta;
    meta.mode = 0755;
    meta.uid = 1000;
    meta.gid = 1000;
    meta.owner = "测试用户\\张三";  // 中文 UTF-8，验证二进制/JSON 编码
    meta.group = "users";
    meta.mtime_ns = kBaseNs;
    meta.atime_ns = kBaseNs - 3600LL * kSecondNs;
    meta.ctime_ns = kBaseNs - 7200LL * kSecondNs;
    meta.win_attrs = kAttrReadonly | kAttrHidden | kAttrArchive;
    meta.acl.resize(97);
    for (std::size_t index = 0; index < meta.acl.size(); ++index) {
        meta.acl[index] = static_cast<std::uint8_t>((index * 7 + 3) & 0xFF);
    }
    return meta;
}

}  // namespace

// ===========================================================================
// 读取
// ===========================================================================

TEST_CASE("metadata: 读取普通文件元数据", "[metadata][read]") {
    const fs::path base = fresh_dir("read");
    const fs::path file = make_file(base, "hello.txt", "hello metadata");

    std::string error;
    const std::optional<Metadata> read = backup::read_metadata(file, error);
    REQUIRE(read.has_value());
    CHECK(error.empty());

    // 可写文件：mode 带写权限位；win_attrs 不带只读位。
    CHECK((read->mode & 0200u) != 0);
    CHECK((read->mode & 0444u) != 0);
    CHECK((read->win_attrs & kAttrReadonly) == 0);
    CHECK((read->win_attrs & kAttrArchive) != 0);
    CHECK((read->win_attrs & kAttrDirectory) == 0);

    // 三个时间戳都应该是刚创建的合理时间。
    CHECK(read->mtime_ns > 0);
    CHECK(read->ctime_ns > 0);
    CHECK(read->atime_ns > 0);
    CHECK(near_ns(read->mtime_ns, now_ns(), 10LL * 60 * kSecondNs));

    // 属主与 ACL。
    CHECK_FALSE(read->owner.empty());
    CHECK(backup::file_owner_name(file) == read->owner);
    CHECK_FALSE(read->acl.empty());
    CHECK(looks_like_self_relative_sd(read->acl));

    // 目录元数据：目录属性位、mode 可写。
    const fs::path subdir = base / "sub";
    fs::create_directories(subdir);
    std::string dir_error;
    const std::optional<Metadata> directory =
        backup::read_metadata(subdir, dir_error);
    REQUIRE(directory.has_value());
    CHECK(dir_error.empty());
    CHECK((directory->win_attrs & kAttrDirectory) != 0);
    CHECK((directory->mode & 0200u) != 0);
    CHECK_FALSE(directory->acl.empty());

    cleanup(base);
}

TEST_CASE("metadata: 中文路径读写元数据", "[metadata][read][unicode]") {
    const fs::path base = fresh_dir("unicode");
    const fs::path file = make_file(base, "中文目录/报告 2025.txt", "中文内容");
    REQUIRE(fs::exists(file));

    std::string error;
    const std::optional<Metadata> read = backup::read_metadata(file, error);
    REQUIRE(read.has_value());
    CHECK(error.empty());
    CHECK_FALSE(read->owner.empty());
    CHECK_FALSE(read->acl.empty());
    CHECK(backup::file_owner_name(file) == read->owner);

    Metadata meta;
    meta.mode = 0644;
    meta.mtime_ns = kBaseNs;
    meta.win_attrs = kAttrArchive;
    CHECK(backup::apply_metadata(file, meta, error));
    CHECK(error.empty());
    const std::optional<Metadata> read_back =
        backup::read_metadata(file, error);
    REQUIRE(read_back.has_value());
    CHECK(near_ns(read_back->mtime_ns, kBaseNs, 2 * kSecondNs));

    cleanup(base);
}

TEST_CASE("metadata: 不存在路径返回失败而非崩溃", "[metadata][read][error]") {
    const fs::path base = fresh_dir("missing");
    const fs::path missing = base / "no_such_file.txt";

    std::string error;
    const std::optional<Metadata> read = backup::read_metadata(missing, error);
    CHECK_FALSE(read.has_value());
    CHECK_FALSE(error.empty());

    CHECK(backup::file_owner_name(missing).empty());

    std::string apply_error;
    CHECK_FALSE(backup::apply_metadata(missing, Metadata{}, apply_error));
    CHECK_FALSE(apply_error.empty());

    cleanup(base);
}

// ===========================================================================
// 属性 / mode
// ===========================================================================

TEST_CASE("metadata: 只读属性与 mode 互相映射", "[metadata][attrs]") {
    const fs::path base = fresh_dir("attrs");
    const fs::path file = make_file(base, "readonly.txt");
    std::string error;

    SECTION("mode 0444 -> 只读属性") {
        Metadata meta;
        meta.mode = 0444;
        meta.win_attrs = kAttrReadonly;
        CHECK(backup::apply_metadata(file, meta, error));
        CHECK(error.empty());

        const std::optional<Metadata> read = backup::read_metadata(file, error);
        REQUIRE(read.has_value());
        CHECK((read->win_attrs & kAttrReadonly) != 0);
        CHECK((read->mode & 0200u) == 0);
    }

    SECTION("mode 0644 -> 清除只读属性") {
        // 先设置只读，再验证还原为可写。
        Metadata readonly_meta;
        readonly_meta.mode = 0444;
        readonly_meta.win_attrs = kAttrReadonly;
        CHECK(backup::apply_metadata(file, readonly_meta, error));

        Metadata writable_meta;
        writable_meta.mode = 0644;
        writable_meta.win_attrs = kAttrArchive;
        CHECK(backup::apply_metadata(file, writable_meta, error));

        const std::optional<Metadata> read = backup::read_metadata(file, error);
        REQUIRE(read.has_value());
        CHECK((read->win_attrs & kAttrReadonly) == 0);
        CHECK((read->mode & 0200u) != 0);
    }

    SECTION("隐藏属性可写回") {
        Metadata meta;
        meta.mode = 0644;
        meta.win_attrs = kAttrHidden | kAttrArchive;
        CHECK(backup::apply_metadata(file, meta, error));

        const std::optional<Metadata> read = backup::read_metadata(file, error);
        REQUIRE(read.has_value());
        CHECK((read->win_attrs & kAttrHidden) != 0);
    }

    SECTION("空 Metadata 是 no-op 且成功") {
        CHECK(backup::apply_metadata(file, Metadata{}, error));
        CHECK(error.empty());
        CHECK(fs::exists(file));
        const std::optional<Metadata> read = backup::read_metadata(file, error);
        REQUIRE(read.has_value());
    }

    cleanup(base);
}

// ===========================================================================
// 时间戳
// ===========================================================================

TEST_CASE("metadata: 应用并读回时间戳", "[metadata][time]") {
    const fs::path base = fresh_dir("time");
    const fs::path file = make_file(base, "timestamps.txt", "time test");
    std::string error;

    Metadata meta;
    meta.mode = 0644;
    meta.win_attrs = kAttrArchive;
    meta.mtime_ns = kBaseNs;
    meta.atime_ns = kBaseNs - 3600LL * kSecondNs;
    meta.ctime_ns = kBaseNs - 7200LL * kSecondNs;
    CHECK(backup::apply_metadata(file, meta, error));
    CHECK(error.empty());

    const std::optional<Metadata> read = backup::read_metadata(file, error);
    REQUIRE(read.has_value());
    CHECK(near_ns(read->mtime_ns, meta.mtime_ns, 2 * kSecondNs));
    CHECK(near_ns(read->atime_ns, meta.atime_ns, 2 * kSecondNs));
    CHECK(near_ns(read->ctime_ns, meta.ctime_ns, 2 * kSecondNs));

    SECTION("只读文件也能设置时间戳（内部先清只读再恢复）") {
        Metadata readonly_meta;
        readonly_meta.mode = 0444;
        readonly_meta.win_attrs = kAttrReadonly;
        readonly_meta.mtime_ns = kBaseNs + 86400LL * kSecondNs;
        CHECK(backup::apply_metadata(file, readonly_meta, error));
        const std::optional<Metadata> readonly_read =
            backup::read_metadata(file, error);
        REQUIRE(readonly_read.has_value());
        CHECK((readonly_read->win_attrs & kAttrReadonly) != 0);
        CHECK(near_ns(readonly_read->mtime_ns, readonly_meta.mtime_ns,
                      2 * kSecondNs));
    }

    SECTION("目录时间戳") {
        const fs::path dir = base / "dir";
        fs::create_directories(dir);
        Metadata dir_meta;
        dir_meta.mode = 0755;
        dir_meta.mtime_ns = kBaseNs + 3600LL * kSecondNs;
        dir_meta.atime_ns = kBaseNs + 1800LL * kSecondNs;
        CHECK(backup::apply_metadata(dir, dir_meta, error));
        const std::optional<Metadata> dir_read = backup::read_metadata(dir, error);
        REQUIRE(dir_read.has_value());
        CHECK(near_ns(dir_read->mtime_ns, dir_meta.mtime_ns, 2 * kSecondNs));
        CHECK(near_ns(dir_read->atime_ns, dir_meta.atime_ns, 2 * kSecondNs));
        CHECK((dir_read->win_attrs & kAttrDirectory) != 0);
    }

    cleanup(base);
}

// ===========================================================================
// 二进制序列化
// ===========================================================================

TEST_CASE("metadata: 二进制序列化往返", "[metadata][serialize]") {
    const Metadata original = make_full_metadata();
    std::string error;

    const Bytes bytes = backup::metadata_to_bytes(original);
    CHECK(bytes.size() == 60 + original.owner.size() + original.group.size() +
                              original.acl.size());
    CHECK(bytes[0] == 'M');
    CHECK(bytes[4] == 1);  // version

    const std::optional<Metadata> restored =
        backup::metadata_from_bytes(bytes, error);
    REQUIRE(restored.has_value());
    CHECK(error.empty());
    check_same_metadata(*restored, original);

    SECTION("空 ACL 与空字符串也往返") {
        Metadata meta = original;
        meta.acl.clear();
        meta.owner.clear();
        meta.group.clear();
        const Bytes small = backup::metadata_to_bytes(meta);
        const std::optional<Metadata> read = backup::metadata_from_bytes(small, error);
        REQUIRE(read.has_value());
        check_same_metadata(*read, meta);
    }

    SECTION("极端数值往返") {
        Metadata meta;
        meta.mode = 0xFFFFFFFFu;
        meta.uid = 0xFFFFFFFFu;
        meta.gid = 0xFFFFFFFFu;
        meta.win_attrs = 0xFFFFFFFFu;
        meta.mtime_ns = -123456789012345LL;
        meta.atime_ns = -1;
        meta.ctime_ns = std::numeric_limits<std::int64_t>::max();
        const Bytes packed = backup::metadata_to_bytes(meta);
        const std::optional<Metadata> read =
            backup::metadata_from_bytes(packed, error);
        REQUIRE(read.has_value());
        check_same_metadata(*read, meta);
    }

    SECTION("magic 错误") {
        Bytes broken = bytes;
        broken[0] = 'X';
        CHECK_FALSE(backup::metadata_from_bytes(broken, error).has_value());
        CHECK_FALSE(error.empty());
    }

    SECTION("版本不支持") {
        Bytes broken = bytes;
        broken[4] = 99;
        CHECK_FALSE(backup::metadata_from_bytes(broken, error).has_value());
        CHECK(error.find("版本") != std::string::npos);
    }

    SECTION("截断输入") {
        for (std::size_t length : {std::size_t{0}, std::size_t{10},
                                   std::size_t{47}, std::size_t{48},
                                   bytes.size() / 2, bytes.size() - 1}) {
            Bytes broken(bytes.begin(),
                         bytes.begin() + static_cast<std::ptrdiff_t>(length));
            CHECK_FALSE(backup::metadata_from_bytes(broken, error).has_value());
            CHECK_FALSE(error.empty());
        }
    }

    SECTION("尾部多余字节") {
        Bytes broken = bytes;
        broken.push_back(0);
        CHECK_FALSE(backup::metadata_from_bytes(broken, error).has_value());
        CHECK(error.find("多余") != std::string::npos);
    }

    SECTION("长度字段越界") {
        Bytes broken = bytes;
        // owner_len 位于固定头之后的第一个 u32；改成超大值必须被边界检查拒绝。
        broken[48] = 0xFF;
        broken[49] = 0xFF;
        broken[50] = 0xFF;
        broken[51] = 0x7F;
        CHECK_FALSE(backup::metadata_from_bytes(broken, error).has_value());
        CHECK_FALSE(error.empty());
    }
}

// ===========================================================================
// JSON 序列化
// ===========================================================================

TEST_CASE("metadata: JSON 序列化往返", "[metadata][serialize][json]") {
    const Metadata original = make_full_metadata();

    const nlohmann::json json = backup::metadata_to_json(original);
    CHECK(json.is_object());
    CHECK(json["mode"] == 0755);
    CHECK(json["owner"] == original.owner);
    CHECK(json["mtime_ns"] == kBaseNs);
    CHECK(json["acl"].is_string());
    CHECK_FALSE(json["acl"].get<std::string>().empty());

    const std::optional<Metadata> restored = backup::metadata_from_json(json);
    REQUIRE(restored.has_value());
    check_same_metadata(*restored, original);

    SECTION("ACL 使用 Base64（已知向量）") {
        Metadata meta;
        meta.acl = {'M', 'E', 'T', 'A'};
        const nlohmann::json encoded = backup::metadata_to_json(meta);
        CHECK(encoded["acl"] == "TUVUQQ==");
        const std::optional<Metadata> decoded = backup::metadata_from_json(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->acl == meta.acl);
    }

    SECTION("空 ACL 编码为空串") {
        Metadata meta;
        const nlohmann::json encoded = backup::metadata_to_json(meta);
        CHECK(encoded["acl"] == "");
        const std::optional<Metadata> decoded = backup::metadata_from_json(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->acl.empty());
    }

    SECTION("空对象得到默认值") {
        const std::optional<Metadata> decoded =
            backup::metadata_from_json(nlohmann::json::object());
        REQUIRE(decoded.has_value());
        check_same_metadata(*decoded, Metadata{});
    }

    SECTION("兼容旧键名 mtime/atime/ctime") {
        nlohmann::json legacy = nlohmann::json::object();
        legacy["mtime"] = kBaseNs;
        legacy["atime"] = kBaseNs - kSecondNs;
        legacy["ctime"] = kBaseNs - 2 * kSecondNs;
        const std::optional<Metadata> decoded = backup::metadata_from_json(legacy);
        REQUIRE(decoded.has_value());
        CHECK(decoded->mtime_ns == kBaseNs);
        CHECK(decoded->atime_ns == kBaseNs - kSecondNs);
        CHECK(decoded->ctime_ns == kBaseNs - 2 * kSecondNs);
    }

    SECTION("非法 JSON 被拒绝") {
        CHECK_FALSE(backup::metadata_from_json(nlohmann::json::array()).has_value());
        CHECK_FALSE(backup::metadata_from_json(nlohmann::json("text")).has_value());

        nlohmann::json bad_mode = nlohmann::json::object();
        bad_mode["mode"] = "0644";
        CHECK_FALSE(backup::metadata_from_json(bad_mode).has_value());

        nlohmann::json bad_time = nlohmann::json::object();
        bad_time["mtime_ns"] = "yesterday";
        CHECK_FALSE(backup::metadata_from_json(bad_time).has_value());

        nlohmann::json bad_acl = nlohmann::json::object();
        bad_acl["acl"] = "!!!not-base64!!!";
        CHECK_FALSE(backup::metadata_from_json(bad_acl).has_value());

        nlohmann::json bad_acl_number = nlohmann::json::object();
        bad_acl_number["acl"] = 42;
        CHECK_FALSE(backup::metadata_from_json(bad_acl_number).has_value());

        nlohmann::json out_of_range_acl = nlohmann::json::object();
        out_of_range_acl["acl"] = nlohmann::json::array({1, 2, 300});
        CHECK_FALSE(backup::metadata_from_json(out_of_range_acl).has_value());
    }
}

// ===========================================================================
// 端到端：备份读出的元数据还原到新文件（B5 场景）
// ===========================================================================

TEST_CASE("metadata: 完整元数据还原到新文件", "[metadata][roundtrip]") {
    const fs::path base = fresh_dir("roundtrip");
    const fs::path source = make_file(base, "source.txt", "source content");
    const fs::path target = make_file(base, "target.txt", "target content");
    std::string error;

    // 先给源文件设置一套非默认元数据。
    Metadata source_meta;
    source_meta.mode = 0644;
    source_meta.win_attrs = kAttrHidden | kAttrArchive;
    source_meta.mtime_ns = kBaseNs;
    source_meta.atime_ns = kBaseNs - 3600LL * kSecondNs;
    source_meta.ctime_ns = kBaseNs - 7200LL * kSecondNs;
    REQUIRE(backup::apply_metadata(source, source_meta, error));
    const std::optional<Metadata> read_source =
        backup::read_metadata(source, error);
    REQUIRE(read_source.has_value());
    REQUIRE_FALSE(read_source->owner.empty());

    // 把它还原到另一个文件（属主/ACL 是 best-effort，可能带 warning）。
    const bool applied = backup::apply_metadata(target, *read_source, error);
    if (!applied) {
        WARN("完整元数据还原失败（权限不足）: " << error);
        cleanup(base);
        return;
    }
    const bool has_warning = !error.empty();
    if (has_warning) {
        WARN("部分字段降级为 warning: " << error);
    }

    const std::optional<Metadata> read_target =
        backup::read_metadata(target, error);
    REQUIRE(read_target.has_value());
    CHECK(near_ns(read_target->mtime_ns, source_meta.mtime_ns, 2 * kSecondNs));
    CHECK(near_ns(read_target->atime_ns, source_meta.atime_ns, 2 * kSecondNs));
    CHECK(near_ns(read_target->ctime_ns, source_meta.ctime_ns, 2 * kSecondNs));
    CHECK((read_target->win_attrs & kAttrHidden) != 0);
    CHECK((read_target->mode & 0200u) != 0);
    if (!has_warning) {
        CHECK(read_target->owner == read_source->owner);
        CHECK(read_target->acl == read_source->acl);
    }

    cleanup(base);
}

// ===========================================================================
// ACL 读回与应用
// ===========================================================================

TEST_CASE("metadata: ACL 可读并可 best-effort 应用", "[metadata][acl]") {
    const fs::path base = fresh_dir("acl");
    const fs::path source = make_file(base, "source.txt", "acl source");
    const fs::path target = make_file(base, "target.txt", "acl target");
    std::string error;

    const std::optional<Metadata> source_meta =
        backup::read_metadata(source, error);
    REQUIRE(source_meta.has_value());
    REQUIRE_FALSE(source_meta->acl.empty());
    CHECK(looks_like_self_relative_sd(source_meta->acl));

    // 只应用 ACL（不碰属主/时间/属性），避免权限不足干扰比较。
    Metadata acl_only;
    acl_only.acl = source_meta->acl;
    const bool applied = backup::apply_metadata(target, acl_only, error);
    if (!applied) {
        WARN("ACL 写入失败（权限不足），按 best-effort 跳过: " << error);
        cleanup(base);
        return;
    }
    const std::optional<Metadata> target_meta =
        backup::read_metadata(target, error);
    REQUIRE(target_meta.has_value());
    if (!error.empty()) {
        // 安全信息写入被系统拒绝，但整体还原仍成功（只记录 warning）。
        WARN("ACL 应用被系统降级: " << error);
    } else {
        CHECK(target_meta->acl == source_meta->acl);
    }

    cleanup(base);
}
