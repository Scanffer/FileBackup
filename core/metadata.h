#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "types.h"

namespace backup {

/// 条目元数据（对应 README §5 的 MetaBlock）。
///
/// 跨平台字段尽量用 Unix 语义表示；Windows 专有字段（文件属性、安全
/// 描述符）单独存放，非 Windows 下为空。
struct Metadata {
    std::uint32_t mode = 0;  ///< Unix 权限位；Windows 下由只读属性映射
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::string owner;  ///< 属主账户名（UTF-8）
    std::string group;

    std::int64_t mtime_ns = 0;  ///< 修改时间，Unix epoch 纳秒
    std::int64_t atime_ns = 0;  ///< 访问时间
    std::int64_t ctime_ns = 0;  ///< 创建时间

    std::uint32_t win_attrs = 0;  ///< FILE_ATTRIBUTE_* 位
    Bytes acl;                    ///< Windows 自相对安全描述符原始字节
};

/// 读取条目的元数据。路径不存在 / 无权读取返回 nullopt。
std::optional<Metadata> read_metadata(const std::filesystem::path& path,
                                      std::string& error);

/// 把元数据应用到条目（还原用）。不支持的字段跳过；整体失败返回 false。
bool apply_metadata(const std::filesystem::path& path, const Metadata& meta,
                    std::string& error);

/// 二进制序列化（写进 VBAK 每条的 MetaBlock）。格式见 metadata.cpp。
Bytes metadata_to_bytes(const Metadata& meta);
std::optional<Metadata> metadata_from_bytes(const Bytes& bytes,
                                            std::string& error);

/// JSON 表示（目录镜像模式的 manifest 用）。ACL 以 Base64 存放。
nlohmann::json metadata_to_json(const Metadata& meta);
std::optional<Metadata> metadata_from_json(const nlohmann::json& json);

/// 过滤规则「用户」维度需要的属主名；取不到返回空串。
std::string file_owner_name(const std::filesystem::path& path);

}  // namespace backup
