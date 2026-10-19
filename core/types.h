#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace backup {

/// 字节缓冲的统一别名。压缩/加密/元数据都用它，避免到处写 vector<uint8_t>。
using Bytes = std::vector<std::uint8_t>;

/// 归档条目的类型。数值直接写进 VBAK 索引，**不要改**。
enum class EntryType : std::uint8_t {
    File = 0,
    Directory = 1,
    Symlink = 2,
    Hardlink = 3,
    Fifo = 4,
    Device = 5,
    Socket = 6,
    Other = 7,
};

inline bool is_link(EntryType t) {
    return t == EntryType::Symlink || t == EntryType::Hardlink;
}

const char* to_string(EntryType type);

}  // namespace backup
