#include "http_server.h"

// httplib.h 必须放在 <windows.h> 之前。
// 它会引入 <winsock2.h>，而 winsock2.h 一定要早于 windows.h 包含，
// 否则就是满屏的 fd_set / timeval / sockaddr 结构体重定义错误。
// （另：httplib.h 顶部有一个 _WIN32_WINNT < 0x0A00 就 #error 的检查，
//  本项目不受影响 —— MinGW 的 _mingw.h 默认定义为 0xa00。）
#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "algorithm.h"
#include "backup.h"
#include "filter.h"
#include "fs_utils.h"
#include "pack.h"
#include "tasks.h"
#include "version.h"
#include "web_page.h"
#include "webview_host.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 不单独 include <shellapi.h>：它依赖 windows.h 里的 HWND/HINSTANCE 等类型，
// 放前面编不过，放后面又是多余的 —— windows.h 自己就会把 shellapi.h 拉进来
// （这里没有定义 WIN32_LEAN_AND_MEAN，所以它包含 shellapi.h）。
#include <windows.h>
#endif

namespace backup {
namespace {

namespace fs = std::filesystem;

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 通用响应助手
// ---------------------------------------------------------------------------

void reply_json(httplib::Response& res, int status,
                const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

/// 统一的 JSON 错误响应。
///
/// 字段刻意冗余：error / message 是同一句话，前端不管读哪个都能显示；
/// code 是给程序判断用的稳定标识（不做国际化，也不用于展示）。
/// 注意：message 里绝不能出现密码 —— 调用方构造错误信息时也不许拼密码。
void reply_error(httplib::Response& res, int status, const char* code,
                 const std::string& message) {
    reply_json(res, status, {{"ok", false},
                             {"error", message},
                             {"message", message},
                             {"code", code}});
}

/// 读取表单/查询参数；不存在时返回空串（不写错误响应，交给调用方判断）。
///
/// httplib 的 req.params 同时合并了 query string 与
/// application/x-www-form-urlencoded 请求体，所以前端用哪种方式传都行。
std::string param(const httplib::Request& req, const char* key) {
    return req.has_param(key) ? req.get_param_value(key) : std::string();
}

/// 取参数并校验「存在且非空」。校验失败时已经把 400 响应写好了。
///
/// 注意 req.get_param_value() 拿到的已经是**百分号解码后的原始字节**，
/// 中文路径在这里是完整的 UTF-8。但绝不能拿它直接构造
/// std::filesystem::path —— 必须过 backup::path_from_u8（见下）。
bool require_param(const httplib::Request& req, const char* key,
                   std::string& out, httplib::Response& res) {
    if (!req.has_param(key)) {
        reply_error(res, 400, "missing_param", std::string("缺少参数：") + key);
        return false;
    }
    out = req.get_param_value(key);
    if (out.empty()) {
        reply_error(res, 400, "empty_param", std::string("参数为空：") + key);
        return false;
    }
    return true;
}

/// 解析 1/0、true/false、yes/no、on/off 形式的布尔参数；缺省或空串用默认值。
bool parse_bool_param(const httplib::Request& req, const char* key,
                      bool default_value, bool& out, httplib::Response& res) {
    out = default_value;
    if (!req.has_param(key)) return true;
    const std::string text = req.get_param_value(key);
    if (text.empty()) return true;
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    reply_error(res, 400, "invalid_param",
                std::string("无效的 ") + key + " 参数（应为 1/0）：" + text);
    return false;
}

/// 严格整数解析：整个字符串都必须是十进制整数（"3x"、"3 " 都算非法）。
bool parse_int(const std::string& text, int& out) {
    if (text.empty()) return false;
    int value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) return false;
    out = value;
    return true;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) joined += "、";
        joined += names[i];
    }
    return joined;
}

// ---------------------------------------------------------------------------
// 压缩级别范围
// ---------------------------------------------------------------------------
//
// core 的 AlgorithmInfo 只带了 has_level，没有 min/max；范围的事实来源是
// core/compress.cpp 的 clamp_compression_level（DEFLATE 1–9、zstd 1–19）。
// 这里跟着它写一份，API 才能告诉前端滑块的上下限。
// **如果哪天 core 改了范围，这里要同步改**（否则前端会显示旧范围，
// 服务端也不会拦，最终由 core 的 clamp 兜底）。
struct LevelRange {
    bool has_level = false;
    int min_level = 1;
    int max_level = 1;
};

LevelRange compression_level_range(CompressionId id) {
    switch (id) {
        case CompressionId::Deflate: return {true, 1, 9};
        case CompressionId::Zstd: return {true, 1, 19};
        default: return {false, 1, 1};
    }
}

// ---------------------------------------------------------------------------
// /api/algorithms：压缩/加密/打包算法清单
// ---------------------------------------------------------------------------

