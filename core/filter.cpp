#include "filter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace backup {
namespace {

using nlohmann::json;

// ===========================================================================
// 字符串小工具
// ===========================================================================

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

std::string trim_copy(std::string text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower_ascii(a) == lower_ascii(b);
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 按逗号拆分（保留空段由调用方丢弃）。用于「同维度多值 OR」。
std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = text.find(',', begin);
        parts.push_back(trim_copy(text.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin)));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return parts;
}

// ===========================================================================
// glob 匹配：`*`（不跨 '/'）、`?`（单个非 '/' 字符）、`**`（可跨 '/'）
// ===========================================================================

bool glob_match_rec(const std::string& pattern, std::size_t pi,
                    const std::string& text, std::size_t si,
                    std::vector<signed char>& memo) {
    const std::size_t pattern_size = pattern.size();
    const std::size_t text_size = text.size();
    signed char& slot = memo[pi * (text_size + 1) + si];
    if (slot >= 0) return slot != 0;

    bool matched = false;
    if (pi == pattern_size) {
        matched = (si == text_size);
    } else if (pattern[pi] == '*') {
        if (pi + 1 < pattern_size && pattern[pi + 1] == '*') {
            if (pi + 2 < pattern_size && pattern[pi + 2] == '/') {
                // `**/`：匹配零个或多个完整的目录段，只能在段边界退出。
                const std::size_t next = pi + 3;
                if (si == 0 || text[si - 1] == '/') {
                    matched = glob_match_rec(pattern, next, text, si, memo);
                }
                if (!matched && si < text_size) {
                    matched = glob_match_rec(pattern, pi, text, si + 1, memo);
                }
            } else if (pi + 2 == pattern_size) {
                // 结尾的 `**`：吃掉剩余所有字符（含 '/'）。
                matched = true;
            } else {
                matched = glob_match_rec(pattern, pi + 2, text, si, memo);
                if (!matched && si < text_size) {
                    matched = glob_match_rec(pattern, pi, text, si + 1, memo);
                }
            }
        } else {
            // 单个 `*`：匹配任意个非 '/' 字符。
            matched = glob_match_rec(pattern, pi + 1, text, si, memo);
            if (!matched && si < text_size && text[si] != '/') {
                matched = glob_match_rec(pattern, pi, text, si + 1, memo);
            }
        }
    } else if (pattern[pi] == '?') {
        matched = si < text_size && text[si] != '/' &&
                  glob_match_rec(pattern, pi + 1, text, si + 1, memo);
    } else {
        matched = si < text_size && text[si] == pattern[pi] &&
                  glob_match_rec(pattern, pi + 1, text, si + 1, memo);
    }

    slot = matched ? 1 : 0;
    return matched;
}

bool glob_match(const std::string& pattern_lower, const std::string& text_lower) {
    std::vector<signed char> memo(
        (pattern_lower.size() + 1) * (text_lower.size() + 1),
        static_cast<signed char>(-1));
    return glob_match_rec(pattern_lower, 0, text_lower, 0, memo);
}

/// 编译后的 glob 模式（小写比较，Windows 语义大小写不敏感）。
class Glob {
public:
    Glob() = default;

    /// literal_dir_prefix：纯字面量模式（无通配符）同时按目录前缀匹配其下条目，
    /// 便于 `{"path":"docs"}` 包含 docs 下所有文件。
    Glob(std::string pattern, bool literal_dir_prefix)
        : pattern_(std::move(pattern)),
          lower_pattern_(lower_ascii(pattern_)),
          trailing_double_star_(ends_with(lower_pattern_, "/**") &&
                                lower_pattern_.size() > 3) {
        literal_dir_prefix_ =
            literal_dir_prefix && pattern_.find_first_of("*?") == std::string::npos;
    }

    bool matches(const std::string& text) const {
        if (lower_pattern_ == "**") return true;
        const std::string lower_text = lower_ascii(text);
        // `docs/**` 也命中目录本身 `docs`，这样目录条目不会被筛掉。
        if (trailing_double_star_) {
            const std::string base = lower_pattern_.substr(0, lower_pattern_.size() - 3);
            if (!base.empty() && glob_match(base, lower_text)) return true;
        }
        if (glob_match(lower_pattern_, lower_text)) return true;
        if (literal_dir_prefix_ && !lower_pattern_.empty() &&
            lower_text.size() > lower_pattern_.size() &&
            lower_text.compare(0, lower_pattern_.size(), lower_pattern_) == 0 &&
            lower_text[lower_pattern_.size()] == '/') {
            return true;
        }
        return false;
    }

