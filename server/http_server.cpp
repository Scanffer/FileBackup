#include "http_server.h"

// httplib.h 必须放在 <windows.h> 之前。
// 它会引入 <winsock2.h>，而 winsock2.h 一定要早于 windows.h 包含，
// 否则就是满屏的 fd_set / timeval / sockaddr 结构体重定义错误。
// （另：httplib.h 顶部有一个 _WIN32_WINNT < 0x0A00 就 #error 的检查，
//  本项目不受影响 —— MinGW 的 _mingw.h 默认定义为 0xa00。）
#include "httplib.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include "backup.h"
#include "fs_utils.h"
#include "pack.h"
#include "web_page.h"

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

/// 串行化所有碰文件系统的请求。
///
/// httplib 默认起 max(8, CPU 核数-1) 个线程处理请求，两个并发的备份写同一个
/// 目标目录会互相踩。这是个单用户本地工具，一把全局锁足够，
/// 不值得为它上更细的锁粒度。
std::mutex g_fs_mutex;

void reply_text(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "text/plain; charset=utf-8");
}

/// 取表单字段并校验「存在且非空」。校验失败时已经把 400 响应写好了。
///
/// 注意 req.get_param_value() 拿到的已经是**百分号解码后的原始字节**，
/// 中文路径在这里是完整的 UTF-8。但绝不能拿它直接构造
/// std::filesystem::path —— 必须过 backup::path_from_u8（见下）。
bool require_param(const httplib::Request& req, const char* key,
                   std::string& out, httplib::Response& res) {
    if (!req.has_param(key)) {
        reply_text(res, 400, std::string("缺少参数：") + key);
        return false;
    }
    out = req.get_param_value(key);
    if (out.empty()) {
        reply_text(res, 400, std::string("参数为空：") + key);
        return false;
    }
    return true;
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

void open_browser(int port) {
#ifdef _WIN32
    const std::wstring url =
        L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    // URL 必须是宽字符：ShellExecuteW 收窄串时用的是 ANSI 代码页。
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
#else
    const std::string cmd = "xdg-open http://127.0.0.1:" +
                            std::to_string(port) + "/ >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

std::string count_message(const char* verb, std::uintmax_t files,
                          std::uintmax_t bytes) {
    return std::string(verb) + "：" + std::to_string(files) + " 个文件，" +
           std::to_string(bytes) + " 字节。\n";
}

void register_routes(httplib::Server& svr) {
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        // 三参数版：省掉一次 std::string 构造，并去掉尾部 NUL。
        res.set_content(kIndexHtml, sizeof(kIndexHtml) - 1,
                        "text/html; charset=utf-8");
    });

    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        reply_text(res, 200, "ok");
    });

    // 字段与语义刻意和 CLI 对齐（cli/main.cpp 的 backup 分支）：
    // 指定了 pack 时 dst 是归档文件路径，否则 dst 是目标目录。
    // 这样 GUI 和 CLI 走的是同一批 backup:: 函数，行为不可能漂移。
    svr.Post("/api/backup",
             [](const httplib::Request& req, httplib::Response& res) {
                 std::string src, dst;
                 if (!require_param(req, "src", src, res)) return;
                 if (!require_param(req, "dst", dst, res)) return;

                 std::lock_guard<std::mutex> lock(g_fs_mutex);
                 try {
                     if (!req.get_param_value("pack").empty()) {
                         PackOptions o;
                         o.src = path_from_u8(src);
                         o.archive = path_from_u8(dst);
                         const PackResult r = pack_directory(o);
                         reply_text(res, 200,
                                    count_message("打包完成", r.entries_written,
                                                  r.bytes_written));
                     } else {
                         MirrorOptions o;
                         o.src = path_from_u8(src);
                         o.dst = path_from_u8(dst);
                         const MirrorResult r = mirror_directory(o);
                         std::string msg = count_message(
                             "备份完成", r.files_copied, r.bytes_copied);
                         if (r.files_skipped > 0) {
                             msg += "跳过 " +
                                    std::to_string(r.files_skipped) +
                                    " 个（已存在）。\n";
                         }
                         reply_text(res, 200, msg);
                     }
                 } catch (const std::exception& e) {
                     reply_text(res, 500, std::string("失败：") + e.what());
                 }
             });

    svr.Post("/api/restore",
             [](const httplib::Request& req, httplib::Response& res) {
                 std::string dst;
                 if (!require_param(req, "dst", dst, res)) return;

                 const bool from_archive = req.has_param("archive") &&
                                           !req.get_param_value("archive").empty();
                 std::string src;
                 if (from_archive) {
                     src = req.get_param_value("archive");
                 } else if (!require_param(req, "src", src, res)) {
                     return;  // 既没给 archive 也没给 src
                 }

                 std::lock_guard<std::mutex> lock(g_fs_mutex);
                 try {
                     if (from_archive) {
                         UnpackOptions o;
                         o.archive = path_from_u8(src);
                         o.dst = path_from_u8(dst);
                         const UnpackResult r = unpack_archive(o);
                         reply_text(res, 200,
                                    count_message("解包完成", r.entries_read,
                                                  r.bytes_read));
                     } else {
                         MirrorOptions o;
                         o.src = path_from_u8(src);
                         o.dst = path_from_u8(dst);
                         const MirrorResult r = mirror_directory(o);
                         reply_text(res, 200,
                                    count_message("还原完成", r.files_copied,
                                                  r.bytes_copied));
                     }
                 } catch (const std::exception& e) {
                     reply_text(res, 500, std::string("失败：") + e.what());
                 }
             });
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

    // bind 之后、listen 之前注册路由是安全的：线程池还没起来。
    register_routes(*svr);

    if (opts.open_browser) {
        // 用 start_handler 而不是另开一个线程去 sleep 等端口就绪。
        // 它在 accept 循环启动的瞬间、于 listen 线程内执行，此时 socket
        // 已经处于 listen 状态（bind_to_port 内部已调用 ::listen），
        // 浏览器的连接会先在 backlog 里排队，不会出现「连不上」。
        svr->set_start_handler([chosen] { open_browser(chosen); });
    }

    std::cout << "FileBackup 界面已启动：http://127.0.0.1:" << chosen << "/\n"
              << "按 Ctrl+C 退出。\n";
    std::cout.flush();

    svr->listen_after_bind();  // 阻塞
    return 0;
}

}  // namespace backup
