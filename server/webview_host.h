// 原生窗口 —— 用 WebView2 把界面显示在一个真正的 Win32 窗口里，
// 而不是丢给浏览器开一个标签页。
//
// 这个头文件**刻意不暴露任何 WebView2 类型**（和 http_server.h 一个思路）：
// WebView2.h 有 2.95 MB / 约 3 万行，还会把 winsock2.h、rpc.h 一堆东西拉进来，
// 让 cli/ 去 include 它纯属自找麻烦。调用方只需要知道下面这两个东西。

#pragma once

#include <string>

namespace backup::webview {

/// 窗口是怎么结束的。
enum class ShowResult {
    /// 窗口开出来了，用户把它关掉了。这时候进程也该退出。
    Closed,

    /// 压根没开起来 —— WebView2 运行时没装，或者 exe 里没嵌 loader。
    /// 调用方应当退回浏览器方案。
    ///
    /// 注意这种情况下窗口**不会闪现**：窗口是先隐藏创建、等 WebView2
    /// 初始化成功才 ShowWindow 的。
    Unavailable,
};

/// 开一个原生窗口显示 url，阻塞直到窗口关闭。
///
/// 必须在**主线程**调用，且调用线程需要是 STA（函数内部会自己
/// CoInitializeEx，重复调用是安全的）。
ShowResult show_url(const std::wstring& url, const std::wstring& title);

}  // namespace backup::webview