/// available 表示当前构建里是否真的实现了（make_compressor/make_cipher
/// 返回非空）。前端据此只把可用的算法放进下拉框，避免用户选了才报错。
/// self_made 用于界面标注「自研」。
nlohmann::json algorithms_payload() {
    nlohmann::json root = nlohmann::json::object();

    root["compress"] = nlohmann::json::array();
    for (const AlgorithmInfo& info : compression_algorithms()) {
        const auto id = compression_from_string(info.id);
        const bool available =
            info.id == "none" || (id && make_compressor(*id) != nullptr);
        const LevelRange range =
            id ? compression_level_range(*id) : LevelRange{};
        const bool has_level = info.has_level && range.has_level;
        root["compress"].push_back(
            {{"id", info.id},
             {"label", info.label},
             {"has_level", has_level},
             {"level_min", has_level ? range.min_level : 1},
             {"level_max", has_level ? range.max_level : 1},
             {"self_made", info.self_made},
             {"note", info.note},
             {"available", available}});
    }

    root["encrypt"] = nlohmann::json::array();
    for (const AlgorithmInfo& info : encryption_algorithms()) {
        const auto id = encryption_from_string(info.id);
        const bool available =
            info.id == "none" || (id && make_cipher(*id) != nullptr);
        // 加密没有「级别」概念，字段照样给全，前端不用做特例判断。
        root["encrypt"].push_back({{"id", info.id},
                                   {"label", info.label},
                                   {"has_level", false},
                                   {"level_min", 1},
                                   {"level_max", 1},
                                   {"self_made", info.self_made},
                                   {"note", info.note},
                                   {"available", available}});
    }

    root["pack"] = nlohmann::json::array();
    for (const AlgorithmInfo& info : pack_algorithms()) {
        root["pack"].push_back({{"id", info.id},
                                {"label", info.label},
                                {"has_level", false},
                                {"level_min", 1},
                                {"level_max", 1},
                                {"self_made", info.self_made},
                                {"note", info.note},
                                {"available", info.id == "vbak"}});
    }
    return root;
}

// ---------------------------------------------------------------------------
// /api/fs/list：目录列表（给 GUI 目录树用）
// ---------------------------------------------------------------------------

/// std::filesystem::path -> UTF-8。Windows 下 u8string() 会对宽字符做
/// UTF-8 编码；MinGW 的 C++17 里它返回 std::string。
std::string path_to_u8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

/// 文件修改时间 -> Unix epoch 纳秒。
///
/// std::filesystem::file_time_type 在 C++17 没有标准换算通道，用「当前时刻
/// 在两个时钟里的偏移」反推；M1 的 pack.cpp/backup.cpp 也是这么算的。
std::int64_t file_mtime_ns(const fs::path& path) {
    std::error_code ec;
    const auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    const auto file_now = fs::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    const auto offset =
        sys_now.time_since_epoch() -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            file_now.time_since_epoch());
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            ft.time_since_epoch()) +
        offset;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch)
        .count();
}

struct DirEntryInfo {
    std::string name;
    std::string path;
    bool is_dir = false;
    std::uintmax_t size = 0;
    std::int64_t mtime_ns = 0;
};

nlohmann::json dir_entry_json(const DirEntryInfo& entry) {
    return {{"name", entry.name},   {"path", entry.path},
            {"is_dir", entry.is_dir}, {"size", entry.size},
            {"mtime_ns", entry.mtime_ns}};
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// 列一层目录（不递归），目录排前面，同类按文件名（ASCII 不区分大小写）排。
/// 失败返回 false 并写 error（UTF-8 文本，可直接展示）。
bool list_directory(const fs::path& dir, std::vector<DirEntryInfo>& out,
                    std::string& error) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        error = "无法读取目录：" + ec.message();
        return false;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "无法读取目录：" + ec.message();
            return false;
        }
        const fs::path& path = it->path();
        DirEntryInfo entry;
        entry.name = path_to_u8(path.filename());
        entry.path = path_to_u8(path);
        std::error_code entry_ec;
        entry.is_dir = it->is_directory(entry_ec);
        if (!entry_ec && !entry.is_dir) {
            const auto size = it->file_size(entry_ec);
            if (!entry_ec) entry.size = size;
        }
        entry.mtime_ns = file_mtime_ns(path);
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntryInfo& a, const DirEntryInfo& b) {
                  if (a.is_dir != b.is_dir) return a.is_dir;
                  return ascii_lower(a.name) < ascii_lower(b.name);
              });
    return true;
}

#ifdef _WIN32
/// 盘符列表：GUI 目录树没有「我的电脑」这一层 API 时的入口。
bool list_drives(std::vector<DirEntryInfo>& out) {
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) return false;
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const std::wstring root =
            std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_NO_ROOT_DIR) continue;
        DirEntryInfo entry;
        entry.name = std::string(1, static_cast<char>('A' + i)) + ":\\";
        entry.path = entry.name;
        entry.is_dir = true;
        out.push_back(std::move(entry));
    }
    return !out.empty();
}
#endif

// ---------------------------------------------------------------------------
// 任务提交：参数校验（同步，出错立刻 400） + 任务体（异步执行文件操作）
// ---------------------------------------------------------------------------

