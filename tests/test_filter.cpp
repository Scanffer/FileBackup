#include <catch.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "filter.h"

namespace {

using backup::Filter;
using backup::FilterContext;

// ---- 构造/解析辅助 ----

Filter must_parse(const std::string& text) {
    std::string error;
    auto filter = Filter::from_json(text, error);
    if (!filter.has_value()) {
        FAIL("筛选规则解析失败: " << error);
    }
    return *filter;
}

// 字符串字面量与 nlohmann::json 都能隐式构造，加一个 const char* 重载消除歧义。
Filter must_parse(const char* text) { return must_parse(std::string(text)); }

Filter must_parse(const nlohmann::json& rules) {
    std::string error;
    auto filter = Filter::from_json(rules, error);
    if (!filter.has_value()) {
        FAIL("筛选规则解析失败: " << error);
    }
    return *filter;
}

std::string must_fail(const std::string& text) {
    std::string error;
    auto filter = Filter::from_json(text, error);
    REQUIRE_FALSE(filter.has_value());
    REQUIRE_FALSE(error.empty());
    return error;
}

nlohmann::json make_rules(std::initializer_list<nlohmann::json> rules) {
    nlohmann::json array = nlohmann::json::array();
    for (const nlohmann::json& rule : rules) array.push_back(rule);
    return array;
}

/// 只含指定维度的一条 include 规则。
Filter single_rule(const std::string& dimension, const std::string& value) {
    return must_parse(make_rules({nlohmann::json{{"action", "include"},
                                                 {dimension, value}}}));
}

/// 带路径/尺寸/mtime 的上下文（名字自动取路径最后一段）。
FilterContext context(const std::string& relative_path,
                      std::uintmax_t size = 0, std::int64_t mtime_ns = 0) {
    FilterContext ctx;
    ctx.relative_path = relative_path;
    const std::size_t slash = relative_path.find_last_of('/');
    ctx.name = slash == std::string::npos ? relative_path
                                          : relative_path.substr(slash + 1);
    ctx.size = size;
    ctx.mtime_ns = mtime_ns;
    return ctx;
}

FilterContext with_owner(FilterContext ctx, const std::string& owner,
                         std::uint32_t uid = 0) {
    ctx.owner = owner;
    ctx.uid = uid;
    return ctx;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 本地时区某天某时刻的 Unix 纳秒（与 filter 的日期解析保持同一约定）。
std::int64_t local_ns(int year, int month, int day, int hour = 0, int minute = 0,
                      int second = 0) {
    std::tm tm_value{};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    tm_value.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&tm_value);
    REQUIRE(seconds != static_cast<std::time_t>(-1));
    return static_cast<std::int64_t>(seconds) * 1000000000LL;
}

constexpr std::uintmax_t kKB = 1024;
constexpr std::uintmax_t kMB = 1024 * 1024;
constexpr std::uintmax_t kGB = 1024 * 1024 * 1024;
constexpr std::int64_t kNanosPerDay = 24LL * 60 * 60 * 1000000000LL;

}  // namespace

// ===========================================================================
// 空过滤器 / 基本语义
// ===========================================================================

TEST_CASE("filter: 空过滤器恒为真", "[filter]") {
    const Filter empty_filter;
    CHECK(empty_filter.empty());
    CHECK(empty_filter.match(context("任意/路径.txt")));
    CHECK(empty_filter.match(context("")));
    CHECK(empty_filter.to_json() == nlohmann::json::array());

    const Filter from_empty_array = must_parse("[]");
    CHECK(from_empty_array.empty());
    CHECK(from_empty_array.match(context("docs/a.txt")));

    // 只有「空规则」（没有任何维度）也视为空过滤器。
    const Filter only_empty_rules =
        must_parse(R"([{"action":"include"},{"action":"exclude"}])");
    CHECK(only_empty_rules.empty());
    CHECK(only_empty_rules.match(context("docs/a.txt")));

    const Filter from_null = must_parse("null");
    CHECK(from_null.empty());

    const Filter from_blank = must_parse("   ");
    CHECK(from_blank.empty());
}