    const std::string& pattern() const { return pattern_; }

private:
    std::string pattern_;
    std::string lower_pattern_;
    bool literal_dir_prefix_ = false;
    bool trailing_double_star_ = false;
};

/// 相对路径归一化：'\\' -> '/'、去掉开头 "./" 与 '/'、合并重复 '/'、去掉结尾 '/'。
std::string normalize_relative_path(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    std::string result;
    result.reserve(path.size());
    for (char c : path) {
        if (c == '/' && !result.empty() && result.back() == '/') continue;
        result.push_back(c);
    }
    while (starts_with(result, "./")) result.erase(0, 2);
    while (!result.empty() && result.front() == '/') result.erase(0, 1);
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

std::string base_name_of(const std::string& relative_path) {
    if (relative_path.empty()) return {};
    const std::size_t slash = relative_path.find_last_of('/');
    return slash == std::string::npos ? relative_path
                                      : relative_path.substr(slash + 1);
}

/// 取扩展名（不含点，小写）。无扩展名或隐藏文件返回空串。
std::string extension_of(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return lower_ascii(name.substr(dot + 1));
}

// ===========================================================================
// 时间表达式
// ===========================================================================

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kNanosPerMinute = 60LL * kNanosPerSecond;
constexpr std::int64_t kNanosPerHour = 60LL * kNanosPerMinute;
constexpr std::int64_t kNanosPerDay = 24LL * kNanosPerHour;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool parse_uint64_strict(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

int days_in_month(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

/// 严格解析 `YYYY-MM-DD` 或 `YYYY/MM/DD` 为本地时区当天 00:00 的 Unix 纳秒。
bool parse_date_start_ns(const std::string& raw, std::int64_t& out_ns) {
    const std::string text = trim_copy(raw);
    if (text.size() != 10) return false;
    if ((text[4] != '-' && text[4] != '/') ||
        (text[7] != '-' && text[7] != '/')) {
        return false;
    }
    auto parse_digits = [&text](std::size_t begin, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[begin + i];
            if (c < '0' || c > '9') return -1;
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const int year = parse_digits(0, 4);
    const int month = parse_digits(5, 2);
    const int day = parse_digits(8, 2);
    // Windows 的 mktime 只保证 1970 年以后可表示，这里限制在实用范围内。
    if (year < 1970 || year > 2200) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > days_in_month(year, month)) return false;

    std::tm tm_value{};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = 0;
    tm_value.tm_min = 0;
    tm_value.tm_sec = 0;
    tm_value.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&tm_value);
    if (seconds == static_cast<std::time_t>(-1)) return false;
    out_ns = static_cast<std::int64_t>(seconds) * kNanosPerSecond;
    return true;
}

bool parse_span_unit(const std::string& raw_unit, std::int64_t& out_ns) {
    const std::string unit = lower_ascii(trim_copy(raw_unit));
    if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" ||
        unit == "seconds") {
        out_ns = kNanosPerSecond;
        return true;
    }
    if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" ||
        unit == "minutes") {
        out_ns = kNanosPerMinute;
        return true;
    }
    if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" ||
        unit == "hours") {
        out_ns = kNanosPerHour;
        return true;
    }
    if (unit == "d" || unit == "day" || unit == "days") {
        out_ns = kNanosPerDay;
        return true;
    }
    if (unit == "w" || unit == "week" || unit == "weeks") {
        out_ns = 7LL * kNanosPerDay;
        return true;
    }
    return false;
}

struct TimePredicate {
    enum class Kind { Last, Before, After, Between };

    Kind kind = Kind::Last;
    std::int64_t span_ns = 0;  ///< Kind::Last：最近 span_ns 纳秒
    std::int64_t lo_ns = 0;    ///< Kind::After / Kind::Between：包含下界
    std::int64_t hi_ns = 0;    ///< Kind::Before / Kind::Between：不含上界
    std::string text;          ///< 归一化后的原始表达式，供 to_json 往返