/// 提交一个备份任务：archive_mode 决定走 pack_directory 还是 mirror_directory。
/// 所有会写文件的参数都已经在 handler 里校验/解析过了，这里只做值捕获。
struct BackupTaskParams {
    bool archive_mode = false;
    std::string src;
    std::string dst;
    CompressionId compression = CompressionId::None;
    int compression_level = 6;
    EncryptionId encryption = EncryptionId::None;
    std::string password;  ///< 只在内存里传给 core；绝不出现在日志/错误里
    Filter filter;
    bool keep_metadata = true;
};

TaskBody make_backup_body(BackupTaskParams params) {
    return [params = std::move(params)](TaskReporter report) -> TaskOutcome {
        const auto progress_cb = [report](const Progress& p) {
            if (!report) return;
            int percent = 0;
            if (p.entries_total > 0) {
                percent = static_cast<int>(100ULL * p.entries_done /
                                           p.entries_total);
            }
            report(percent, "正在备份：" + p.current + " (" +
                                std::to_string(p.entries_done) + "/" +
                                std::to_string(p.entries_total) + ")");
        };

        TaskOutcome out;
        if (params.archive_mode) {
            PackOptions options;
            options.src = path_from_u8(params.src);
            options.archive = path_from_u8(params.dst);
            options.compression = params.compression;
            options.compression_level = params.compression_level;
            options.encryption = params.encryption;
            options.password = params.password;
            options.filter = params.filter;
            options.keep_metadata = params.keep_metadata;
            options.progress = progress_cb;
            const PackResult result = pack_directory(options);

            out.result = {{"mode", "archive"},
                          {"entries", result.entries_written},
                          {"bytes", result.bytes_written},
                          {"compression", to_string(params.compression)},
                          {"encryption", to_string(params.encryption)}};
            out.message = "打包完成：" +
                          std::to_string(result.entries_written) + " 个文件，" +
                          std::to_string(result.bytes_written) + " 字节";
        } else {
            MirrorOptions options;
            options.src = path_from_u8(params.src);
            options.dst = path_from_u8(params.dst);
            options.filter = params.filter;
            options.keep_metadata = params.keep_metadata;
            options.progress = progress_cb;
            const MirrorResult result = mirror_directory(options);

            out.result = {{"mode", "mirror"},
                          {"entries", result.files_copied},
                          {"bytes", result.bytes_copied},
                          {"skipped", result.files_skipped},
                          {"filtered", result.entries_filtered}};
            out.message = "备份完成：" +
                          std::to_string(result.files_copied) + " 个文件，" +
                          std::to_string(result.bytes_copied) + " 字节";
            if (result.entries_filtered > 0) {
                out.message += "（筛选排除 " +
                               std::to_string(result.entries_filtered) + " 项）";
            }
            if (result.files_skipped > 0) {
                out.message += "（跳过 " +
                               std::to_string(result.files_skipped) +
                               " 个已存在文件）";
            }
        }
        return out;
    };
}

struct RestoreTaskParams {
    bool from_archive = false;
    std::string src;  ///< from_archive ? 归档文件 : 镜像源目录
    std::string dst;
    std::string password;
    bool restore_metadata = true;
};

TaskBody make_restore_body(RestoreTaskParams params) {
    return [params = std::move(params)](TaskReporter report) -> TaskOutcome {
        const auto progress_cb = [report](const Progress& p) {
            if (!report) return;
            int percent = 0;
            if (p.entries_total > 0) {
                percent = static_cast<int>(100ULL * p.entries_done /
                                           p.entries_total);
            }
            report(percent, "正在还原：" + p.current + " (" +
                                std::to_string(p.entries_done) + "/" +
                                std::to_string(p.entries_total) + ")");
        };

        TaskOutcome out;
        if (params.from_archive) {
            UnpackOptions options;
            options.archive = path_from_u8(params.src);
            options.dst = path_from_u8(params.dst);
            options.password = params.password;
            options.restore_metadata = params.restore_metadata;
            options.progress = progress_cb;
            const UnpackResult result = unpack_archive(options);

            out.result = {{"mode", "archive"},
                          {"entries", result.entries_read},
                          {"bytes", result.bytes_read}};
            out.message = "解包完成：" +
                          std::to_string(result.entries_read) + " 个文件，" +
                          std::to_string(result.bytes_read) + " 字节";
        } else {
            // 镜像还原走 restore_mirror：它读 `.backup-manifest.json` 把
            // 权限/时间戳/ACL 一并还原（restore_metadata=false 时只复制文件）。
            MirrorRestoreOptions options;
            options.src = path_from_u8(params.src);
            options.dst = path_from_u8(params.dst);
            options.restore_metadata = params.restore_metadata;
            options.progress = progress_cb;
            const MirrorResult result = restore_mirror(options);

            out.result = {{"mode", "mirror"},
                          {"entries", result.files_copied},
                          {"bytes", result.bytes_copied},
                          {"skipped", result.files_skipped}};
            out.message = "还原完成：" +
                          std::to_string(result.files_copied) + " 个文件，" +
                          std::to_string(result.bytes_copied) + " 字节";
        }
        return out;
    };
}

