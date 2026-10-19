#include "archive.h"

namespace backup {

// 由 archive agent 实现（VBAK v2 + 目录镜像 manifest）。
// 骨架阶段：所有操作返回「尚未实现」，保证基线构建通过。

const char* const kManifestFileName = ".backup-manifest.json";

const char* to_string(PackId id) {
    switch (id) {
        case PackId::Vbak: return "vbak";
        case PackId::Ustar: return "ustar";
    }
    return "vbak";
}

std::optional<PackId> pack_from_string(const std::string& s) {
    if (s == "vbak") return PackId::Vbak;
    if (s == "ustar") return PackId::Ustar;
    return std::nullopt;
}

const std::vector<AlgorithmInfo>& pack_algorithms() {
    static const std::vector<AlgorithmInfo> list = {
        {"vbak", "VBAK 索引格式", false, true,
         "自研单容器格式，支持随机访问与逐条目算法"},
    };
    return list;
}

std::optional<BackupResult> backup(const ArchiveOptions& /*options*/,
                                   const ProgressFn& /*progress*/,
                                   std::string& error) {
    error = "备份引擎尚未实现";
    return std::nullopt;
}

std::optional<RestoreResult> restore(const RestoreOptions& /*options*/,
                                     const ProgressFn& /*progress*/,
                                     std::string& error) {
    error = "还原引擎尚未实现";
    return std::nullopt;
}

std::optional<ArchiveInfo> list_archive(const std::filesystem::path& /*archive*/,
                                        bool /*container*/,
                                        std::string& error) {
    error = "归档列表尚未实现";
    return std::nullopt;
}

std::optional<ArchiveInfo> verify_archive(const std::filesystem::path& /*archive*/,
                                          bool /*container*/,
                                          const std::string& /*password*/,
                                          std::string& error) {
    error = "归档校验尚未实现";
    return std::nullopt;
}

}  // namespace backup