    bool matches(std::int64_t mtime_ns, std::int64_t now) const {
        switch (kind) {
            case Kind::Last:
                return mtime_ns >= now - span_ns;
            case Kind::Before:
                return mtime_ns < hi_ns;
            case Kind::After:
                return mtime_ns >= lo_ns;
            case Kind::Between:
                return mtime_ns >= lo_ns && mtime_ns < hi_ns;
        }
        return false;
    }
};

bool parse_time_predicate(const std::string& raw, TimePredicate& out,
                          std::string& reason) {
    const std::string text = trim_copy(raw);
    if (text.empty()) {
        reason = "时间表达式为空";
        return false;
    }
    const std::string lower = lower_ascii(text);

    if (starts_with(lower, "last ")) {
        const std::string rest = trim_copy(text.substr(5));
        std::size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
            ++digits;
        }
        if (digits == 0) {
            reason = "last 后缺少天数，例如 \"last 7 days\"";
            return false;
        }
        std::uint64_t count = 0;
        if (!parse_uint64_strict(rest.substr(0, digits), count) || count > 1000000) {
            reason = "last 后的数量无效或过大";
            return false;
        }
        std::int64_t unit_ns = kNanosPerDay;
        const std::string unit = trim_copy(rest.substr(digits));
        if (!unit.empty() && !parse_span_unit(unit, unit_ns)) {
            reason = "不支持的时间单位 \"" + unit + "\"";
            return false;
        }
        out.kind = TimePredicate::Kind::Last;
        out.span_ns = static_cast<std::int64_t>(count) * unit_ns;
        out.text = text;
        return true;
    }

    const std::size_t range_pos = text.find("..");
    if (range_pos != std::string::npos) {
        const std::string left = trim_copy(text.substr(0, range_pos));
        const std::string right = trim_copy(text.substr(range_pos + 2));
        std::int64_t lo = 0;
        std::int64_t hi = 0;
        if (!parse_date_start_ns(left, lo)) {
            reason = "无法解析起始日期 \"" + left + "\"（应为 YYYY-MM-DD）";
            return false;
        }
        if (!parse_date_start_ns(right, hi)) {
            reason = "无法解析结束日期 \"" + right + "\"（应为 YYYY-MM-DD）";
            return false;
        }
        if (hi < lo) {
            reason = "时间区间起点晚于终点";
            return false;
        }
        out.kind = TimePredicate::Kind::Between;
        out.lo_ns = lo;
        out.hi_ns = hi + kNanosPerDay;  // 结束日期当天也算在区间内
        out.text = text;
        return true;
    }

    if (starts_with(lower, "before ")) {
        const std::string value = trim_copy(text.substr(7));
        std::int64_t day = 0;
        if (!parse_date_start_ns(value, day)) {
            reason = "无法解析日期 \"" + value + "\"（应为 YYYY-MM-DD）";
            return false;
        }
        out.kind = TimePredicate::Kind::Before;
        out.hi_ns = day;
        out.text = text;
        return true;
    }
    if (starts_with(lower, "after ")) {
        const std::string value = trim_copy(text.substr(6));
        std::int64_t day = 0;
        if (!parse_date_start_ns(value, day)) {
            reason = "无法解析日期 \"" + value + "\"（应为 YYYY-MM-DD）";
            return false;
        }
        out.kind = TimePredicate::Kind::After;
        out.lo_ns = day;
        out.text = text;
        return true;
    }

    // 只写一个日期：匹配当天整天。
    std::int64_t day = 0;
    if (parse_date_start_ns(text, day)) {
        out.kind = TimePredicate::Kind::Between;
        out.lo_ns = day;
        out.hi_ns = day + kNanosPerDay;
        out.text = text;
        return true;
    }

    reason = "无法识别的时间表达式 \"" + text +
             "\"（支持 last N days / before|after YYYY-MM-DD / "
             "YYYY-MM-DD..YYYY-MM-DD）";
    return false;
}

// ===========================================================================
// 尺寸表达式
// ===========================================================================

struct SizePredicate {
    enum class Kind { Lt, Le, Gt, Ge, Eq, Between };

    Kind kind = Kind::Eq;
    std::uint64_t a = 0;  ///< 比较值 / 区间下界（含）
    std::uint64_t b = 0;  ///< 区间上界（含）
    std::string text;     ///< 归一化后的原始表达式

