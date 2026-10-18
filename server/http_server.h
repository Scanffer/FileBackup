#pragma once

namespace backup {

/// Web GUI 的启动选项。
struct WebOptions {
    int port = 8080;  ///< 起始端口；被占用时自动往后试

    /// 优先用 WebView2 原生窗口显示界面（关掉窗口 = 退出程序）。
    /// 置 false 时直接用浏览器。--browser 会把它关掉。
    bool native_window = true;

    /// 允许「打开界面」这个动作。置 false 时只起服务不开任何窗口
    /// （--no-browser，用于 curl 调接口）。
    ///
    /// 注意两者是**独立**的：native_window 那步起不来时，只要
    /// open_browser 还是 true，就会退回浏览器。
    bool open_browser = true;
};

/// 启动 Web GUI，**阻塞**。
///
/// 两种结束方式：
///   原生窗口模式 —— 用户关掉窗口，函数返回，进程可以退出
///   浏览器模式   —— 一直服务到进程被终止（Ctrl+C）
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