// ===========================================================================
// 六维：path
// ===========================================================================

TEST_CASE("filter: path 维度 glob", "[filter][path]") {
    struct Case {
        const char* pattern;
        const char* path;
        bool expected;
    };
    const Case cases[] = {
        {"docs/**", "docs/a.txt", true},
        {"docs/**", "docs/sub/deep/b.pdf", true},
        {"docs/**", "docs", true},          // 目录本身也命中
        {"docs/**", "docs2/a.txt", false},
        {"docs/**", "other/docs/a.txt", false},
        {"src/*.cpp", "src/main.cpp", true},
        {"src/*.cpp", "src/util/helper.cpp", false},
        {"src/*.cpp", "src/main.h", false},
        {"src/?.cpp", "src/a.cpp", true},
        {"src/?.cpp", "src/ab.cpp", false},
        {"**/readme.md", "readme.md", true},
        {"**/readme.md", "docs/readme.md", true},
        {"**/readme.md", "docs/sub/readme.md", true},
        {"**/readme.md", "docs/readme.txt", false},
        {"docs/**/*.txt", "docs/a.txt", true},
        {"docs/**/*.txt", "docs/sub/a.txt", true},
        {"docs/**/*.txt", "docs/sub/a.md", false},
        {"*.txt", "a.txt", true},
        {"*.txt", "docs/a.txt", false},     // 单 * 不跨目录
        {"**", "任意/路径/文件.bin", true},
        {"**", "", true},
        {"docs/", "docs/a.txt", true},      // 以 / 结尾等价 docs/**
        {"docs", "docs/a.txt", true},       // 纯字面量按目录前缀
        {"docs", "docs", true},
        {"docs", "docs2/a.txt", false},
        {"DOCS/**", "docs/sub/a.txt", true},  // 大小写不敏感
        {"docs/**.TXT", "docs/sub/Readme.Txt", true},
    };
    for (const Case& item : cases) {
        INFO("pattern=" << item.pattern << " path=" << item.path);
        const Filter filter = single_rule("path", item.pattern);
        CHECK(filter.match(context(item.path)) == item.expected);
    }

    SECTION("路径归一化：反斜杠与 ./ 前缀") {
        const Filter filter = single_rule("path", "docs/**");
        CHECK(filter.match(context("docs\\sub\\b.pdf")));
        CHECK(filter.match(context("./docs/a.txt")));
        CHECK(filter.match(context("/docs/a.txt")));
    }
}

// ===========================================================================
// 六维：name
// ===========================================================================

TEST_CASE("filter: name 维度 glob", "[filter][name]") {
    const Filter report = single_rule("name", "report_*.pdf");
    CHECK(report.match(context("docs/report_2024.pdf")));
    CHECK(report.match(context("docs/REPORT_2024.PDF")));
    CHECK_FALSE(report.match(context("docs/report.pdf")));
    CHECK_FALSE(report.match(context("docs/notes.pdf")));

    const Filter any_pdf = single_rule("name", "*.pdf");
    CHECK(any_pdf.match(context("a.pdf")));
    CHECK_FALSE(any_pdf.match(context("a.txt")));

    SECTION("同维度多值 OR") {
        const Filter filter = must_parse(make_rules(
            {nlohmann::json{{"action", "include"}, {"name", "*.pdf"}},
             nlohmann::json{{"action", "include"}, {"name", "*.txt"}}}));
        CHECK(filter.match(context("a.pdf")));
        CHECK(filter.match(context("a.txt")));
        CHECK_FALSE(filter.match(context("a.md")));
    }

    SECTION("name 为空时从路径取文件名") {
        FilterContext ctx = context("docs/sub/file.txt");
        CHECK_FALSE(ctx.name.empty());  // 辅助函数默认填了 name
        ctx.name.clear();
        CHECK(single_rule("name", "*.txt").match(ctx));
        CHECK_FALSE(single_rule("name", "*.pdf").match(ctx));
    }
}

// ===========================================================================
// 六维：type
// ===========================================================================