    bool matches(std::uintmax_t size) const {
        const std::uint64_t value = static_cast<std::uint64_t>(size);
        switch (kind) {
            case Kind::Lt:
                return value < a;
            case Kind::Le:
                return value <= a;
            case Kind::Gt:
                return value > a;
            case Kind::Ge:
                return value >= a;
            case Kind::Eq:
                return value == a;
            case Kind::Between:
                return value >= a && value <= b;
        }
        return false;
    }
};

bool parse_size_value(const std::string& raw, std::uint64_t& out_bytes,
                      std::string& reason) {
    const std::string text = lower_ascii(trim_copy(raw));
    if (text.empty()) {
        reason = "尺寸值为空";
        return false;
    }
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        reason = "尺寸 \"" + raw + "\" 缺少数字";
        return false;
    }
    std::uint64_t number = 0;
    if (!parse_uint64_strict(text.substr(0, digits), number)) {
        reason = "尺寸 \"" + raw + "\" 数字过大";
        return false;
    }
    const std::string unit = trim_copy(text.substr(digits));
    std::uint64_t multiplier = 1;
    if (unit.empty() || unit == "b" || unit == "byte" || unit == "bytes") {
        multiplier = 1;
    } else if (unit == "k" || unit == "kb" || unit == "kib") {
        multiplier = 1024ULL;
    } else if (unit == "m" || unit == "mb" || unit == "mib") {
        multiplier = 1024ULL * 1024ULL;
    } else if (unit == "g" || unit == "gb" || unit == "gib") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (unit == "t" || unit == "tb" || unit == "tib") {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    } else {
        reason = "不支持的尺寸单位 \"" + unit + "\"";
        return false;
    }
    if (number > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        reason = "尺寸 \"" + raw + "\" 数值过大";
        return false;
    }
    out_bytes = number * multiplier;
    return true;
}

bool parse_size_predicate(const std::string& raw, SizePredicate& out,
                          std::string& reason) {
    const std::string text = trim_copy(raw);
    if (text.empty()) {
        reason = "尺寸表达式为空";
        return false;
    }
    const std::size_t range_pos = text.find("..");
    if (range_pos != std::string::npos) {
        const std::string left = trim_copy(text.substr(0, range_pos));
        const std::string right = trim_copy(text.substr(range_pos + 2));
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        if (!parse_size_value(left, lo, reason)) {
            reason = "区间下界无效: " + reason;
            return false;
        }
        if (!parse_size_value(right, hi, reason)) {
            reason = "区间上界无效: " + reason;
            return false;
        }
        if (hi < lo) {
            reason = "尺寸区间下界大于上界";
            return false;
        }
        out.kind = SizePredicate::Kind::Between;
        out.a = lo;
        out.b = hi;
        out.text = text;
        return true;
    }

    auto parse_after = [&](std::size_t skip, SizePredicate::Kind kind,
                           const char* op) -> bool {
        const std::string value_text = trim_copy(text.substr(skip));
        std::uint64_t bytes = 0;
        if (!parse_size_value(value_text, bytes, reason)) {
            reason = std::string(op) + " 后的尺寸无效: " + reason;
            return false;
        }
        out.kind = kind;
        out.a = bytes;
        out.text = text;
        return true;
    };

    if (starts_with(text, "<=")) return parse_after(2, SizePredicate::Kind::Le, "<=");
    if (starts_with(text, ">=")) return parse_after(2, SizePredicate::Kind::Ge, ">=");
    if (starts_with(text, "<")) return parse_after(1, SizePredicate::Kind::Lt, "<");
    if (starts_with(text, ">")) return parse_after(1, SizePredicate::Kind::Gt, ">");
    if (starts_with(text, "=")) return parse_after(1, SizePredicate::Kind::Eq, "=");

    std::uint64_t bytes = 0;
    if (!parse_size_value(text, bytes, reason)) return false;
    out.kind = SizePredicate::Kind::Eq;
    out.a = bytes;
    out.text = text;
    return true;
}

// ===========================================================================
// 文件类型（扩展名 / 分类）
// ===========================================================================

using CategoryTable =
    std::vector<std::pair<std::string, std::vector<std::string>>>;

