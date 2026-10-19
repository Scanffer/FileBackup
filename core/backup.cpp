#include "backup.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "algorithm.h"
#include "archive.h"
#include "fs_utils.h"
#include "metadata.h"

namespace backup {
namespace {

namespace fs = std::filesystem;

/// std::filesystem::file_clock 的纪元没有标准转换；用当前时刻求偏移。
std::int64_t file_mtime_ns(const fs::path& path) {
    std::error_code ec;
    const auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;

    using namespace std::chrono;
    const auto file_now = fs::file_time_type::clock::now();
    const auto sys_now = system_clock::now();
    const auto offset =
        sys_now.time_since_epoch() -
        duration_cast<system_clock::duration>(file_now.time_since_epoch());
    const auto since_epoch =
        duration_cast<system_clock::duration>(ft.time_since_epoch()) + offset;
    return duration_cast<nanoseconds>(since_epoch).count();
}

std::string filename_u8(const std::string& relative_path) {
    return path_from_u8(relative_path).filename().generic_u8string();
}

bool filter_uses_user(const Filter& filter) {
    if (filter.empty()) return false;
    const nlohmann::json rules = filter.to_json();
    if (!rules.is_array()) return false;
    for (const auto& rule : rules) {
        if (rule.is_object() && rule.contains("user")) return true;
    }
    return false;
}

/// 应用筛选并返回保留的文件；entries_filtered = 被排除数量。
std::vector<FileEntry> apply_filter(const std::vector<FileEntry>& files,
                                    const fs::path& src, const Filter& filter,
                                    std::uintmax_t& entries_filtered) {
    entries_filtered = 0;
    if (filter.empty()) return files;

    const bool needs_owner = filter_uses_user(filter);
    std::vector<FileEntry> kept;
    kept.reserve(files.size());
    for (const FileEntry& entry : files) {
        const fs::path from = src / path_from_u8(entry.relative_path);
        FilterContext ctx;
        ctx.relative_path = entry.relative_path;
        ctx.name = filename_u8(entry.relative_path);
        ctx.size = entry.size;
        ctx.mtime_ns = file_mtime_ns(from);
        ctx.type = EntryType::File;
        ctx.uid = 0;
        if (needs_owner) ctx.owner = file_owner_name(from);
        if (filter.match(ctx)) {
            kept.push_back(entry);
        } else {
            ++entries_filtered;
        }
    }
    return kept;
}

/// 复制一个文件，返回 true=复制，false=跳过；出错抛异常。
bool copy_one(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("无法创建目录：" + to.parent_path().u8string() +
                                 "：" + ec.message());
    }

    if (overwrite) {
        std::error_code ignore;
        fs::remove(to, ignore);
    }

    ec.clear();
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) {
        if (ec == std::errc::file_exists) return false;
        throw std::runtime_error("复制失败：" + from.u8string() + "：" +
                                 ec.message());
    }
    return true;
}

nlohmann::json read_manifest(const fs::path& src) {
    std::ifstream in(src / kManifestFileName, std::ios::binary);
    if (!in) return nlohmann::json();
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return nlohmann::json();
    }
}

}  // namespace

// ------------------------------------------------------------------
// 目录镜像备份
// ------------------------------------------------------------------

MirrorResult mirror_directory(const MirrorOptions& opts) {
    std::error_code ec;

    if (!fs::is_directory(opts.src, ec)) {
        throw std::runtime_error("源目录不存在或不是目录");
    }

    fs::create_directories(opts.dst, ec);
    if (ec) {
        throw std::runtime_error("无法创建目标目录：" + ec.message());
    }

    std::uintmax_t filtered = 0;
    const std::vector<FileEntry> files =
        apply_filter(list_files(opts.src), opts.src, opts.filter, filtered);

    nlohmann::json manifest;
    if (opts.keep_metadata) {
        manifest = nlohmann::json::object();
        manifest["version"] = 1;
        manifest["created_ns"] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        manifest["source"] = opts.src.generic_u8string();
        manifest["filtered"] = !opts.filter.empty();
        manifest["entries"] = nlohmann::json::array();
    }

    MirrorResult result;
    result.entries_filtered = filtered;

    for (const FileEntry& entry : files) {
        const fs::path rel = path_from_u8(entry.relative_path);
        const fs::path from = opts.src / rel;
        const fs::path to = opts.dst / rel;

        if (opts.keep_metadata) {
            std::string meta_error;
            const auto meta = read_metadata(from, meta_error);
            if (meta) {
                nlohmann::json item;
                item["path"] = entry.relative_path;
                item["type"] = to_string(entry.type);
                item["size"] = static_cast<std::uint64_t>(entry.size);
                item["mtime_ns"] = file_mtime_ns(from);
                item["metadata"] = metadata_to_json(*meta);
                manifest["entries"].push_back(std::move(item));
            }
        }

        if (copy_one(from, to, opts.overwrite)) {
            ++result.files_copied;
            result.bytes_copied += entry.size;
        } else {
            ++result.files_skipped;
        }
    }

    if (opts.keep_metadata) {
        std::ofstream out(opts.dst / kManifestFileName,
                          std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 manifest");
        out << manifest.dump(2);
        if (!out) throw std::runtime_error("写 manifest 失败");
    }

    return result;
}

// ------------------------------------------------------------------
// 目录镜像还原
// ------------------------------------------------------------------

MirrorResult restore_mirror(const MirrorRestoreOptions& opts) {
    std::error_code ec;

    if (!fs::is_directory(opts.src, ec)) {
        throw std::runtime_error("备份目录不存在或不是目录");
    }
    fs::create_directories(opts.dst, ec);
    if (ec) {
        throw std::runtime_error("无法创建目标目录：" + ec.message());
    }

    // path -> metadata JSON
    std::unordered_map<std::string, nlohmann::json> meta_by_path;
    if (opts.restore_metadata) {
        const nlohmann::json manifest = read_manifest(opts.src);
        if (manifest.is_object() && manifest.contains("entries") &&
            manifest["entries"].is_array()) {
            for (const auto& item : manifest["entries"]) {
                if (!item.is_object() || !item.contains("path")) continue;
                if (!item.contains("metadata")) continue;
                meta_by_path[item["path"].get<std::string>()] =
                    item["metadata"];
            }
        }
    }

    MirrorResult result;
    for (const FileEntry& entry : list_files(opts.src)) {
        if (entry.relative_path == kManifestFileName) continue;

        const fs::path rel = path_from_u8(entry.relative_path);
        const fs::path from = opts.src / rel;
        const fs::path to = opts.dst / rel;

        if (copy_one(from, to, opts.overwrite)) {
            ++result.files_copied;
            result.bytes_copied += entry.size;
        } else {
            ++result.files_skipped;
        }

        if (opts.restore_metadata) {
            const auto it = meta_by_path.find(entry.relative_path);
            if (it != meta_by_path.end()) {
                const auto meta = metadata_from_json(it->second);
                if (meta) {
                    std::string apply_error;
                    // ACL/属主跨机可能失败：降级忽略，不中断还原。
                    (void)apply_metadata(to, *meta, apply_error);
                }
            }
        }
    }

    return result;
}

}  // namespace backup