TEST_CASE("filter: type 维度（扩展名与分类）", "[filter][type]") {
    const Filter pictures = single_rule("type", "jpg,png");
    CHECK(pictures.match(context("photos/IMG_001.JPG")));
    CHECK(pictures.match(context("photos/pic.png")));
    CHECK_FALSE(pictures.match(context("photos/clip.mp4")));

    const Filter star_form = single_rule("type", "*.jpg,*.png");
    CHECK(star_form.match(context("photos/a.JPEG")) == false);  // jpeg != jpg
    CHECK(star_form.match(context("photos/pic.png")));

    const Filter images = single_rule("type", "category:image");
    CHECK(images.match(context("a.jpg")));
    CHECK(images.match(context("a.png")));
    CHECK(images.match(context("a.svg")));
    CHECK_FALSE(images.match(context("a.mp4")));
    CHECK_FALSE(images.match(context("no_extension")));

    CHECK(single_rule("type", "category:video").match(context("a.mkv")));
    CHECK(single_rule("type", "category:audio").match(context("a.flac")));
    CHECK(single_rule("type", "category:document").match(context("a.pdf")));
    CHECK(single_rule("type", "category:archive").match(context("a.tar.gz")));
    CHECK(single_rule("type", "category:code").match(context("a.cpp")));
    CHECK(single_rule("type", "category:executable").match(context("a.exe")));

    const Filter mixed = must_parse(make_rules(
        {nlohmann::json{{"action", "include"}, {"type", "category:code"}},
         nlohmann::json{{"action", "include"}, {"type", "png"}}}));
    CHECK(mixed.match(context("src/main.cpp")));
    CHECK(mixed.match(context("img/a.png")));
    CHECK_FALSE(mixed.match(context("a.bmp")));
}

// ===========================================================================
// 六维：size
// ===========================================================================

TEST_CASE("filter: size 维度表达式", "[filter][size]") {
    struct Case {
        const char* expression;
        std::uintmax_t size;
        bool expected;
    };
    const Case cases[] = {
        {"< 100MB", 0, true},
        {"< 100MB", 100 * kMB - 1, true},
        {"< 100MB", 100 * kMB, false},
        {"<= 100MB", 100 * kMB, true},
        {"> 1KB", kKB, false},
        {"> 1KB", kKB + 1, true},
        {">= 1KB", kKB, true},
        {"= 1KB", kKB, true},
        {"= 1KB", kKB + 1, false},
        {"1KB..10MB", kKB, true},
        {"1KB..10MB", 10 * kMB, true},
        {"1KB..10MB", kKB - 1, false},
        {"1KB..10MB", 10 * kMB + 1, false},
        {"1 B", 1, true},
        {"1GB", kGB, true},
        {"1024", 1024, true},
    };
    for (const Case& item : cases) {
        INFO("expr=" << item.expression << " size=" << item.size);
        const Filter filter = single_rule("size", item.expression);
        CHECK(filter.match(context("a.bin", item.size)) == item.expected);
    }

    // 1024 进制盲区：1000B 不等于 1KB。
    CHECK_FALSE(single_rule("size", "= 1KB").match(context("a.bin", 1000)));
}

TEST_CASE("filter: size 非法表达式", "[filter][size][error]") {
    const std::vector<std::string> bad = {
        "100XB", "abc", "<", ">= ", "10MB..1KB", "-1KB", "1KB..", "..1KB",
    };
    for (const std::string& expression : bad) {
        INFO("expr=" << expression);
        const std::string error =
            must_fail(make_rules({nlohmann::json{{"action", "include"},
                                                 {"size", expression}}})
                          .dump());
        CHECK(error.find("第 1 条") != std::string::npos);
    }
}

// ===========================================================================
// 六维：time
// ===========================================================================