const CategoryTable& category_table() {
    static const CategoryTable kTable = {
        {"image",
         {"jpg", "jpeg", "png", "gif", "bmp", "webp", "tif", "tiff", "ico",
          "svg", "heic", "heif", "avif", "raw", "cr2", "nef", "arw", "psd",
          "ai", "eps"}},
        {"video",
         {"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg",
          "mpeg", "ts", "m2ts", "3gp", "rm", "rmvb", "vob"}},
        {"audio",
         {"mp3", "wav", "flac", "aac", "ogg", "m4a", "wma", "opus", "mid",
          "midi", "ape", "aiff", "amr"}},
        {"document",
         {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "md",
          "markdown", "rtf", "odt", "ods", "odp", "csv", "epub", "mobi",
          "pages", "numbers", "key", "tex"}},
        {"archive",
         {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "tbz2", "xz", "txz",
          "zst", "iso", "cab", "jar", "war", "lz", "lzma", "zipx"}},
        {"code",
         {"c", "h", "cc", "cpp", "cxx", "hpp", "hxx", "m", "mm", "py", "js",
          "ts", "jsx", "tsx", "java", "cs", "go", "rs", "rb", "php", "html",
          "htm", "css", "scss", "less", "json", "xml", "yaml", "yml", "sh",
          "bat", "ps1", "cmd", "sql", "kt", "kts", "swift", "lua", "pl",
          "asm", "s", "vue", "dart", "r", "scala", "groovy", "cmake", "mk"}},
        {"text",
         {"txt", "md", "markdown", "log", "csv", "tsv", "json", "xml", "yaml",
          "yml", "ini", "cfg", "conf", "properties", "rst", "text", "nfo"}},
        {"executable",
         {"exe", "dll", "msi", "sys", "com", "scr", "so", "dylib", "bin",
          "app", "apk", "deb", "rpm", "drv", "ocx", "cpl"}},
        {"font", {"ttf", "otf", "woff", "woff2", "eot", "fon", "ttc"}},
    };
    return kTable;
}

const std::vector<std::string>* category_extensions(const std::string& category) {
    for (const auto& entry : category_table()) {
        if (entry.first == category) return &entry.second;
    }
    return nullptr;
}

std::string category_name_list() {
    std::string names;
    for (const auto& entry : category_table()) {
        if (!names.empty()) names += "/";
        names += entry.first;
    }
    return names;
}

bool extension_in_category(const std::string& category, const std::string& ext) {
    const std::vector<std::string>* extensions = category_extensions(category);
    if (extensions == nullptr) return false;
    for (const std::string& item : *extensions) {
        if (item == ext) return true;
    }
    return false;
}

// ===========================================================================
// 用户维度
// ===========================================================================

struct UserPredicate {
    enum class Kind { Uid, Owner };

    Kind kind = Kind::Owner;
    std::uint32_t uid = 0;
    std::string name;  ///< 属主名，可能带 `DOMAIN\` 前缀
    std::string text;  ///< 归一化后的原始表达式

    bool matches(const FilterContext& ctx) const;
};

/// 属主名比较：Windows 大小写不敏感；带/不带 `DOMAIN\` 前缀都算命中。
bool owner_name_matches(const std::string& owner, const std::string& pattern) {
    if (iequals(owner, pattern)) return true;
    auto account_part = [](const std::string& name) -> std::string {
        const std::size_t slash = name.find_last_of("\\/");
        return slash == std::string::npos ? name : name.substr(slash + 1);
    };
    const bool owner_has_domain = owner.find('\\') != std::string::npos;
    const bool pattern_has_domain = pattern.find('\\') != std::string::npos;
    if (!owner_has_domain && !pattern_has_domain) return false;
    return iequals(account_part(owner), account_part(pattern));
}

bool UserPredicate::matches(const FilterContext& ctx) const {
    if (kind == Kind::Uid) return ctx.uid == uid;
    if (ctx.owner.empty()) return false;
    return owner_name_matches(ctx.owner, name);
}

bool parse_user_predicate(const std::string& raw, UserPredicate& out,
                          std::string& reason) {
    const std::string text = trim_copy(raw);
    if (text.empty()) {
        reason = "用户表达式为空";
        return false;
    }
    const std::string lower = lower_ascii(text);
    if (starts_with(lower, "uid:")) {
        const std::string value_text = trim_copy(text.substr(4));
        std::uint64_t value = 0;
        if (!parse_uint64_strict(value_text, value) ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            reason = "uid 必须是 0..4294967295 的整数，实际为 \"" + value_text + "\"";
            return false;
        }
        out.kind = UserPredicate::Kind::Uid;
        out.uid = static_cast<std::uint32_t>(value);
        out.text = "uid:" + std::to_string(out.uid);
        return true;
    }
    std::string name;
    if (starts_with(lower, "owner:")) {
        name = trim_copy(text.substr(6));
    } else {
        name = text;
    }
    if (name.empty()) {
        reason = "owner: 后缺少属主名";
        return false;
    }
    out.kind = UserPredicate::Kind::Owner;
    out.name = name;
    out.text = "owner:" + name;
    return true;
}