/// 收集「当前模式下不该出现的参数名」。
std::vector<std::string> unsupported_params(
    const httplib::Request& req,
    const std::vector<const char*>& names) {
    std::vector<std::string> found;
    for (const char* name : names) {
        if (!param(req, name).empty()) found.emplace_back(name);
    }
    return found;
}

// ---------------------------------------------------------------------------
// 路由注册
// ---------------------------------------------------------------------------

void register_routes(httplib::Server& svr,
                     const std::shared_ptr<TaskManager>& tasks,
                     const std::shared_ptr<std::atomic<bool>>& shutting_down) {
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        // 三参数版：省掉一次 std::string 构造，并去掉尾部 NUL。
        res.set_content(kIndexHtml, sizeof(kIndexHtml) - 1,
                        "text/html; charset=utf-8");
    });

    svr.Get("/api/status", [tasks](const httplib::Request&,
                                   httplib::Response& res) {
        const std::size_t task_count = tasks->list().size();
        reply_json(res, 200, {{"ok", true},
                              {"service", "FileBackup"},
                              {"version", version()},
                              {"task_count", task_count}});
    });

    // 算法清单。前端下拉框据此生成，只有 available=true 的才会出现。
    svr.Get("/api/algorithms",
            [](const httplib::Request&, httplib::Response& res) {
                reply_json(res, 200, algorithms_payload());
            });

    // ---- 任务查询 --------------------------------------------------------
    svr.Get("/api/tasks", [tasks](const httplib::Request&,
                                  httplib::Response& res) {
        nlohmann::json list = nlohmann::json::array();
        for (const TaskInfo& info : tasks->list()) {
            list.push_back(TaskManager::to_json(info));
        }
        reply_json(res, 200, {{"tasks", list}});
    });

    // 单任务。没命中正则时 httplib 会自然落到 404，这里只处理任务 id 不存在。
    svr.Get(R"(/api/tasks/([A-Za-z0-9_-]+))",
            [tasks](const httplib::Request& req, httplib::Response& res) {
                const std::string id = req.matches[1].str();
                const auto info = tasks->get(id);
                if (!info) {
                    reply_error(res, 404, "task_not_found",
                                "任务不存在：" + id);
                    return;
                }
                reply_json(res, 200, TaskManager::to_json(*info));
            });

    // ---- SSE 任务进度 ----------------------------------------------------
    //
    // 用 chunked content provider：handler 先把响应头发出去，然后在
    // provider 里阻塞轮询任务状态，把变化推成 `data: {JSON}\n\n`。
    //   1. 任务到终态（succeeded/failed）→ 发最后一个快照，sink.done() 关连接；
    //   2. 15 秒没有任何事件 → 发一行 SSE 注释（`: ping\n\n`）当心跳，
    //      防止 WebView2 把空闲连接掐掉；
    //   3. 客户端断开（sink.write 失败）或服务正在关闭 → 立刻返回 false 收尾。
    // 每个连接占 httplib 线程池一个线程，但本地单用户只会有 1~2 个，够用。
    svr.Get(R"(/api/tasks/([A-Za-z0-9_-]+)/stream)",
            [tasks, shutting_down](const httplib::Request& req,
                                   httplib::Response& res) {
                const std::string id = req.matches[1].str();
                if (!tasks->get(id)) {
                    reply_error(res, 404, "task_not_found",
                                "任务不存在：" + id);
                    return;
                }

                res.set_header("Cache-Control", "no-cache");
                res.set_header("X-Accel-Buffering", "no");
                res.set_chunked_content_provider(
                    "text/event-stream; charset=utf-8",
                    [tasks, shutting_down, id](std::size_t,
                                               httplib::DataSink& sink)
                        -> bool {
                        const auto write_event =
                            [&sink](const nlohmann::json& payload) -> bool {
                            std::string chunk = "data: ";
                            chunk += payload.dump();
                            chunk += "\n\n";
                            return sink.write(chunk.data(), chunk.size());
                        };
                        const auto write_comment =
                            [&sink](const char* text) -> bool {
                            std::string chunk = ": ";
                            chunk += text;
                            chunk += "\n\n";
                            return sink.write(chunk.data(), chunk.size());
                        };

                        std::string last_payload;
                        auto last_write = std::chrono::steady_clock::now();
                        bool first = true;
                        while (!shutting_down->load()) {
                            const auto info = tasks->get(id);
                            if (!info) {
                                // 任务记录被历史清理掉了（正常不会发生，
                                // 除非客户端盯了几百个任务之后的那一个）。
                                write_event({{"id", id},
                                             {"kind", ""},
                                             {"state", "failed"},
                                             {"progress", 50},
                                             {"message", "任务记录已清理"},
                                             {"error", "任务记录已清理"},
                                             {"result", nullptr}});
                                sink.done();
                                return true;
                            }

                            const nlohmann::json payload =
                                TaskManager::to_json(*info);
                            const std::string text = payload.dump();
                            if (first || text != last_payload) {
                                if (!write_event(payload)) {
                                    return false;  // 客户端断开
                                }
                                last_payload = text;
                                last_write = std::chrono::steady_clock::now();
                                first = false;
                            }

                            if (info->state == TaskState::Succeeded ||
                                info->state == TaskState::Failed) {
                                sink.done();
                                return true;
                            }

                            std::this_thread::sleep_for(200ms);
                            const auto now = std::chrono::steady_clock::now();
                            if (now - last_write >= 15s) {
                                if (!write_comment("ping")) return false;
                                last_write = now;
                            }
                        }
                        sink.done();  // 服务正在关闭：安静地收尾
                        return true;
                    });
            });

    // ---- 归档信息（还原页选择 .vbak 后展示） ----------------------------
    svr.Get("/api/archive/info", [](const httplib::Request& req,
                                    httplib::Response& res) {
        const std::string path = param(req, "path");
        if (path.empty()) {
            reply_error(res, 400, "missing_param", "缺少参数：path");
            return;
        }
        if (path.find('\0') != std::string::npos) {
            reply_error(res, 400, "invalid_path", "path 含非法字符");
            return;
        }

        std::string error;
        std::optional<PackArchiveInfo> info;
        try {
            info = inspect_archive(path_from_u8(path), error);
        } catch (const std::exception& e) {
            reply_error(res, 400, "invalid_path", e.what());
            return;
        }
        if (!info) {
            reply_error(res, 400, "archive_error",
                        error.empty() ? "无法读取归档" : error);
            return;
        }

        nlohmann::json body = {
            {"ok", true},
            {"info",
             {{"version", info->version},
              {"entry_count", info->entry_count},
              {"archive_size", info->archive_size},
              {"compression", to_string(info->compression)},
              {"encryption", to_string(info->encryption)},
              {"encrypted", info->encrypted},
              {"has_metadata", info->has_metadata}}}};
        reply_json(res, 200, body);
    });

    // ---- 目录列表（给 GUI 目录树用，可选能力） --------------------------
    svr.Get("/api/fs/list", [](const httplib::Request& req,
                               httplib::Response& res) {
        if (!req.has_param("path")) {
            reply_error(res, 400, "missing_param", "缺少参数：path");
            return;
        }
        const std::string text = req.get_param_value("path");
        if (text.find('\0') != std::string::npos) {
            reply_error(res, 400, "invalid_path", "path 含非法字符");
            return;
        }

        // Windows 上 path= 空串给盘符列表，GUI 目录树没有别的入口能拿盘符。
        if (text.empty()) {
            std::vector<DirEntryInfo> entries;
#ifdef _WIN32
            if (!list_drives(entries)) {
                reply_error(res, 500, "fs_error", "无法枚举磁盘");
                return;
            }
#else
            DirEntryInfo root;
            root.name = "/";
            root.path = "/";
            root.is_dir = true;
            entries.push_back(root);
#endif
            nlohmann::json list = nlohmann::json::array();
            for (const DirEntryInfo& entry : entries) {
                list.push_back(dir_entry_json(entry));
            }
            reply_json(res, 200, {{"path", ""}, {"entries", list}});
            return;
        }

        fs::path dir;
        try {
            dir = path_from_u8(text);
        } catch (const std::exception&) {
            reply_error(res, 400, "invalid_path",
                        "路径不合法（需要 UTF-8 绝对路径）");
            return;
        }
        if (!dir.is_absolute()) {
            reply_error(res, 400, "invalid_path",
                        "path 必须是绝对路径：" + text);
            return;
        }

        std::error_code ec;
        if (!fs::exists(dir, ec)) {
            reply_error(res, 404, "path_not_found", "路径不存在：" + text);
            return;
        }
        if (!fs::is_directory(dir, ec)) {
            reply_error(res, 400, "not_directory", "不是目录：" + text);
            return;
        }

        std::vector<DirEntryInfo> entries;
        std::string error;
        try {
            if (!list_directory(dir, entries, error)) {
                reply_error(res, 500, "fs_error", error);
                return;
            }
        } catch (const std::exception& e) {
            reply_error(res, 500, "fs_error",
                        std::string("读取目录失败：") + e.what());
            return;
        }

        nlohmann::json list = nlohmann::json::array();
        for (const DirEntryInfo& entry : entries) {
            list.push_back(dir_entry_json(entry));
        }
        reply_json(res, 200, {{"path", path_to_u8(dir)}, {"entries", list}});
    });

    // ---- 备份（异步任务） ------------------------------------------------
    //
    // 字段与语义刻意和 CLI 对齐（cli/main.cpp 的 backup 分支）：
    // 指定了 pack 时 dst 是归档文件路径，否则 dst 是目标目录。
    // 这里只做参数校验 + 入队；真正的 pack_directory / mirror_directory
    // 在 TaskManager 的 worker 线程里跑，HTTP 立刻返回 task_id。
    svr.Post("/api/backup",
             [tasks](const httplib::Request& req, httplib::Response& res) {
                 std::string src, dst;
                 if (!require_param(req, "src", src, res)) return;
                 if (!require_param(req, "dst", dst, res)) return;

                 const std::string pack = param(req, "pack");
                 const bool archive_mode = !pack.empty();
                 if (archive_mode && pack != "vbak") {
                     if (!pack_from_string(pack)) {
                         reply_error(res, 400, "unsupported_pack",
                                     "不支持的打包格式：" + pack +
                                         "（当前仅支持 vbak）");
                     } else {
                         reply_error(res, 400, "unsupported_pack",
                                     "打包格式尚未实现：" + pack +
                                         "（当前仅支持 vbak）");
                     }
                     return;
                 }

                 const std::string compress_text = param(req, "compress");
                 const std::string level_text = param(req, "level");
                 const std::string encrypt_text = param(req, "encrypt");
                 // 密码只留在内存里交给 core；不进日志、不进错误信息、不回显。
                 const std::string password = param(req, "password");
                 const std::string filter_text = param(req, "filter");

                 bool keep_metadata = true;
                 if (!parse_bool_param(req, "metadata", true, keep_metadata,
                                       res)) {
                     return;
                 }

                 BackupTaskParams params;
                 params.src = src;
                 params.dst = dst;
                 params.keep_metadata = keep_metadata;

                 // filter 两种模式都支持：core 的 pack_directory 和
                 // mirror_directory 都接受 Filter（镜像模式额外把元数据写进
                 // `.backup-manifest.json`）。规则在 HTTP 线程先解析，非法就
                 // 立刻 400，不让它变成后台任务的失败。
                 if (!filter_text.empty()) {
                     std::string filter_error;
                     auto filter =
                         Filter::from_json(filter_text, filter_error);
                     if (!filter) {
                         reply_error(res, 400, "invalid_filter",
                                     "筛选规则无效：" + filter_error);
                         return;
                     }
                     params.filter = std::move(*filter);
                 }

                 if (!archive_mode) {
                     // 镜像模式只有「目录复制」这一种落盘方式，压缩/加密
                     // 是单容器格式的选项，传了说明用户以为会生效 —— 直接
                     // 报错，不悄悄忽略。
                     const auto unsupported = unsupported_params(
                         req, {"compress", "level", "encrypt"});
                     if (!unsupported.empty()) {
                         reply_error(
                             res, 400, "unsupported_in_mirror",
                             "镜像备份模式不支持参数：" +
                                 join_names(unsupported) +
                                 "。压缩/加密需要勾选"
                                 "「打包为 .vbak」单容器模式。");
                         return;
                     }
                     params.archive_mode = false;
                 } else {
                     params.archive_mode = true;

                     if (!compress_text.empty()) {
                         const auto id = compression_from_string(compress_text);
                         if (!id) {
                             reply_error(res, 400, "unsupported_compress",
                                         "不支持的压缩算法：" + compress_text);
                             return;
                         }
                         if (*id != CompressionId::None &&
                             make_compressor(*id) == nullptr) {
                             reply_error(
                                 res, 400, "unsupported_compress",
                                 "压缩算法在当前构建中不可用：" + compress_text);
                             return;
                         }
                         params.compression = *id;
                     }

                     const LevelRange range =
                         compression_level_range(params.compression);
                     if (params.compression == CompressionId::None) {
                         if (!level_text.empty()) {
                             reply_error(
                                 res, 400, "invalid_param",
                                 "指定压缩级别时必须同时选择压缩算法");
                             return;
                         }
                     } else if (range.has_level && !level_text.empty()) {
                         int level = 0;
                         if (!parse_int(level_text, level)) {
                             reply_error(res, 400, "invalid_param",
                                         "无效的压缩级别：" + level_text);
                             return;
                         }
                         if (level < range.min_level ||
                             level > range.max_level) {
                             reply_error(
                                 res, 400, "invalid_param",
                                 "压缩级别超出范围（" +
                                     std::string(to_string(params.compression)) +
                                     " 支持 " +
                                     std::to_string(range.min_level) + "-" +
                                     std::to_string(range.max_level) + "）");
                             return;
                         }
                         params.compression_level = level;
                     }
                     // 没有级别的算法（lz4/rle/lzw）忽略 level：core 的
                     // clamp_compression_level 本来也会忽略它。

                     if (!encrypt_text.empty()) {
                         const auto id = encryption_from_string(encrypt_text);
                         if (!id) {
                             reply_error(res, 400, "unsupported_encrypt",
                                         "不支持的加密算法：" + encrypt_text);
                             return;
                         }
                         if (*id != EncryptionId::None &&
                             make_cipher(*id) == nullptr) {
                             reply_error(
                                 res, 400, "unsupported_encrypt",
                                 "加密算法在当前构建中不可用：" + encrypt_text);
                             return;
                         }
                         params.encryption = *id;
                         if (params.encryption != EncryptionId::None &&
                             password.empty()) {
                             reply_error(res, 400, "missing_password",
                                         "选择加密算法后必须提供密码");
                             return;
                         }
                         params.password = password;
                     }
                 }

                 const std::string task_id = tasks->submit(
                     "backup",
                     archive_mode ? "正在打包…" : "正在镜像备份…",
                     make_backup_body(std::move(params)));
                 reply_json(res, 200, {{"task_id", task_id}});
             });

    // ---- 还原（异步任务） ------------------------------------------------
    //
    // 归档还原：压缩/加密算法由归档头自动识别，所以 compress/encrypt/level
    // 这些备份侧参数出现在还原请求里是误用，直接 400 说清楚；password 只
    // 用于加密归档。
    // 镜像还原：src 是目录，走 restore_mirror（读 manifest 还原元数据）；
    // password/filter 无意义，password 忽略、filter 按「不适用」报错。
    svr.Post("/api/restore",
             [tasks](const httplib::Request& req, httplib::Response& res) {
                 std::string dst;
                 if (!require_param(req, "dst", dst, res)) return;

                 const std::string archive_text = param(req, "archive");
                 const bool from_archive = !archive_text.empty();

                 std::string src;
                 if (from_archive) {
                     src = archive_text;
                 } else if (!require_param(req, "src", src, res)) {
                     return;  // 既没给 archive 也没给 src
                 }

                 const std::string password = param(req, "password");
                 bool restore_metadata = true;
                 if (!parse_bool_param(req, "metadata", true,
                                       restore_metadata, res)) {
                     return;
                 }

                 const auto unsupported = unsupported_params(
                     req, {"compress", "level", "encrypt", "filter"});
                 if (!unsupported.empty()) {
                     reply_error(res, 400, "unsupported_param",
                                 "还原不需要参数：" + join_names(unsupported) +
                                     "。压缩/加密算法由归档头自动识别，"
                                     "筛选规则只对备份生效。");
                     return;
                 }

                 RestoreTaskParams params;
                 params.from_archive = from_archive;
                 params.src = src;
                 params.dst = dst;
                 params.restore_metadata = restore_metadata;
                 if (from_archive) {
                     // 加密归档需要密码；明文归档会忽略它。绝不回显密码。
                     params.password = password;
                 }

                 const std::string task_id = tasks->submit(
                     "restore",
                     from_archive ? "正在解包…" : "正在还原镜像…",
                     make_restore_body(std::move(params)));
                 reply_json(res, 200, {{"task_id", task_id}});
             });
}

