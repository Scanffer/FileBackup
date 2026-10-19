#include "metadata.h"

namespace backup {

// 由 filter/meta agent 实现（Windows 权限、时间戳、属性、ACL 读写）。
// 骨架阶段返回空/失败，保证基线构建通过。

std::optional<Metadata> read_metadata(const std::filesystem::path& /*path*/,
                                      std::string& error) {
    error = "元数据读取尚未实现";
    return std::nullopt;
}

bool apply_metadata(const std::filesystem::path& /*path*/,
                    const Metadata& /*meta*/, std::string& error) {
    error = "元数据还原尚未实现";
    return false;
}

Bytes metadata_to_bytes(const Metadata& /*meta*/) { return {}; }

std::optional<Metadata> metadata_from_bytes(const Bytes& /*bytes*/,
                                            std::string& error) {
    error = "元数据反序列化尚未实现";
    return std::nullopt;
}

nlohmann::json metadata_to_json(const Metadata& /*meta*/) {
    return nlohmann::json::object();
}

std::optional<Metadata> metadata_from_json(const nlohmann::json& /*json*/) {
    return std::nullopt;
}

std::string file_owner_name(const std::filesystem::path& /*path*/) {
    return {};
}

}  // namespace backup