TEST_CASE("filter: time 维度表达式", "[filter][time]") {
    SECTION("before / after") {
        const Filter before = single_rule("time", "before 2025-03-01");
        CHECK(before.match(context("a.txt", 0, local_ns(2025, 2, 28))));
        CHECK(before.match(context("a.txt", 0, local_ns(1975, 1, 1))));
        CHECK_FALSE(before.match(context("a.txt", 0, local_ns(2025, 3, 1))));

        const Filter after = single_rule("time", "after 2025-03-01");
        CHECK(after.match(context("a.txt", 0, local_ns(2025, 3, 1))));
        CHECK_FALSE(after.match(context("a.txt", 0, local_ns(2025, 2, 28, 23, 59))));
    }

    SECTION("日期区间（结束日期当天包含）") {
        const Filter range = single_rule("time", "2025-01-01..2025-06-30");
        CHECK(range.match(context("a.txt", 0, local_ns(2025, 1, 1))));
        CHECK(range.match(context("a.txt", 0, local_ns(2025, 6, 30, 23, 59, 59))));
        CHECK_FALSE(range.match(context("a.txt", 0, local_ns(2024, 12, 31, 23, 59))));
        CHECK_FALSE(range.match(context("a.txt", 0, local_ns(2025, 7, 1))));
    }

    SECTION("单个日期 = 当天整天") {
        const Filter day = single_rule("time", "2025-06-15");
        CHECK(day.match(context("a.txt", 0, local_ns(2025, 6, 15, 0, 0, 0))));
        CHECK(day.match(context("a.txt", 0, local_ns(2025, 6, 15, 23, 59, 59))));
        CHECK_FALSE(day.match(context("a.txt", 0, local_ns(2025, 6, 16, 0, 0, 0))));
    }

    SECTION("last N days 相对当前时间") {
        const std::int64_t now = now_ns();
        const Filter recent = single_rule("time", "last 7 days");
        CHECK(recent.match(context("a.txt", 0, now)));
        CHECK(recent.match(context("a.txt", 0, now - 6 * kNanosPerDay)));
        CHECK_FALSE(recent.match(context("a.txt", 0, now - 8 * kNanosPerDay)));
    }

    SECTION("同维度多值 OR") {
        const Filter filter =
            single_rule("time", "before 2020-01-01,after 2025-01-01");
        CHECK(filter.match(context("a.txt", 0, local_ns(2019, 6, 1))));
        CHECK(filter.match(context("a.txt", 0, local_ns(2026, 1, 1))));
        CHECK_FALSE(filter.match(context("a.txt", 0, local_ns(2024, 6, 1))));
    }
}

TEST_CASE("filter: time 非法表达式", "[filter][time][error]") {
    const std::vector<std::string> bad = {
        "before 2025-13-01", "2025-02-30..2025-03-01", "last x days",
        "garbage", "", "2025/02/30",
    };
    for (const std::string& expression : bad) {
        INFO("expr=" << expression);
        const std::string error =
            must_fail(make_rules({nlohmann::json{{"action", "include"},
                                                 {"time", expression}}})
                          .dump());
        CHECK(error.find("第 1 条") != std::string::npos);
    }
}

// ===========================================================================
// 六维：user
// ===========================================================================

TEST_CASE("filter: user 维度", "[filter][user]") {
    SECTION("uid") {
        const Filter filter = single_rule("user", "uid:1000");
        CHECK(filter.match(with_owner(context("a.txt"), "", 1000)));
        CHECK_FALSE(filter.match(with_owner(context("a.txt"), "alice", 1001)));
        CHECK_FALSE(single_rule("user", "uid:0")
                        .match(with_owner(context("a.txt"), "", 1000)));
    }

    SECTION("owner 与直接姓名") {
        const Filter by_owner = single_rule("user", "owner:alice");
        CHECK(by_owner.match(with_owner(context("a.txt"), "alice")));
        CHECK(by_owner.match(with_owner(context("a.txt"), "ALICE")));
        CHECK_FALSE(by_owner.match(with_owner(context("a.txt"), "bob")));
        CHECK_FALSE(by_owner.match(context("a.txt")));

        const Filter bare = single_rule("user", "alice");
        CHECK(bare.match(with_owner(context("a.txt"), "alice")));
    }

    SECTION("带域名的属主名") {
        CHECK(single_rule("user", "owner:alice")
                  .match(with_owner(context("a.txt"), "WORKGROUP\\alice")));
        CHECK(single_rule("user", "owner:WORKGROUP\\alice")
                  .match(with_owner(context("a.txt"), "WORKGROUP\\alice")));
        CHECK(single_rule("user", "owner:alice")
                  .match(with_owner(context("a.txt"), "alice")));
    }

    SECTION("多值 OR") {
        const Filter filter = single_rule("user", "uid:1000,owner:bob");
        CHECK(filter.match(with_owner(context("a.txt"), "", 1000)));
        CHECK(filter.match(with_owner(context("a.txt"), "bob", 42)));
        CHECK_FALSE(filter.match(with_owner(context("a.txt"), "carol", 42)));
    }
}