/// 覆盖 httplib 默认的 socket 选项。
///
/// httplib 默认设的是 SO_REUSEADDR（Windows 上没有 SO_REUSEPORT，
/// 见 httplib.h 的 default_socket_options）。但 **Windows 的 SO_REUSEADDR
/// 语义与 Unix 相反**：它允许第二个 socket 绑到同一个地址端口，
/// 且连接最终归谁收是未定义的。
///
/// 不覆盖的后果很隐蔽：第二个实例的 bind 会「成功」，于是
/// 「8080 被占用就退到 8081」这个探测永远探不出占用 ——
/// 程序打印 8080 并以为自己起来了，实际请求全被第一个实例接走。
/// 失败得毫无提示，比直接报错难查得多。
///
/// Windows 上表达「这个端口归我独占」的选项是 SO_EXCLUSIVEADDRUSE，
/// 设了它之后重复 bind 会返回失败，端口回退才真正生效。
// 注意是全局作用域的 socket_t（httplib.h 里的 using socket_t = SOCKET），
// 不在 httplib 命名空间内 —— 写 httplib::socket_t 编不过。
void server_socket_options(socket_t sock) {
    const int yes = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
}

/// 用 Chromium 的「应用窗口」模式打开 url，成功返回 true。
///
/// `--app=` 是 Chromium 系通用开关：开一个**没有标签页、没有地址栏**的窗口，
/// 关上就完事，视觉上就是个独立软件，而不是「又一个浏览器标签页」。
///
/// exe 只写文件名、不写全路径：Edge / Chrome 安装时都会在注册表
///   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\<exe>.exe
/// 登记自己，ShellExecuteW 据此解析出完整路径。硬编码
/// "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" 反而会在
/// 非默认安装位置上翻车。
bool open_in_app_mode(const wchar_t* exe, const std::wstring& url) {
    // --no-first-run / --no-default-browser-check 是给「这台机器从没启动过
    // 这个浏览器」兜底的。少了它们，全新环境下会先弹欢迎向导或
    // 「把 Edge 设为默认浏览器？」把我们的界面顶掉 —— 演示当天很难看。
    const std::wstring args = L"--app=" + url +
                              L" --window-size=920,620"
                              L" --no-first-run --no-default-browser-check";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe, args.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    // ShellExecuteW 的返回值是「伪装成 HINSTANCE 的错误码」：
    // **> 32 才算成功**，<= 32 是 SE_ERR_* 错误码。
    // 所以绝不能写 if (r) —— 那是个经典陷阱，失败时它照样非零。
    return reinterpret_cast<INT_PTR>(r) > 32;
}

