#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "types.h"

namespace backup {

/// 过滤规则求值时需要的条目信息。调用方在扫描阶段填好。
struct FilterContext {
    std::string relative_path;  ///< 相对根目录的路径，UTF-8，'/' 分隔
    std::string name;           ///< 文件名（不含目录）
    std::uintmax_t size = 0;
    std::int64_t mtime_ns = 0;  ///< Unix epoch 纳秒
    EntryType type = EntryType::File;
    std::uint32_t uid = 0;
    std::string owner;          ///< 属主账户名（UTF-8）
};

/// 六维筛选引擎。
///
/// 规则以 JSON 数组下发（README §6「筛选规则」）：
///
///   [
///     {"action": "include", "path": "docs/**", "name": "*.pdf"},
///     {"action": "exclude", "type": "tmp,bak"},
///     {"action": "include", "size": "< 100MB"},
///     {"action": "include", "time": "last 7 days"},
///     {"action": "include", "user": "owner:alice"}
///   ]
///
/// 语义：
///   - 一条规则里出现的多个维度之间是 AND；
///   - 同一维度内逗号分隔的多个值是 OR；
///   - 先求 include：没有任何 include 规则时默认包含，否则命中任一
///     include 规则即进入候选；
///   - 再求 exclude：命中任一 exclude 规则即排除。
///
/// 支持的写法：
///   path   glob（`*`、`?`、`**`），如 `docs/**`、`src/*.cpp`
///   type   扩展名列表 `jpg,png`，或分类 `category:image`
///   name   文件名 glob，如 `report_*.pdf`
///   time   `last 7 days` / `before 2025-01-01` / `after 2025-01-01`
///          或 `2025-01-01..2025-06-30`
///   size   `< 100MB`、`>= 1KB`、`1KB..10MB`
///   user   `uid:1000`、`owner:alice`、或直接写 `alice`
class Filter {
public:
    Filter();
    Filter(const Filter&);
    Filter(Filter&&) noexcept;
    Filter& operator=(const Filter&);
    Filter& operator=(Filter&&) noexcept;
    ~Filter();

    /// 解析 JSON；失败返回 nullopt，error 里带出错规则序号与原因。
    static std::optional<Filter> from_json(const std::string& json_text,
                                           std::string& error);
    static std::optional<Filter> from_json(const nlohmann::json& rules,
                                           std::string& error);

    nlohmann::json to_json() const;

    /// 没有任何规则（或所有规则为空）时恒为 true。
    bool empty() const;

    bool match(const FilterContext& ctx) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

}  // namespace backup
