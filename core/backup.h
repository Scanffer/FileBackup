#pragma once

#include <cstdint>
#include <filesystem>

#include "filter.h"
#include "progress.h"

namespace backup {

struct MirrorOptions {
    std::filesystem::path src;   ///< 源目录（宽字符构造，别用窄串）
    std::filesystem::path dst;   ///< 目标目录
    bool overwrite = true;       ///< 目标已存在时是否覆盖

    Filter filter;               ///< 六维筛选；空表示全选
    bool keep_metadata = true;   ///< 写 manifest 并保存元数据
    ProgressFn progress;         ///< 可选进度回调
};

struct MirrorResult {
    std::uintmax_t files_copied = 0;
    std::uintmax_t bytes_copied = 0;
    std::uintmax_t files_skipped = 0;  ///< 已存在且 overwrite=false
    std::uintmax_t entries_filtered = 0;  ///< 被筛选排除的条目数
};

/// 目录镜像：把 src 整棵树复制到 dst。
/// keep_metadata=true 时额外写入 `.backup-manifest.json`（含逐文件元数据），
/// 供 restore_mirror 还原权限/时间戳/ACL。
/// 失败抛 std::runtime_error（UTF-8 消息），CLI 负责打印和映射退出码。
MirrorResult mirror_directory(const MirrorOptions& opts);

struct MirrorRestoreOptions {
    std::filesystem::path src;   ///< 备份根目录（含 manifest）
    std::filesystem::path dst;   ///< 还原目标
    bool overwrite = true;
    bool restore_metadata = true;  ///< 读取 manifest 并还原元数据
    ProgressFn progress;           ///< 可选进度回调
};

/// 目录镜像还原：复制文件树；若存在 manifest 且 restore_metadata=true，
/// 按 manifest 还原元数据（失败降级为忽略，不中断整体还原）。
MirrorResult restore_mirror(const MirrorRestoreOptions& opts);

}  // namespace backup
