#pragma once

#include <cstdint>
#include <filesystem>

namespace backup {

struct MirrorOptions {
    std::filesystem::path src;   ///< 源目录（宽字符构造，别用窄串）
    std::filesystem::path dst;   ///< 目标目录
    bool overwrite = true;       ///< 目标已存在时是否覆盖
};

struct MirrorResult {
    std::uintmax_t files_copied = 0;
    std::uintmax_t bytes_copied = 0;
    std::uintmax_t files_skipped = 0;  ///< 已存在且 overwrite=false
};

/// 目录镜像：把 src 整棵树复制到 dst。
/// 失败抛 std::runtime_error（UTF-8 消息），CLI 负责打印和映射退出码。
/// 目录不存在、src 不是目录等前置错误也走异常。
MirrorResult mirror_directory(const MirrorOptions& opts);

}  // namespace backup