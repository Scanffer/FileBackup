#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace backup {

/// 单调进度回调。备份/还原每处理完一个条目调用一次。
struct Progress {
    std::uint64_t entries_done = 0;
    std::uint64_t entries_total = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    std::string current;  ///< 当前/刚处理完的相对路径（UTF-8）
};

using ProgressFn = std::function<void(const Progress&)>;

}  // namespace backup