TEST_CASE("filter: user 非法表达式", "[filter][user][error]") {
    const std::vector<std::string> bad = {"uid:abc", "uid:99999999999",
                                          "owner:", "uid:"};
    for (const std::string& expression : bad) {
        INFO("expr=" << expression);
        const std::string error =
            must_fail(make_rules({nlohmann::json{{"action", "include"},
                                                 {"user", expression}}})
                          .dump());
        CHECK(error.find("第 1 条") != std::string::npos);
    }
}

// ===========================================================================
// 组合语义
// ===========================================================================

TEST_CASE("filter: 规则内 AND、同维度 OR、include/exclude 语义", "[filter][logic]") {
    SECTION("规则内维度 AND") {
        const Filter filter = must_parse(make_rules({
            nlohmann::json{{"action", "include"},
                           {"path", "docs/**"},
                           {"name", "*.pdf"}}}));
        CHECK(filter.match(context("docs/a.pdf")));
        CHECK_FALSE(filter.match(context("docs/a.txt")));
        CHECK_FALSE(filter.match(context("other/a.pdf")));
    }

    SECTION("同维度多值 OR") {
        const Filter filter = must_parse(make_rules({
            nlohmann::json{{"action", "include"}, {"name", "*.pdf,*.txt"}}}));
        CHECK(filter.match(context("a.pdf")));
        CHECK(filter.match(context("a.txt")));
        CHECK_FALSE(filter.match(context("a.md")));
    }

    SECTION("没有 include 规则时默认包含") {
        const Filter filter = must_parse(
            R"([{"action":"exclude","type":"tmp,bak"}])");
        CHECK(filter.match(context("a.txt")));
        CHECK_FALSE(filter.match(context("a.tmp")));
        CHECK_FALSE(filter.match(context("a.bak")));
    }

    SECTION("只有 include 规则") {
        const Filter filter =
            must_parse(R"([{"action":"include","name":"*.txt"}])");
        CHECK(filter.match(context("a.txt")));
        CHECK_FALSE(filter.match(context("a.md")));
    }

    SECTION("include 后再 exclude，exclude 优先") {
        const Filter filter = must_parse(R"([
            {"action":"include","name":"*.txt"},
            {"action":"exclude","name":"*_backup.txt"}
        ])");
        CHECK(filter.match(context("notes.txt")));
        CHECK_FALSE(filter.match(context("notes_backup.txt")));
        CHECK_FALSE(filter.match(context("notes.md")));
    }

    SECTION("多条 include 规则之间 OR") {
        const Filter filter = must_parse(R"([
            {"action":"include","path":"docs/**"},
            {"action":"include","name":"*.md"}
        ])");
        CHECK(filter.match(context("docs/a.txt")));
        CHECK(filter.match(context("readme.md")));
        CHECK_FALSE(filter.match(context("other/a.txt")));
    }

    SECTION("省略 action 默认 include") {
        const Filter filter = must_parse(R"([{"name":"*.txt"}])");
        CHECK(filter.match(context("a.txt")));
        CHECK_FALSE(filter.match(context("a.md")));
    }

    SECTION("action 大小写不敏感") {
        CHECK_FALSE(must_parse(R"([{"action":"EXCLUDE","name":"*.tmp"}])")
                        .match(context("a.tmp")));
    }

    SECTION("六维组合：path + type + size + user") {
        const Filter filter = must_parse(R"([
            {"action":"include",
             "path":"docs/**",
             "type":"category:document",
             "size":"< 10MB",
             "user":"owner:alice"}
        ])");
        const FilterContext ok = with_owner(
            context("docs/report.pdf", 1024), "alice");
        CHECK(filter.match(ok));

        FilterContext too_big = with_owner(
            context("docs/report.pdf", 20 * kMB), "alice");
        CHECK_FALSE(filter.match(too_big));

        FilterContext wrong_owner = with_owner(
            context("docs/report.pdf", 1024), "bob");
        CHECK_FALSE(filter.match(wrong_owner));

        FilterContext wrong_type = with_owner(
            context("docs/report.exe", 1024), "alice");
        CHECK_FALSE(filter.match(wrong_type));
    }
}

