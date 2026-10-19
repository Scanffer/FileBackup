#include "filter.h"

namespace backup {

// 由 filter/meta agent 实现。骨架阶段：拒绝解析、恒返回 true。

struct Filter::Impl {
    nlohmann::json rules = nlohmann::json::array();
};

Filter::Filter() : impl_(std::make_shared<const Impl>()) {}
Filter::Filter(const Filter&) = default;
Filter::Filter(Filter&&) noexcept = default;
Filter& Filter::operator=(const Filter&) = default;
Filter& Filter::operator=(Filter&&) noexcept = default;
Filter::~Filter() = default;

std::optional<Filter> Filter::from_json(const std::string& /*json_text*/,
                                        std::string& error) {
    error = "筛选引擎尚未实现";
    return std::nullopt;
}

std::optional<Filter> Filter::from_json(const nlohmann::json& /*rules*/,
                                        std::string& error) {
    error = "筛选引擎尚未实现";
    return std::nullopt;
}

nlohmann::json Filter::to_json() const {
    return impl_ ? impl_->rules : nlohmann::json::array();
}

bool Filter::empty() const { return true; }

bool Filter::match(const FilterContext& /*ctx*/) const { return true; }

}  // namespace backup