void open_browser(int port) {
#ifdef _WIN32
    // URL 必须是宽字符：ShellExecuteW 收窄串时用的是 ANSI 代码页。
    const std::wstring url =
        L"http://127.0.0.1:" + std::to_wstring(port) + L"/";

    // Edge 在 Win11 上是自带的，排第一；Chrome 有就也能用。
    if (open_in_app_mode(L"msedge.exe", url)) return;
    if (open_in_app_mode(L"chrome.exe", url)) return;

    // 兜底：一个 Chromium 浏览器都没有（LTSC 版、被企业策略移除、用户自己
    // 卸了）就退回默认浏览器 —— 顶多多几个标签页，总比点了没反应强。
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
#else
    const std::string cmd = "xdg-open http://127.0.0.1:" +
                            std::to_string(port) + "/ >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

}  // namespace

int run_web_server(const WebOptions& opts) {
    // 每个端口都要 new 一个新的 Server 对象。
    //
    // bind_to_port 失败会把对象标记成 is_decommissioned（httplib.h:13032），
    // 之后 bind_internal 直接返回 -1 不再尝试（httplib.h:13624）——
    // 也就是说**同一个对象重试永远不会成功**。
    // 这是「先试 8080，被占用就试 8081」这种代码最经典的写法错误。
    std::unique_ptr<httplib::Server> svr;
    int chosen = 0;
    constexpr int kPortTries = 20;

    for (int p = opts.port; p < opts.port + kPortTries; ++p) {
        auto candidate = std::make_unique<httplib::Server>();
        // 必须在 bind_to_port 之前设置：httplib 在创建 socket 时应用这些
        // 选项，然后才 bind（见 httplib.h 里 create_server_socket 的顺序）。
        candidate->set_socket_options(server_socket_options);
        // 绑 127.0.0.1 而不是 0.0.0.0：既避开 Windows 防火墙弹窗
        // （演示时被点「取消」就尴尬了），也不把服务暴露到局域网。
        if (candidate->bind_to_port("127.0.0.1", p)) {
            svr = std::move(candidate);
            chosen = p;
            break;
        }
    }

    if (!svr) {
        std::cerr << "端口 " << opts.port << "-" << (opts.port + kPortTries - 1)
                  << " 全部被占用，无法启动。\n";
        return 1;
    }

    // 异步任务管理器。所有备份/还原都入队到一个后台 worker 里串行执行，
    // 避免两个请求同时往一个目标目录写文件。
    //
    // shutting_down 是给 SSE 连接用的：关窗口时置位，正在轮询任务状态的
    // SSE 线程才会尽快收尾，不会被线程池的 join 卡住几百毫秒。
    const auto tasks = std::make_shared<TaskManager>();
    const auto shutting_down = std::make_shared<std::atomic<bool>>(false);

    // bind 之后、listen 之前注册路由是安全的：线程池还没起来。
    register_routes(*svr, tasks, shutting_down);

    const std::wstring url =
        L"http://127.0.0.1:" + std::to_wstring(chosen) + L"/";
    std::cout << "FileBackup 已启动：http://127.0.0.1:" << chosen << "/\n";
    std::cout.flush();

    // 服务放后台线程，主线程留给窗口的消息循环 —— WebView2 的回调全靠
    // 消息派发，主线程被 listen 占住的话它永远初始化不完。
    //
    // 这里不需要再操心「窗口比服务先就绪」的竞态：bind_to_port 内部
    // 已经调用过 ::listen，socket 处于监听状态，早到的连接会先在
    // backlog 里排队。
    std::thread server_thread([&svr] { svr->listen_after_bind(); });

    if (opts.native_window) {
        const webview::ShowResult r =
            webview::show_url(url, L"FileBackup —— 文件备份");
        if (r == webview::ShowResult::Closed) {
            // 关窗口 = 退出程序。这正是原生窗口相对浏览器方案的价值：
            // 窗口就在本进程里，关掉它我们能顺手把服务停了；浏览器方案
            // 关掉标签页之后服务会一直挂着占住端口。
            shutting_down->store(true);  // 让 SSE 连接尽快收尾
            svr->stop();
            server_thread.join();
            std::cout << "窗口已关闭，服务停止。\n";
            return 0;
        }
        // Unavailable：这台机器没装 WebView2 运行时（LTSC、或被企业策略
        // 移除了 Edge），退回浏览器。窗口是隐藏创建的，所以用户不会看到
        // 一个白窗口闪一下再消失。
        std::cout << "（WebView2 不可用，改用浏览器打开）\n";
    }

    if (opts.open_browser) {
        open_browser(chosen);
    }
    std::cout << "按 Ctrl+C 退出。\n";
    std::cout.flush();

    server_thread.join();  // 阻塞到进程被终止
    return 0;
}

}  // namespace backup