// ===========================================================================
// 非法输入
// ===========================================================================

TEST_CASE("filter: 非法规则报错带规则序号", "[filter][error]") {
    SECTION("非法 JSON 文本") {
        const std::string error = must_fail("{not json");
        CHECK(error.find("JSON") != std::string::npos);
    }

    SECTION("根不是数组/对象") {
        CHECK(must_fail("42").find("数组") != std::string::npos);
        CHECK(must_fail("\"abc\"").find("数组") != std::string::npos);
    }

    SECTION("规则不是对象") {
        const std::string error = must_fail(R"([{"action":"include"}, 42])");
        CHECK(error.find("第 2 条") != std::string::npos);
    }

    SECTION("action 非法") {
        const std::string error = must_fail(R"([{"action":"keep"}])");
        CHECK(error.find("第 1 条") != std::string::npos);
        CHECK(error.find("action") != std::string::npos);
    }

    SECTION("未知字段") {
        const std::string error = must_fail(R"([{"action":"include","pth":"x"}])");
        CHECK(error.find("第 1 条") != std::string::npos);
        CHECK(error.find("pth") != std::string::npos);
    }

    SECTION("维度类型错误") {
        const std::string error =
            must_fail(R"([{"action":"include","path":123}])");
        CHECK(error.find("第 1 条") != std::string::npos);
    }

    SECTION("数组元素不是字符串") {
        const std::string error =
            must_fail(R"([{"action":"include","name":["*.txt",42]}])");
        CHECK(error.find("第 1 条") != std::string::npos);
    }

    SECTION("空字符串维度") {
        CHECK(must_fail(R"([{"action":"include","path":""}])").find("第 1 条") !=
              std::string::npos);
    }

    SECTION("错误序号定位到具体规则") {
        const std::string error = must_fail(R"([
            {"action":"include","path":"docs/**"},
            {"action":"include","size":"100XB"},
            {"action":"exclude","name":"*.tmp"}
        ])");
        CHECK(error.find("第 2 条") != std::string::npos);
    }

    SECTION("未知分类") {
        const std::string error = must_fail(R"([{"type":"category:nope"}])");
        CHECK(error.find("第 1 条") != std::string::npos);
        CHECK(error.find("nope") != std::string::npos);
    }
}

// ===========================================================================
// 序列化往返
// ===========================================================================