// ===========================================================================
// 单条规则
// ===========================================================================

struct Rule {
    bool include = true;
    std::vector<Glob> paths;
    std::vector<Glob> names;
    std::vector<std::string> extensions;
    std::vector<std::string> categories;
    std::vector<TimePredicate> times;
    std::vector<SizePredicate> sizes;
    std::vector<UserPredicate> users;

    /// to_json 的原样输出（规则解析时归一化，保证可再次 from_json 往返）。
    json source = json::object();

    bool has_dimension() const {
        return !paths.empty() || !names.empty() || !extensions.empty() ||
               !categories.empty() || !times.empty() || !sizes.empty() ||
               !users.empty();
    }

    bool matches(const FilterContext& ctx, std::int64_t now) const;
};

bool Rule::matches(const FilterContext& ctx, std::int64_t now) const {
    const std::string path = normalize_relative_path(ctx.relative_path);
    std::string name = ctx.name;
    if (name.empty()) name = base_name_of(path);

    if (!paths.empty()) {
        bool hit = false;
        for (const Glob& glob : paths) {
            if (glob.matches(path)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    if (!names.empty()) {
        bool hit = false;
        for (const Glob& glob : names) {
            if (glob.matches(name)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    if (!extensions.empty() || !categories.empty()) {
        const std::string ext = extension_of(name);
        bool hit = false;
        for (const std::string& item : extensions) {
            if (item == ext) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            for (const std::string& category : categories) {
                if (extension_in_category(category, ext)) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit) return false;
    }

    if (!times.empty()) {
        bool hit = false;
        for (const TimePredicate& predicate : times) {
            if (predicate.matches(ctx.mtime_ns, now)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    if (!sizes.empty()) {
        bool hit = false;
        for (const SizePredicate& predicate : sizes) {
            if (predicate.matches(ctx.size)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    if (!users.empty()) {
        bool hit = false;
        for (const UserPredicate& predicate : users) {
            if (predicate.matches(ctx)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    return true;
}

// ===========================================================================
// 规则解析
// ===========================================================================

json one_or_many(const std::vector<std::string>& values) {
    if (values.size() == 1) return json(values.front());
    return json(values);
}

bool is_known_rule_key(const std::string& key) {
    static const char* const kKnown[] = {"action", "path", "type", "name",
                                         "time", "size", "user"};
    for (const char* known : kKnown) {
        if (key == known) return true;
    }
    return false;
}

/// 把 JSON 维度值展开成一组字符串：支持字符串 / 字符串数组，
/// size 额外兼容 `{"min":..,"max":..}`；逗号分隔视为多个 OR 值。
bool json_value_list(const json& value, const std::string& dimension,
                     const std::string& prefix, std::vector<std::string>& out,
                     std::string& error) {
    std::vector<std::string> raw;
    if (value.is_string()) {
        raw.push_back(value.get<std::string>());
    } else if (value.is_array()) {
        for (const json& item : value) {
            if (!item.is_string()) {
                error = prefix + "字段 " + dimension + " 的数组元素必须是字符串";
                return false;
            }
            raw.push_back(item.get<std::string>());
        }
    } else if (value.is_object() && dimension == "size") {
        // 兼容早期 README 写法：{"size": {"min": "1KB", "max": "10MB"}}
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() != "min" && it.key() != "max") {
                error = prefix + "size 对象只支持 min / max，遇到未知键 \"" +
                        it.key() + "\"";
                return false;
            }
        }
        std::string min_text;
        std::string max_text;
        if (value.contains("min")) {
            if (!value["min"].is_string()) {
                error = prefix + "size.min 必须是字符串";
                return false;
            }
            min_text = trim_copy(value["min"].get<std::string>());
        }
        if (value.contains("max")) {
            if (!value["max"].is_string()) {
                error = prefix + "size.max 必须是字符串";
                return false;
            }
            max_text = trim_copy(value["max"].get<std::string>());
        }
        if (min_text.empty() && max_text.empty()) {
            error = prefix + "size 对象至少需要 min 或 max";
            return false;
        }
        if (!min_text.empty() && !max_text.empty()) {
            raw.push_back(min_text + ".." + max_text);
        } else if (!min_text.empty()) {
            raw.push_back(">= " + min_text);
        } else {
            raw.push_back("<= " + max_text);
        }
    } else {
        error = prefix + "字段 " + dimension + " 必须是字符串或字符串数组";
        return false;
    }

    for (const std::string& item : raw) {
        for (const std::string& part : split_commas(item)) {
            if (!part.empty()) out.push_back(part);
        }
    }
    if (out.empty()) {
        error = prefix + "字段 " + dimension + " 至少需要一个非空值";
        return false;
    }
    return true;
}

bool parse_rule(const json& item, std::size_t index, Rule& rule,
                std::string& error) {
    const std::string prefix =
        "第 " + std::to_string(index + 1) + " 条规则: ";

    if (!item.is_object()) {
        error = prefix + "规则必须是 JSON 对象";
        return false;
    }
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (!is_known_rule_key(it.key())) {
            error = prefix + "未知的规则字段 \"" + it.key() +
                    "\"（可用: action/path/type/name/time/size/user）";
            return false;
        }
    }

    if (item.contains("action")) {
        if (!item["action"].is_string()) {
            error = prefix + "action 必须是字符串 include 或 exclude";
            return false;
        }
        const std::string action =
            lower_ascii(trim_copy(item["action"].get<std::string>()));
        if (action == "include") {
            rule.include = true;
        } else if (action == "exclude") {
            rule.include = false;
        } else {
            error = prefix + "action 只能是 include 或 exclude，实际为 \"" +
                    action + "\"";
            return false;
        }
    }

    json canonical = json::object();
    canonical["action"] = rule.include ? "include" : "exclude";

    if (item.contains("path")) {
        std::vector<std::string> values;
        if (!json_value_list(item["path"], "path", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            std::string trimmed = trim_copy(value);
            const bool trailing_separator =
                !trimmed.empty() &&
                (trimmed.back() == '/' || trimmed.back() == '\\');
            std::string pattern = normalize_relative_path(trimmed);
            if (pattern.empty()) {
                pattern = "**";  // 单独一个 '/' 视为「全部路径」
            } else if (trailing_separator) {
                pattern += "/**";  // `docs/` 等价于 `docs/**`
            }
            normalized.push_back(pattern);
            rule.paths.emplace_back(pattern, true);
        }
        canonical["path"] = one_or_many(normalized);
    }

    if (item.contains("name")) {
        std::vector<std::string> values;
        if (!json_value_list(item["name"], "name", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            const std::string trimmed = trim_copy(value);
            if (trimmed.empty()) {
                error = prefix + "name 含有空模式";
                return false;
            }
            normalized.push_back(trimmed);
            rule.names.emplace_back(trimmed, false);
        }
        canonical["name"] = one_or_many(normalized);
    }

    if (item.contains("type")) {
        std::vector<std::string> values;
        if (!json_value_list(item["type"], "type", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            const std::string token = lower_ascii(trim_copy(value));
            if (starts_with(token, "category:")) {
                const std::string category = trim_copy(token.substr(9));
                if (category.empty() || category_extensions(category) == nullptr) {
                    error = prefix + "未知的文件分类 \"" + category +
                            "\"（可用: " + category_name_list() + "）";
                    return false;
                }
                rule.categories.push_back(category);
                normalized.push_back("category:" + category);
            } else {
                std::string ext = token;
                if (starts_with(ext, "*.")) {
                    ext = ext.substr(2);
                } else if (!ext.empty() && ext.front() == '.') {
                    ext = ext.substr(1);
                }
                if (ext.empty() ||
                    ext.find_first_of("*?/\\") != std::string::npos) {
                    error = prefix + "非法的类型值 \"" + value +
                            "\"（应为扩展名、*.ext 或 category:<分类>）";
                    return false;
                }
                rule.extensions.push_back(ext);
                normalized.push_back(ext);
            }
        }
        canonical["type"] = one_or_many(normalized);
    }

    if (item.contains("time")) {
        std::vector<std::string> values;
        if (!json_value_list(item["time"], "time", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            TimePredicate predicate;
            std::string reason;
            if (!parse_time_predicate(value, predicate, reason)) {
                error = prefix + "time: " + reason;
                return false;
            }
            normalized.push_back(predicate.text);
            rule.times.push_back(std::move(predicate));
        }
        canonical["time"] = one_or_many(normalized);
    }

    if (item.contains("size")) {
        std::vector<std::string> values;
        if (!json_value_list(item["size"], "size", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            SizePredicate predicate;
            std::string reason;
            if (!parse_size_predicate(value, predicate, reason)) {
                error = prefix + "size: " + reason;
                return false;
            }
            normalized.push_back(predicate.text);
            rule.sizes.push_back(std::move(predicate));
        }
        canonical["size"] = one_or_many(normalized);
    }

    if (item.contains("user")) {
        std::vector<std::string> values;
        if (!json_value_list(item["user"], "user", prefix, values, error)) {
            return false;
        }
        std::vector<std::string> normalized;
        for (const std::string& value : values) {
            UserPredicate predicate;
            std::string reason;
            if (!parse_user_predicate(value, predicate, reason)) {
                error = prefix + "user: " + reason;
                return false;
            }
            normalized.push_back(predicate.text);
            rule.users.push_back(std::move(predicate));
        }
        canonical["user"] = one_or_many(normalized);
    }

    rule.source = std::move(canonical);
    return true;
}

}  // namespace

// ===========================================================================
// Filter（PIMPL）
// ===========================================================================

struct Filter::Impl {
    std::vector<Rule> rules;
};

Filter::Filter() : impl_(std::make_shared<const Impl>()) {}
Filter::Filter(const Filter&) = default;
Filter::Filter(Filter&&) noexcept = default;
Filter& Filter::operator=(const Filter&) = default;
Filter& Filter::operator=(Filter&&) noexcept = default;
Filter::~Filter() = default;

std::optional<Filter> Filter::from_json(const std::string& json_text,
                                        std::string& error) {
    if (trim_copy(json_text).empty()) {
        error.clear();
        return Filter();
    }
    json parsed;
    try {
        parsed = json::parse(json_text);
    } catch (const std::exception& exception) {
        error = std::string("筛选规则 JSON 解析失败: ") + exception.what();
        return std::nullopt;
    }
    return from_json(parsed, error);
}

std::optional<Filter> Filter::from_json(const nlohmann::json& rules,
                                        std::string& error) {
    std::vector<Rule> parsed;
    if (rules.is_null()) {
        // null 视为「没有规则」。
    } else if (rules.is_array()) {
        parsed.reserve(rules.size());
        for (std::size_t index = 0; index < rules.size(); ++index) {
            Rule rule;
            if (!parse_rule(rules[index], index, rule, error)) {
                return std::nullopt;
            }
            parsed.push_back(std::move(rule));
        }
    } else if (rules.is_object()) {
        // 兼容只传一条规则的写法（CLI 里很常见）。
        Rule rule;
        if (!parse_rule(rules, 0, rule, error)) return std::nullopt;
        parsed.push_back(std::move(rule));
    } else {
        error = "筛选规则必须是 JSON 数组（或单个规则对象）";
        return std::nullopt;
    }

    auto impl = std::make_shared<Impl>();
    impl->rules = std::move(parsed);
    Filter filter;
    filter.impl_ = impl;
    error.clear();
    return filter;
}

nlohmann::json Filter::to_json() const {
    nlohmann::json rules = nlohmann::json::array();
    if (!impl_) return rules;
    for (const Rule& rule : impl_->rules) {
        rules.push_back(rule.source);
    }
    return rules;
}

bool Filter::empty() const {
    if (!impl_) return true;
    for (const Rule& rule : impl_->rules) {
        if (rule.has_dimension()) return false;
    }
    return true;
}

bool Filter::match(const FilterContext& ctx) const {
    if (empty()) return true;

    const std::int64_t now = now_ns();

    // 先求 include：没有任何 include 规则时默认包含。
    bool has_include = false;
    bool included = false;
    for (const Rule& rule : impl_->rules) {
        if (!rule.include || !rule.has_dimension()) continue;
        has_include = true;
        if (!included && rule.matches(ctx, now)) included = true;
    }
    if (has_include && !included) return false;

    // 再求 exclude：命中任一 exclude 规则即排除。
    for (const Rule& rule : impl_->rules) {
        if (rule.include || !rule.has_dimension()) continue;
        if (rule.matches(ctx, now)) return false;
    }
    return true;
}

}  // namespace backup
