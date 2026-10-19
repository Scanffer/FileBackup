#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "algorithm.h"
#include "filter.h"
#include "metadata.h"
#include "progress.h"
#include "types.h"

namespace backup {

/// 目录镜像模式使用的 manifest 文件名（放在备份根目录下）。
extern const char* const kManifestFileName;

struct ArchiveOptions {
    std::filesystem::path source;
    /// 目录镜像模式：目标目录；单容器模式：归档文件路径。
    std::filesystem::path destination;
    bool container = false;  ///< true = 写单个 .vbak；false = 目录镜像

    PackId pack = PackId::Vbak;
    CompressionId compression = CompressionId::None;
    int compression_level = 3;
    EncryptionId encryption = EncryptionId::None;
    std::string password;  ///< encryption != None 时必填

    Filter filter;         ///< 空表示不筛选
    bool keep_metadata = true;
    bool overwrite = true;
};

struct BackupResult {
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path output;
};

struct RestoreOptions {
    /// 单容器：归档文件路径；目录镜像：备份根目录。
    std::filesystem::path source;
    std::filesystem::path destination;
    bool container = false;
    std::string password;
    std::optional<std::string> entry;  ///< 只还原单个条目（仅单容器）
    bool overwrite = true;
    bool restore_metadata = true;
};

struct RestoreResult {
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
};

struct EntryInfo {
    std::string path;
    EntryType type = EntryType::File;
    std::uint64_t original_size = 0;
    std::uint64_t stored_size = 0;
    std::uint32_t crc32 = 0;
    std::string link_target;
    bool has_metadata = false;
};

struct ArchiveInfo {
    std::uint16_t version = 0;
    PackId pack = PackId::Vbak;
    CompressionId compression = CompressionId::None;
    EncryptionId encryption = EncryptionId::None;
    std::uint64_t entry_count = 0;
    std::uint64_t archive_size = 0;
    std::vector<EntryInfo> entries;
};

/// 备份。失败返回 nullopt 并填 error（UTF-8）。
std::optional<BackupResult> backup(const ArchiveOptions& options,
                                   const ProgressFn& progress,
                                   std::string& error);

/// 还原。失败返回 nullopt 并填 error（UTF-8）。
std::optional<RestoreResult> restore(const RestoreOptions& options,
                                     const ProgressFn& progress,
                                     std::string& error);

/// 列出内容。container=false 时读取目录镜像里的 manifest。
std::optional<ArchiveInfo> list_archive(const std::filesystem::path& archive,
                                        bool container, std::string& error);

/// 校验完整性。目录镜像模式校验 manifest 与文件大小/CRC。
std::optional<ArchiveInfo> verify_archive(const std::filesystem::path& archive,
                                          bool container,
                                          const std::string& password,
                                          std::string& error);

}  // namespace backup
