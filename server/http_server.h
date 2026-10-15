#pragma once

namespace backup {

/// Web GUI 的启动选项。
struct WebOptions {
    int port = 8080;         ///< 起始端口；被占用时自动往后试
    bool open_browser = true;  ///< 启动后是否自动打开默认浏览器
};

/// 启动 Web GUI 的 HTTP 服务，**阻塞**直到进程被终止（Ctrl+C）。
///
/// 返回建议的进程退出码：0 正常结束，1 端口全被占用、服务起不来。
///
/// ---- 为什么这个头文件里看不到 httplib ----
/// httplib.h 会拉进 <winsock2.h>，而 <winsock2.h> 必须早于 <windows.h>
/// 包含，顺序一乱就是满屏的结构体重定义错误。让调用方（cli/main.cpp）
/// 完全不接触 httplib 类型，这个顺序陷阱就根本不可能发生。
/// 同理，-municode 要求的 wmain 也留在 cli 侧，server 不掺和入口的事。
int run_web_server(const WebOptions& opts);

}  // namespace backup
