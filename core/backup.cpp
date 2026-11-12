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
///
/// 注意：覆盖靠 copy_options::overwrite_existing 完成，**不要**先 fs::remove(to)。
/// 早期实现用「先删目标再复制」，当 from 与 to 是同一个文件时（源目录 == 目标
/// 目录、或目标位于源内部）会把源文件本身删掉，随后 copy_file 报
/// no_such_file_or_directory，源文件就此永久丢失（不进回收站）。见同文件守卫。
bool copy_one(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code ec;

    // 同一个文件（硬链接、大小写不同写法、符号链接指向同处）直接跳过：
    // 既不可能有实际复制动作，也绝不能删/写。
    if (fs::exists(to, ec) && !ec) {
        ec.clear();
        if (fs::equivalent(from, to, ec) && !ec) return false;
    }

    ec.clear();
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("无法创建目录：" + to.parent_path().u8string() +
                                 "：" + ec.message());
    }

    const auto options = overwrite ? fs::copy_options::overwrite_existing
                                   : fs::copy_options::none;

    // 目标可能残留上一次备份/还原贴上的只读属性，覆盖前先清掉。
    if (overwrite) {
        std::error_code ro_ec;
        if (fs::exists(to, ro_ec) && !ro_ec) {
            std::string readonly_error;
            if (!clear_readonly(to, readonly_error)) {
                throw std::runtime_error("无法清除只读属性：" + to.u8string() +
                                         "：" + readonly_error);
            }
        }
    }

    ec.clear();
    fs::copy_file(from, to, options, ec);
    if (ec) {
        if (ec == std::errc::file_exists) return false;
        throw std::runtime_error("复制失败：" + from.u8string() + "：" +
                                 ec.message());
    }
    return true;
}

/// 返回规范化后的绝对路径；路径不存在也尽量解析（weakly_canonical）。
/// 失败时返回原路径，让调用方继续用未规范化的值做比较（保守但不会崩）。
fs::path normalized_or_self(const fs::path& path) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) return path;
    return resolved;
}

/// 判断 a 是否等于 b 或位于 b 之内（两者都应是已规范化的绝对路径）。
bool is_same_or_inside(const fs::path& a, const fs::path& b) {
    if (a == b) return true;
    auto it = b.begin();
    auto end = b.end();
    for (auto candidate = a.begin(); candidate != a.end(); ++candidate) {
        if (it == end) return true;  // b 已走完，说明 a 在 b 内部
        if (*candidate != *it) return false;
        ++it;
    }
    return it == end;  // 完全相等（已在上面处理过，这里兜底）
}

/// 校验源/目标目录不能相同、也不能互相嵌套，否则镜像会自我覆盖。
///
/// 相同：源文件与目标文件是同一个路径，复制等于原地重写（早期版本会先删后拷，
///       直接丢文件）。
/// 嵌套：目标在源内部时，上一次的复制结果会在下一次被当成源再复制一遍，目录
///       逐层膨胀；源在目标内部时同理。
void ensure_disjoint_directories(const fs::path& src, const fs::path& dst,
                                 const char* src_label, const char* dst_label) {
    const fs::path a = normalized_or_self(src);
    const fs::path b = normalized_or_self(dst);
    if (a == b) {
        throw std::runtime_error(std::string(src_label) + "与" + dst_label +
                                 "不能是同一个目录：" + a.u8string());
    }
    if (is_same_or_inside(b, a)) {
        throw std::runtime_error(std::string(dst_label) + "不能位于" + src_label +
                                 "内部：" + b.u8string());
    }
    if (is_same_or_inside(a, b)) {
        throw std::runtime_error(std::string(src_label) + "不能位于" + dst_label +
                                 "内部：" + a.u8string());
    }
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

    // 必须在校验之后、create_directories 之前做完：否则目标目录会被先建出来，
    // 之后再报错就留下了半个空目录。
    ensure_disjoint_directories(opts.src, opts.dst, "源目录", "目标目录");

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

    Progress progress;
    progress.entries_total = files.size();
    for (const FileEntry& entry : files) progress.bytes_total += entry.size;
    auto report_progress = [&](const FileEntry& entry) {
        if (!opts.progress) return;
        ++progress.entries_done;
        progress.bytes_done += entry.size;
        progress.current = entry.relative_path;
        try {
            opts.progress(progress);
        } catch (...) {
        }
    };

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
        report_progress(entry);
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

    // 同 mirror_directory：还原时目标落在备份目录里会把备份本身覆盖掉。
    ensure_disjoint_directories(opts.src, opts.dst, "备份目录", "还原目录");

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
    const std::vector<FileEntry> files = list_files(opts.src);

    Progress progress;
    progress.entries_total = files.size();
    for (const FileEntry& entry : files) progress.bytes_total += entry.size;
    auto report_progress = [&](const FileEntry& entry) {
        if (!opts.progress) return;
        ++progress.entries_done;
        progress.bytes_done += entry.size;
        progress.current = entry.relative_path;
        try {
            opts.progress(progress);
        } catch (...) {
        }
    };

    for (const FileEntry& entry : files) {
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
        report_progress(entry);
    }

    return result;
}

}  // namespace backup