TEST_CASE("filter: 更多边界写法", "[filter][edge]") {
    SECTION("path 数组形式") {
        const Filter filter = must_parse(make_rules({
            nlohmann::json{{"action", "include"},
                           {"path", nlohmann::json::array({"docs/**", "src/*.cpp"})}}}));
        CHECK(filter.match(context("docs/a.txt")));
        CHECK(filter.match(context("src/main.cpp")));
        CHECK_FALSE(filter.match(context("other/a.txt")));
    }

    SECTION("**/ 不允许跨段部分匹配") {
        const Filter filter = single_rule("path", "docs/**/x");
        CHECK(filter.match(context("docs/x")));
        CHECK(filter.match(context("docs/a/x")));
        CHECK(filter.match(context("docs/a/b/x")));
        CHECK_FALSE(filter.match(context("docs/abx")));
    }

    SECTION("中文路径与文件名") {
        const Filter path_filter = single_rule("path", "文档/**");
        CHECK(path_filter.match(context("文档/报告/2025.pdf")));
        CHECK_FALSE(path_filter.match(context("其他/报告.pdf")));

        const Filter name_filter = single_rule("name", "报告_*.pdf");
        CHECK(name_filter.match(context("文档/报告_2025.pdf")));
        CHECK_FALSE(name_filter.match(context("文档/summary.pdf")));
    }

    SECTION("size 对象写法（兼容早期 README）") {
        const Filter range =
            must_parse(R"({"action":"include","size":{"min":"1KB","max":"10MB"}})");
        CHECK(range.match(context("a.bin", 2048)));
        CHECK(range.match(context("a.bin", 10 * kMB)));
        CHECK_FALSE(range.match(context("a.bin", 512)));
        CHECK_FALSE(range.match(context("a.bin", 20 * kMB)));

        const Filter upper =
            must_parse(R"({"action":"include","size":{"max":"100MB"}})");
        CHECK(upper.match(context("a.bin", 100 * kMB)));
        CHECK_FALSE(upper.match(context("a.bin", 100 * kMB + 1)));
    }

    SECTION("路径统一为 '/' 后再匹配") {
        CHECK(single_rule("path", "src/*.cpp")
                  .match(context("src\\main.cpp")));
        CHECK(single_rule("path", "src\\*.cpp")
                  .match(context("src/main.cpp")));
    }
}

TEST_CASE("filter: to_json 可往返", "[filter][json]") {    const std::string original = R"([
        {"action":"include","path":"docs/**","name":"*.pdf"},
        {"action":"exclude","type":"tmp,bak"},
        {"action":"include","size":">= 1KB,< 100MB"},
        {"action":"include","time":"last 7 days"},
        {"action":"exclude","user":"uid:1000,owner:bob"}
    ])";
    const Filter first = must_parse(original);

    const nlohmann::json serialized = first.to_json();
    REQUIRE(serialized.is_array());
    REQUIRE(serialized.size() == 5);
    CHECK(serialized[0]["action"] == "include");
    CHECK(serialized[0]["path"] == "docs/**");
    CHECK(serialized[0]["name"] == "*.pdf");

    const Filter second = must_parse(serialized);
    const std::vector<FilterContext> samples = {
        context("docs/a.pdf", 2048, now_ns()),
        context("docs/sub/a.txt", 100, now_ns() - 30 * kNanosPerDay),
        context("other/b.tmp", 4096, now_ns()),
        context("other/c.bak", 4096, now_ns()),
        with_owner(context("x.bin", 1024 * 1024, now_ns()), "bob"),
        with_owner(context("x.bin", 1024 * 1024, now_ns()), "alice"),
    };
    for (const FilterContext& sample : samples) {
        INFO("sample=" << sample.relative_path);
        CHECK(first.match(sample) == second.match(sample));
    }

    // 通过字符串再解析一次也应该一致；to_json 幂等。
    const Filter third = must_parse(serialized.dump());
    CHECK(third.to_json() == serialized);
    CHECK(first.to_json() == serialized);
}

TEST_CASE("filter: from_json 兼容单条规则对象", "[filter][json]") {
    const Filter filter = must_parse(
        R"({"action":"include","path":"docs/**"})");
    CHECK_FALSE(filter.empty());
    CHECK(filter.match(context("docs/a.txt")));
    CHECK_FALSE(filter.match(context("other/a.txt")));

    const nlohmann::json serialized = filter.to_json();
    REQUIRE(serialized.is_array());
    REQUIRE(serialized.size() == 1);
    CHECK(serialized[0]["path"] == "docs/**");
    CHECK(must_parse(serialized).match(context("docs/a.txt")));
}

TEST_CASE("filter: 拷贝/移动语义保持行为", "[filter][copy]") {
    const Filter filter = must_parse(R"([{"action":"include","name":"*.txt"}])");
    const Filter copied = filter;
    Filter moved = filter;
    const Filter moved_into = std::move(moved);
    CHECK(copied.match(context("a.txt")));
    CHECK(moved_into.match(context("a.txt")));
    CHECK_FALSE(moved_into.match(context("a.md")));
}
